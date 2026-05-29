#include "bloom_filter.h"
#include "murmur2.h"
#include "rm_alloc.h"

#include <algorithm>
#include <bit>
#include <climits>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>

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

HashPair Hash32Policy::Compute(std::span<const std::byte> data) {
  auto* ptr = reinterpret_cast<const void*>(data.data());
  size_t len = data.size();
  uint32_t h1 = MurmurHash2(ptr, len, 0x9747b28c);
  return {h1, MurmurHash2(ptr, len, h1)};
}

HashPair Hash64Policy::Compute(std::span<const std::byte> data) {
  auto* ptr = reinterpret_cast<const void*>(data.data());
  size_t len = data.size();
  uint64_t h1 = MurmurHash64A(ptr, len, 0xc6a4a7935bd1e995ULL);
  return {h1, MurmurHash64A(ptr, len, h1)};
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
  constexpr double kLn2Squared = std::numbers::ln2 * std::numbers::ln2;
  return -std::log(fpRate) / kLn2Squared;
}

static std::optional<uint32_t> OptimalHashCount(double bitsPerEntry) {
  double probes = std::ceil(std::numbers::ln2 * bitsPerEntry);
  if (!std::isfinite(probes) || probes < 1.0 ||
      probes > static_cast<double>(std::numeric_limits<uint32_t>::max())) {
    return std::nullopt;
  }
  return std::max(1u, static_cast<uint32_t>(probes));
}

std::optional<BloomLayer> BloomLayer::Create(uint64_t cap, double falsePositiveRate,
                                              BloomFlags flags) {
  if (cap == 0 ||
      cap > static_cast<uint64_t>(std::numeric_limits<long long>::max()) ||
      !std::isfinite(falsePositiveRate) ||
      falsePositiveRate <= 0.0 || falsePositiveRate >= 1.0 ||
      HasFlag(flags, BloomFlags::RawBits)) {
    return std::nullopt;
  }

  BloomLayer layer;
  layer.capacity_ = cap;
  layer.fpRate_ = falsePositiveRate;
  layer.use64Bit_ = HasFlag(flags, BloomFlags::Use64Bit);

  layer.bitsPerEntry_ = OptimalBitsPerEntry(falsePositiveRate);
  if (!std::isfinite(layer.bitsPerEntry_) || layer.bitsPerEntry_ <= 0.0) {
    return std::nullopt;
  }

  double rawBits = static_cast<double>(cap) * layer.bitsPerEntry_;
  double boundedBits = std::max(rawBits, 1024.0);
  if (!std::isfinite(boundedBits) ||
      static_cast<long double>(boundedBits) >
        static_cast<long double>(std::numeric_limits<uint64_t>::max() - 7)) {
    return std::nullopt;
  }
  layer.totalBits_ = static_cast<uint64_t>(boundedBits);

  auto hashCount = OptimalHashCount(layer.bitsPerEntry_);
  if (!hashCount) return std::nullopt;
  layer.hashCount_ = *hashCount;

  if (!HasFlag(flags, BloomFlags::NoRound)) {
    if (layer.totalBits_ > (1ULL << 63)) return std::nullopt;
    layer.totalBits_ = std::bit_ceil(layer.totalBits_);
    layer.log2Bits_ = static_cast<uint8_t>(std::bit_width(layer.totalBits_) - 1);
  }

  if (layer.totalBits_ > std::numeric_limits<uint64_t>::max() - 7) {
    return std::nullopt;
  }
  layer.dataSize_ = (layer.totalBits_ + 7) / 8;
  if (layer.dataSize_ == 0 ||
      layer.dataSize_ > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
    return std::nullopt;
  }

  layer.bitArray_ = static_cast<uint8_t*>(RMCalloc(static_cast<size_t>(layer.dataSize_), 1));
  if (!layer.bitArray_) return std::nullopt;

  return layer;
}

// --- Bit-level operations ---

uint64_t BloomLayer::ComputeModuloMask() const {
  return totalBits_ - 1;
}

bool BloomLayer::TestBit(uint64_t bitIndex) const {
  auto [byteOff, mask] = ResolveBit(bitIndex);
  return (bitArray_[byteOff] & mask) != 0;
}

void BloomLayer::SetBit(uint64_t bitIndex) {
  auto [byteOff, mask] = ResolveBit(bitIndex);
  bitArray_[byteOff] |= mask;
}

// --- Membership queries ---
// Uses Kirsch-Mitzenmacher enhanced double hashing to derive k probe
// positions from a single HashPair. Reference: Kirsch & Mitzenmacher,
// "Less Hashing, Same Performance" (ESA 2006).

bool BloomLayer::Test(const HashPair& hp) const {
  bool isPow2 = IsPowerOfTwo();
  uint64_t mask = isPow2 ? ComputeModuloMask() : 0;

  for (uint32_t probe = 0; probe < hashCount_; probe++) {
    uint64_t pos = ProbePosition(hp, probe, mask, totalBits_, isPow2);
    if (!TestBit(pos)) return false;
  }
  return true;
}

bool BloomLayer::Insert(const HashPair& hp) {
  bool isPow2 = IsPowerOfTwo();
  uint64_t mask = isPow2 ? ComputeModuloMask() : 0;
  bool anyNew = false;

  for (uint32_t probe = 0; probe < hashCount_; probe++) {
    uint64_t pos = ProbePosition(hp, probe, mask, totalBits_, isPow2);
    if (!TestBit(pos)) {
      SetBit(pos);
      anyNew = true;
    }
  }
  return anyNew;
}
