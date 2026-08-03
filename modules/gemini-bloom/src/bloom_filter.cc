#include "bloom_filter.h"
#include "murmur2.h"
#include "rm_alloc.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

// --- Hash policies ---
// The double-hashing scheme follows Kirsch & Mitzenmacher (ESA 2006):
// compute two independent hashes h1, h2 and derive k probes as h1 + i*h2.
//
// Seed values and the "h2 = hash(data, seed=h1)" pattern are part of the
// wire-format protocol — they determine which bits are set in the persisted
// bit array. Any implementation that reads/writes the same RDB format MUST
// use these same seeds to avoid false negatives on deserialized filters.
//   32-bit seed: 0x9747b28c  (MurmurHash2 conventional default)
//   64-bit seed: 0xc6a4a7935bd1e995  (MurmurHash64A mixing constant m)

HashPair Hash32Policy::Compute(const void* data, size_t len) {
  int hashLen = static_cast<int>(std::min(len, static_cast<size_t>(INT_MAX)));
  uint32_t h1 = MurmurHash2(data, hashLen, 0x9747b28c);
  return {h1, MurmurHash2(data, hashLen, h1)};
}

HashPair Hash64Policy::Compute(const void* data, size_t len) {
  int hashLen = static_cast<int>(std::min(len, static_cast<size_t>(INT_MAX)));
  uint64_t h1 = MurmurHash64A(data, hashLen, 0xc6a4a7935bd1e995ULL);
  return {h1, MurmurHash64A(data, hashLen, h1)};
}

// --- BloomLayer lifecycle ---

BloomLayer::~BloomLayer() {
  if (bitArray_) {
    RMFree(bitArray_);
    bitArray_ = nullptr;
  }
}

BloomLayer::BloomLayer(BloomLayer&& other) noexcept
    : hashCount_(other.hashCount_),
      log2Bits_(other.log2Bits_),
      use64Bit_(other.use64Bit_),
      capacity_(other.capacity_),
      fpRate_(other.fpRate_),
      bitsPerEntry_(other.bitsPerEntry_),
      bitArray_(other.bitArray_),
      dataSize_(other.dataSize_),
      totalBits_(other.totalBits_) {
  other.bitArray_ = nullptr;
}

BloomLayer& BloomLayer::operator=(BloomLayer&& other) noexcept {
  if (this != &other) {
    if (bitArray_) RMFree(bitArray_);
    hashCount_ = other.hashCount_;
    log2Bits_ = other.log2Bits_;
    use64Bit_ = other.use64Bit_;
    capacity_ = other.capacity_;
    fpRate_ = other.fpRate_;
    bitsPerEntry_ = other.bitsPerEntry_;
    bitArray_ = other.bitArray_;
    dataSize_ = other.dataSize_;
    totalBits_ = other.totalBits_;
    other.bitArray_ = nullptr;
  }
  return *this;
}

// --- Optimal parameter computation ---
// Formulas from Mitzenmacher & Upfal, "Probability and Computing" (2005):
//   m/n = -log2(p) / ln(2)       (bits per entry)
//   k   = (m/n) * ln(2)           (optimal hash count)

static double OptimalBitsPerEntry(double fpRate) {
  constexpr double kLn2Squared = kLn2 * kLn2;
  return -std::log(fpRate) / kLn2Squared;
}

static uint32_t OptimalHashCount(double bitsPerEntry) {
  return std::max(1u,
    static_cast<uint32_t>(std::ceil(kLn2 * bitsPerEntry)));
}

