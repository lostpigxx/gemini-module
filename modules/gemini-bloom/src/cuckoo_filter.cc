#include "cuckoo_filter.h"
#include "murmur2.h"
#include "rm_alloc.h"

#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <cstring>

namespace {

constexpr uint32_t kCfFpSeed = 0xd2f4e5c1u;
constexpr uint32_t kCfIndexSeed = 0x5bd1e995u;
constexpr uint32_t kCfAltSeed = 0x9747b28cu;

// Compile-time 1-byte MurmurHash2 matching MurmurHash2(&fp, 1, kCfAltSeed)
// bit-for-bit, eliminating a hash function call on every lookup and kick.
constexpr uint32_t Murmur2Byte(uint8_t b, uint32_t seed) {
  constexpr uint32_t m = 0x5bd1e995u;
  uint32_t h = (seed ^ 1u) ^ static_cast<uint32_t>(b);
  h *= m;
  h ^= h >> 13;
  h *= m;
  h ^= h >> 15;
  return h;
}

constexpr std::array<uint32_t, 256> BuildAltHashLut() {
  std::array<uint32_t, 256> lut{};
  for (size_t i = 0; i < 256; i++) {
    lut[i] = Murmur2Byte(static_cast<uint8_t>(i), kCfAltSeed);
  }
  return lut;
}

constexpr std::array<uint32_t, 256> kCfAltHashLut = BuildAltHashLut();

// splitmix64 — a fast, stateless bit-mixer used here purely to pick a
// pseudo-random victim bucket/slot during the kick loop. Using a
// deterministic mix of runtime values (rather than <random>'s global
// engine state) keeps CuckooFilter free of hidden mutable global state, so
// behavior is reproducible given the same sequence of operations.
uint64_t Splitmix64(uint64_t x) {
  x += 0x9E3779B97F4A7C15ULL;
  uint64_t z = x;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

}  // namespace

CfItemHash CfHashItem(std::span<const std::byte> item) {
  auto* ptr = reinterpret_cast<const void*>(item.data());
  auto len = static_cast<int>(std::min(item.size(), static_cast<size_t>(INT_MAX)));
  uint32_t hFp = MurmurHash2(ptr, len, kCfFpSeed);
  uint8_t fp = static_cast<uint8_t>(hFp & 0xFFu);
  if (fp == 0) fp = 1;
  uint32_t rawIdx = MurmurHash2(ptr, len, kCfIndexSeed);
  return {fp, rawIdx};
}

uint8_t CfFingerprint(std::span<const std::byte> item) {
  auto* ptr = reinterpret_cast<const void*>(item.data());
  auto len = static_cast<int>(std::min(item.size(), static_cast<size_t>(INT_MAX)));
  uint32_t h = MurmurHash2(ptr, len, kCfFpSeed);
  uint8_t fp = static_cast<uint8_t>(h & 0xFFu);
  return fp == 0 ? 1 : fp;
}

uint64_t CfIndexOf(std::span<const std::byte> item, uint64_t numBuckets) {
  auto* ptr = reinterpret_cast<const void*>(item.data());
  auto len = static_cast<int>(std::min(item.size(), static_cast<size_t>(INT_MAX)));
  uint32_t h = MurmurHash2(ptr, len, kCfIndexSeed);
  return static_cast<uint64_t>(h) & (numBuckets - 1);
}

uint64_t CfAltIndex(uint64_t index, uint8_t fp, uint64_t numBuckets) {
  uint64_t mask = numBuckets - 1;
  return index ^ (static_cast<uint64_t>(kCfAltHashLut[fp]) & mask);
}

// --- CuckooFilter lifecycle ---

CuckooFilter::~CuckooFilter() {
  if (buckets_) {
    RMFree(buckets_);
    buckets_ = nullptr;
  }
}

CuckooFilter::CuckooFilter(CuckooFilter&& other) noexcept
    : numBuckets_(other.numBuckets_),
      bucketSize_(other.bucketSize_),
      maxIterations_(other.maxIterations_),
      numItems_(other.numItems_),
      buckets_(other.buckets_) {
  other.buckets_ = nullptr;
}

CuckooFilter& CuckooFilter::operator=(CuckooFilter&& other) noexcept {
  if (this != &other) {
    if (buckets_) RMFree(buckets_);
    numBuckets_ = other.numBuckets_;
    bucketSize_ = other.bucketSize_;
    maxIterations_ = other.maxIterations_;
    numItems_ = other.numItems_;
    buckets_ = other.buckets_;
    other.buckets_ = nullptr;
  }
  return *this;
}

std::optional<CuckooFilter> CuckooFilter::Create(uint64_t numBuckets, uint32_t bucketSize,
                                                  uint32_t maxIterations) {
  if (numBuckets == 0 || (numBuckets & (numBuckets - 1)) != 0) return std::nullopt;
  if (numBuckets > kCfMaxBuckets) return std::nullopt;
  if (bucketSize == 0 || bucketSize > kCfMaxBucketSize) return std::nullopt;
  if (maxIterations == 0 || maxIterations > kCfMaxIterations) return std::nullopt;

  CuckooFilter filter;
  filter.numBuckets_ = numBuckets;
  filter.bucketSize_ = bucketSize;
  filter.maxIterations_ = maxIterations;

  uint64_t dataSize = numBuckets * static_cast<uint64_t>(bucketSize);
  if (dataSize > kCfMaxLayerDataSize) return std::nullopt;
  filter.buckets_ = static_cast<uint8_t*>(RMCalloc(dataSize, 1));
  if (!filter.buckets_) return std::nullopt;

  return filter;
}

std::optional<CuckooFilter> CuckooFilter::FromLoadedBuffer(uint64_t numBuckets, uint32_t bucketSize,
                                                           uint32_t maxIterations, uint8_t* adoptedBuf) {
  if (!adoptedBuf) return std::nullopt;
  if (numBuckets == 0 || (numBuckets & (numBuckets - 1)) != 0) return std::nullopt;
  if (numBuckets > kCfMaxBuckets) return std::nullopt;
  if (bucketSize == 0 || bucketSize > kCfMaxBucketSize) return std::nullopt;
  if (maxIterations == 0 || maxIterations > kCfMaxIterations) return std::nullopt;
  if (numBuckets * static_cast<uint64_t>(bucketSize) > kCfMaxLayerDataSize) return std::nullopt;

  CuckooFilter filter;
  filter.numBuckets_ = numBuckets;
  filter.bucketSize_ = bucketSize;
  filter.maxIterations_ = maxIterations;
  filter.buckets_ = adoptedBuf;
  filter.RecountItems();
  return filter;
}

std::optional<CuckooFilter> CuckooFilter::Clone() const {
  CuckooFilter copy;
  copy.numBuckets_ = numBuckets_;
  copy.bucketSize_ = bucketSize_;
  copy.maxIterations_ = maxIterations_;
  copy.numItems_ = numItems_;

  uint64_t dataSize = GetDataSize();
  copy.buckets_ = static_cast<uint8_t*>(RMAlloc(dataSize));
  if (!copy.buckets_) return std::nullopt;
  std::memcpy(copy.buckets_, buckets_, dataSize);
  return copy;
}

// --- Slot lookup helpers (SWAR-accelerated for common bucket sizes) ---

int CuckooFilter::FindSlot(uint64_t bucketIndex, uint8_t fp) const {
  const uint8_t* slots = SlotsOf(bucketIndex);
  uint32_t bs = bucketSize_;

  if (bs == 2) {
    if (slots[0] == fp) return 0;
    if (slots[1] == fp) return 1;
    return -1;
  }

#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
  if (bs == 4) {
    uint32_t word;
    std::memcpy(&word, slots, 4);
    uint32_t x = word ^ (static_cast<uint32_t>(fp) * 0x01010101u);
    uint32_t zeroMask = (x - 0x01010101u) & ~x & 0x80808080u;
    if (!zeroMask) return -1;
    return static_cast<int>(std::countr_zero(zeroMask) >> 3);
  }
  uint32_t i = 0;
  if (bs >= 8) {
    uint64_t pat = static_cast<uint64_t>(fp) * 0x0101010101010101ULL;
    for (; i + 8 <= bs; i += 8) {
      uint64_t word;
      std::memcpy(&word, slots + i, 8);
      uint64_t x = word ^ pat;
      uint64_t zeroMask = (x - 0x0101010101010101ULL) & ~x & 0x8080808080808080ULL;
      if (zeroMask) return static_cast<int>(i + (std::countr_zero(zeroMask) >> 3));
    }
  }
  for (; i < bs; i++) {
    if (slots[i] == fp) return static_cast<int>(i);
  }
  return -1;
#else
  for (uint32_t i = 0; i < bs; i++) {
    if (slots[i] == fp) return static_cast<int>(i);
  }
  return -1;
#endif
}

int CuckooFilter::FindEmptySlot(uint64_t bucketIndex) const {
  return FindSlot(bucketIndex, 0);
}

// --- Membership operations ---

std::optional<bool> CuckooFilter::Insert(std::span<const std::byte> item) {
  return Insert(CfHashItem(item));
}

std::optional<bool> CuckooFilter::Insert(CfItemHash h) {
  uint8_t fp = h.fp;
  uint64_t mask = numBuckets_ - 1;
  uint64_t i1 = static_cast<uint64_t>(h.rawIndex) & mask;
  uint64_t i2 = i1 ^ (static_cast<uint64_t>(kCfAltHashLut[fp]) & mask);

  if (int slot = FindEmptySlot(i1); slot >= 0) {
    SlotsOf(i1)[slot] = fp;
    numItems_++;
    return true;
  }
  if (int slot = FindEmptySlot(i2); slot >= 0) {
    SlotsOf(i2)[slot] = fp;
    numItems_++;
    return true;
  }

  uint64_t state = i1 ^ (static_cast<uint64_t>(fp) << 32) ^ (i2 << 16);
  uint64_t curIndex = (state & 1) ? i2 : i1;
  uint8_t curFp = fp;

  struct KickEntry {
    uint64_t index;
    uint32_t slot;
    uint8_t oldFp;
  };
  constexpr uint32_t kInlineHistory = 64;
  KickEntry stackHistory[kInlineHistory];
  KickEntry* history = stackHistory;
  KickEntry* heapHistory = nullptr;
  uint32_t numKicks = 0;

  for (uint32_t iter = 0; iter < maxIterations_; iter++) {
    if (numKicks == kInlineHistory && !heapHistory) {
      heapHistory = static_cast<KickEntry*>(RMAlloc(sizeof(KickEntry) * maxIterations_));
      if (!heapHistory) {
        for (uint32_t i = numKicks; i-- > 0;) {
          SlotsOf(history[i].index)[history[i].slot] = history[i].oldFp;
        }
        return std::nullopt;
      }
      std::memcpy(heapHistory, stackHistory, sizeof(KickEntry) * numKicks);
      history = heapHistory;
    }

    state = Splitmix64(state + iter);
    uint32_t victimSlot = static_cast<uint32_t>(state % bucketSize_);
    uint8_t* slots = SlotsOf(curIndex);
    uint8_t evicted = slots[victimSlot];
    history[numKicks++] = {curIndex, victimSlot, evicted};
    slots[victimSlot] = curFp;
    curFp = evicted;
    curIndex = curIndex ^ (static_cast<uint64_t>(kCfAltHashLut[curFp]) & mask);

    if (int slot = FindEmptySlot(curIndex); slot >= 0) {
      SlotsOf(curIndex)[slot] = curFp;
      numItems_++;
      if (heapHistory) RMFree(heapHistory);
      return true;
    }
  }

  for (uint32_t i = numKicks; i-- > 0;) {
    SlotsOf(history[i].index)[history[i].slot] = history[i].oldFp;
  }
  if (heapHistory) RMFree(heapHistory);
  return std::nullopt;
}

bool CuckooFilter::Contains(std::span<const std::byte> item) const {
  return Contains(CfHashItem(item));
}

bool CuckooFilter::Contains(CfItemHash h) const {
  uint8_t fp = h.fp;
  uint64_t mask = numBuckets_ - 1;
  uint64_t i1 = static_cast<uint64_t>(h.rawIndex) & mask;
  uint64_t i2 = i1 ^ (static_cast<uint64_t>(kCfAltHashLut[fp]) & mask);
  return FindSlot(i1, fp) >= 0 || FindSlot(i2, fp) >= 0;
}

bool CuckooFilter::Delete(std::span<const std::byte> item) {
  return Delete(CfHashItem(item));
}

bool CuckooFilter::Delete(CfItemHash h) {
  uint8_t fp = h.fp;
  uint64_t mask = numBuckets_ - 1;
  uint64_t i1 = static_cast<uint64_t>(h.rawIndex) & mask;
  uint64_t i2 = i1 ^ (static_cast<uint64_t>(kCfAltHashLut[fp]) & mask);

  if (int slot = FindSlot(i1, fp); slot >= 0) {
    SlotsOf(i1)[slot] = 0;
    numItems_--;
    return true;
  }
  if (int slot = FindSlot(i2, fp); slot >= 0) {
    SlotsOf(i2)[slot] = 0;
    numItems_--;
    return true;
  }
  return false;
}

void CuckooFilter::RecountItems() {
  uint64_t count = 0;
  uint64_t dataSize = GetDataSize();
  uint64_t i = 0;
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
  for (; i + 8 <= dataSize; i += 8) {
    uint64_t word;
    std::memcpy(&word, buckets_ + i, 8);
    if (word == 0) continue;
    // Borrow-free per-byte non-zero detection: bit 7 of each lane is 1 iff lane != 0.
    uint64_t nonZero = (((word & 0x7f7f7f7f7f7f7f7fULL) + 0x7f7f7f7f7f7f7f7fULL) | word) & 0x8080808080808080ULL;
    count += static_cast<uint64_t>(std::popcount(nonZero));
  }
#endif
  for (; i < dataSize; i++) {
    if (buckets_[i] != 0) count++;
  }
  numItems_ = count;
}

uint64_t CuckooFilter::Count(std::span<const std::byte> item) const {
  return Count(CfHashItem(item));
}

uint64_t CuckooFilter::Count(CfItemHash h) const {
  uint8_t fp = h.fp;
  uint64_t mask = numBuckets_ - 1;
  uint64_t i1 = static_cast<uint64_t>(h.rawIndex) & mask;
  uint64_t i2 = i1 ^ (static_cast<uint64_t>(kCfAltHashLut[fp]) & mask);

  auto CountBucket = [this, fp](uint64_t bIdx) -> uint64_t {
    const uint8_t* s = SlotsOf(bIdx);
    uint64_t c = 0;
    uint32_t i = 0;
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    if (bucketSize_ >= 8) {
      uint64_t pat = static_cast<uint64_t>(fp) * 0x0101010101010101ULL;
      for (; i + 8 <= bucketSize_; i += 8) {
        uint64_t word;
        std::memcpy(&word, s + i, 8);
        uint64_t x = word ^ pat;
        uint64_t nonZero = (((x & 0x7f7f7f7f7f7f7f7fULL) + 0x7f7f7f7f7f7f7f7fULL) | x) & 0x8080808080808080ULL;
        c += 8u - static_cast<uint64_t>(std::popcount(nonZero));
      }
    }
#endif
    for (; i < bucketSize_; i++) {
      if (s[i] == fp) c++;
    }
    return c;
  };

  uint64_t count = CountBucket(i1);
  if (i2 != i1) count += CountBucket(i2);
  return count;
}
