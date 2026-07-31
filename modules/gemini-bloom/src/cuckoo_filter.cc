#include "cuckoo_filter.h"
#include "murmur2.h"
#include "rm_alloc.h"

#include <algorithm>
#include <climits>
#include <cstring>

// --- Fingerprint / candidate-bucket derivation ---
// Independent clean-room implementation of the partial-key cuckoo hashing
// scheme described in Fan et al., "Cuckoo Filter: Practically Better Than
// Bloom" (CoNEXT 2014): an item's alternate bucket is derivable from its
// fingerprint and *either* candidate bucket, without needing to re-hash the
// original item. Seeds are arbitrary constants distinct from the Bloom
// filter's murmur2 seeds (this is a private wire format — no compatibility
// requirement with any external filter's on-disk layout).
namespace {

constexpr uint32_t kCfFpSeed = 0xd2f4e5c1u;
constexpr uint32_t kCfIndexSeed = 0x5bd1e995u;
constexpr uint32_t kCfAltSeed = 0x9747b28cu;

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
  // Masking the fingerprint's hash *before* XOR-ing (rather than masking
  // the XOR result) is what makes this involutive: since `index` is
  // already < numBuckets, XOR-ing it with a value that is also masked to
  // < numBuckets keeps the result < numBuckets, and applying the same XOR
  // twice recovers the original index — CfAltIndex(CfAltIndex(i, fp, n),
  // fp, n) == i for any i < n.
  uint64_t mask = numBuckets - 1;
  uint32_t h = MurmurHash2(&fp, sizeof(fp), kCfAltSeed);
  return index ^ (static_cast<uint64_t>(h) & mask);
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
  filter.buckets_ = static_cast<uint8_t*>(RMCalloc(dataSize, 1));
  if (!filter.buckets_) return std::nullopt;

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

// --- Slot lookup helpers ---

int CuckooFilter::FindSlot(uint64_t bucketIndex, uint8_t fp) const {
  const uint8_t* slots = SlotsOf(bucketIndex);
  for (uint32_t i = 0; i < bucketSize_; i++) {
    if (slots[i] == fp) return static_cast<int>(i);
  }
  return -1;
}

int CuckooFilter::FindEmptySlot(uint64_t bucketIndex) const {
  return FindSlot(bucketIndex, 0);
}

// --- Membership operations ---

std::optional<bool> CuckooFilter::Insert(std::span<const std::byte> item) {
  uint8_t fp = CfFingerprint(item);
  uint64_t i1 = CfIndexOf(item, numBuckets_);
  uint64_t i2 = CfAltIndex(i1, fp, numBuckets_);

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

  // Kick loop: relocate an existing fingerprint to its alternate bucket,
  // repeating until a free slot is found or maxIterations_ is exhausted. If
  // the loop exhausts without success, the still-traveling fingerprint has
  // nowhere to land, so every displacement made during this call must be
  // rolled back — otherwise that fingerprint is silently dropped, producing
  // a false negative for whichever item it belonged to. `history` records
  // each (bucket, slot, previousFingerprint) write so a failed insert can
  // restore the table to its pre-call state.
  uint64_t state = i1 ^ (static_cast<uint64_t>(fp) << 32) ^ (i2 << 16);
  uint64_t curIndex = (state & 1) ? i2 : i1;
  uint8_t curFp = fp;

  struct KickEntry {
    uint64_t index;
    uint32_t slot;
    uint8_t oldFp;
  };
  auto* history = static_cast<KickEntry*>(RMAlloc(sizeof(KickEntry) * maxIterations_));
  if (!history) return std::nullopt;
  uint32_t numKicks = 0;

  for (uint32_t iter = 0; iter < maxIterations_; iter++) {
    state = Splitmix64(state + iter);
    uint32_t victimSlot = static_cast<uint32_t>(state % bucketSize_);
    uint8_t* slots = SlotsOf(curIndex);
    uint8_t evicted = slots[victimSlot];
    history[numKicks++] = {curIndex, victimSlot, evicted};
    slots[victimSlot] = curFp;
    curFp = evicted;
    curIndex = CfAltIndex(curIndex, curFp, numBuckets_);

    if (int slot = FindEmptySlot(curIndex); slot >= 0) {
      SlotsOf(curIndex)[slot] = curFp;
      numItems_++;
      RMFree(history);
      return true;
    }
  }

  for (uint32_t i = numKicks; i-- > 0;) {
    SlotsOf(history[i].index)[history[i].slot] = history[i].oldFp;
  }
  RMFree(history);
  return std::nullopt;
}

bool CuckooFilter::Contains(std::span<const std::byte> item) const {
  uint8_t fp = CfFingerprint(item);
  uint64_t i1 = CfIndexOf(item, numBuckets_);
  uint64_t i2 = CfAltIndex(i1, fp, numBuckets_);
  return FindSlot(i1, fp) >= 0 || FindSlot(i2, fp) >= 0;
}

bool CuckooFilter::Delete(std::span<const std::byte> item) {
  uint8_t fp = CfFingerprint(item);
  uint64_t i1 = CfIndexOf(item, numBuckets_);
  uint64_t i2 = CfAltIndex(i1, fp, numBuckets_);

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
  for (uint64_t i = 0; i < dataSize; i++) {
    if (buckets_[i] != 0) count++;
  }
  numItems_ = count;
}

uint64_t CuckooFilter::Count(std::span<const std::byte> item) const {
  uint8_t fp = CfFingerprint(item);
  uint64_t i1 = CfIndexOf(item, numBuckets_);
  uint64_t i2 = CfAltIndex(i1, fp, numBuckets_);

  const uint8_t* s1 = SlotsOf(i1);
  uint64_t count = 0;
  for (uint32_t i = 0; i < bucketSize_; i++) {
    if (s1[i] == fp) count++;
  }
  if (i2 != i1) {
    const uint8_t* s2 = SlotsOf(i2);
    for (uint32_t i = 0; i < bucketSize_; i++) {
      if (s2[i] == fp) count++;
    }
  }
  return count;
}