bool BloomLayer::Create(uint64_t cap, double falsePositiveRate,
                        BloomFlags flags, BloomLayer* out) {
  if (!out) return false;
  if (cap == 0 || !std::isfinite(falsePositiveRate) ||
      falsePositiveRate <= 0.0 || falsePositiveRate >= 1.0) {
    return false;
  }

  BloomLayer layer;
  layer.capacity_ = cap;
  layer.fpRate_ = falsePositiveRate;
  layer.use64Bit_ = HasFlag(flags, BloomFlags::Use64Bit);

  if (HasFlag(flags, BloomFlags::RawBits)) {
    layer.bitsPerEntry_ = 0;
    layer.totalBits_ = cap;
    layer.hashCount_ = 0;
  } else {
    layer.bitsPerEntry_ = OptimalBitsPerEntry(falsePositiveRate);
    if (layer.bitsPerEntry_ > kMaxBitsPerEntry) return false;
    auto rawBits = static_cast<double>(cap) * layer.bitsPerEntry_;
    constexpr double kMaxBits = static_cast<double>(UINT64_MAX - 7);
    if (!std::isfinite(rawBits) || rawBits > kMaxBits) return false;
    layer.totalBits_ = static_cast<uint64_t>(std::max(rawBits, 1024.0));
    layer.hashCount_ = OptimalHashCount(layer.bitsPerEntry_);
  }

  if (!HasFlag(flags, BloomFlags::NoRound)) {
    if (layer.totalBits_ > (1ULL << 63)) return false;
    uint64_t roundedBits = 1;
    uint8_t log2Bits = 0;
    while (roundedBits < layer.totalBits_) {
      roundedBits <<= 1;
      ++log2Bits;
    }
    layer.totalBits_ = roundedBits;
    layer.log2Bits_ = log2Bits;
  }

  // Align bytes to 8-byte boundary, then set totalBits = bytes * 8.
  // This matches RedisBloom's bloom_init() alignment.
  uint64_t bytes = (layer.totalBits_ + 63) / 64 * 8;
  layer.totalBits_ = bytes * 8;
  layer.dataSize_ = bytes;
  if (layer.dataSize_ == 0) return false;

  if (layer.dataSize_ > kMaxLayerDataSize) return false;

  layer.bitArray_ = static_cast<uint8_t*>(RMCalloc(layer.dataSize_, 1));
  if (!layer.bitArray_) return false;

  *out = std::move(layer);
  return true;
}

bool BloomLayer::Clone(BloomLayer* out) const {
  if (!out) return false;
  BloomLayer copy;
  copy.hashCount_ = hashCount_;
  copy.log2Bits_ = log2Bits_;
  copy.use64Bit_ = use64Bit_;
  copy.capacity_ = capacity_;
  copy.fpRate_ = fpRate_;
  copy.bitsPerEntry_ = bitsPerEntry_;
  copy.totalBits_ = totalBits_;
  copy.dataSize_ = dataSize_;

  copy.bitArray_ = static_cast<uint8_t*>(RMAlloc(copy.dataSize_));
  if (!copy.bitArray_) return false;
  std::memcpy(copy.bitArray_, bitArray_, copy.dataSize_);
  *out = std::move(copy);
  return true;
}

// --- Bit-level operations ---

uint64_t BloomLayer::ComputeModuloMask() const {
  return (1ULL << log2Bits_) - 1;
}

bool BloomLayer::TestBit(uint64_t bitIndex) const {
  BitAddress addr = ResolveBit(bitIndex);
  return (bitArray_[addr.byteOffset] & addr.mask) != 0;
}

void BloomLayer::SetBit(uint64_t bitIndex) {
  BitAddress addr = ResolveBit(bitIndex);
  bitArray_[addr.byteOffset] |= addr.mask;
}

// --- Membership queries ---
// Uses Kirsch-Mitzenmacher enhanced double hashing to derive k probe
// positions from a single HashPair. Reference: Kirsch & Mitzenmacher,
// "Less Hashing, Same Performance" (ESA 2006).

bool BloomLayer::Test(const HashPair& hp) const {
  bool isPow2 = UseBitMasking();
  uint64_t mask = isPow2 ? ComputeModuloMask() : 0;

  for (uint32_t probe = 0; probe < hashCount_; probe++) {
    uint64_t pos = ProbePosition(hp, probe, mask, totalBits_, isPow2);
    if (!TestBit(pos)) return false;
  }
  return true;
}

bool BloomLayer::Insert(const HashPair& hp) {
  bool isPow2 = UseBitMasking();
  uint64_t mask = isPow2 ? ComputeModuloMask() : 0;
  bool anyNew = false;

  for (uint32_t probe = 0; probe < hashCount_; probe++) {
    uint64_t pos = ProbePosition(hp, probe, mask, totalBits_, isPow2);
    BitAddress addr = ResolveBit(pos);
    uint8_t old = bitArray_[addr.byteOffset];
    if ((old & addr.mask) == 0) {
      bitArray_[addr.byteOffset] = old | addr.mask;
      anyNew = true;
    }
  }
  return anyNew;
}
