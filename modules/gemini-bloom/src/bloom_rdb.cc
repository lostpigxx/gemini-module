#include "bloom_rdb.h"
#include "sb_chain.h"
#include "rm_alloc.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <optional>
#include <utility>

namespace {

constexpr unsigned kKnownBloomFlagMask =
  ToUnderlying(BloomFlags::NoRound | BloomFlags::RawBits |
               BloomFlags::Use64Bit | BloomFlags::FixedSize);

bool ValidateFlags(BloomFlags flags) {
  unsigned raw = ToUnderlying(flags);
  if ((raw & ~kKnownBloomFlagMask) != 0) return false;
  return !HasFlag(flags, BloomFlags::RawBits);
}

std::optional<uint64_t> ExpectedDataSize(uint64_t totalBits) {
  if (totalBits == 0 || totalBits > std::numeric_limits<uint64_t>::max() - 7) {
    return std::nullopt;
  }
  uint64_t bytes = (totalBits + 7) / 8;
  if (bytes == 0 ||
      bytes > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
    return std::nullopt;
  }
  return bytes;
}

bool NearlyEqual(double a, double b) {
  double scale = std::max({1.0, std::abs(a), std::abs(b)});
  return std::abs(a - b) <= scale * 1e-12;
}

std::optional<double> ExpectedBitsPerEntry(double fpRate) {
  if (!std::isfinite(fpRate) || fpRate <= 0.0 || fpRate >= 1.0) {
    return std::nullopt;
  }
  constexpr double kLn2Squared = std::numbers::ln2 * std::numbers::ln2;
  double bitsPerEntry = -std::log(fpRate) / kLn2Squared;
  if (!std::isfinite(bitsPerEntry) || bitsPerEntry <= 0.0) {
    return std::nullopt;
  }
  return bitsPerEntry;
}

std::optional<uint32_t> ExpectedHashCount(double bitsPerEntry) {
  double probes = std::ceil(std::numbers::ln2 * bitsPerEntry);
  if (!std::isfinite(probes) || probes < 1.0 ||
      probes > static_cast<double>(std::numeric_limits<uint32_t>::max())) {
    return std::nullopt;
  }
  return std::max(1u, static_cast<uint32_t>(probes));
}

std::optional<uint64_t> ExpectedTotalBits(uint64_t capacity, double bitsPerEntry,
                                          BloomFlags flags) {
  double rawBits = static_cast<double>(capacity) * bitsPerEntry;
  double boundedBits = std::max(rawBits, 1024.0);
  if (!std::isfinite(boundedBits) ||
      static_cast<long double>(boundedBits) >
        static_cast<long double>(std::numeric_limits<uint64_t>::max() - 7)) {
    return std::nullopt;
  }

  auto totalBits = static_cast<uint64_t>(boundedBits);
  if (!HasFlag(flags, BloomFlags::NoRound)) {
    if (totalBits > (uint64_t{1} << 63)) return std::nullopt;
    totalBits = std::bit_ceil(totalBits);
  }
  return totalBits;
}

bool ValidateLog2Bits(uint64_t totalBits, uint64_t log2Bits, BloomFlags flags) {
  if (HasFlag(flags, BloomFlags::NoRound)) return log2Bits == 0;

  bool isPowerOfTwo = totalBits != 0 && (totalBits & (totalBits - 1)) == 0;
  if (!isPowerOfTwo) return false;
  return log2Bits < 64 && (uint64_t{1} << log2Bits) == totalBits;
}

bool ValidateLayerMetaValues(uint64_t capacity, double fpRate,
                             uint64_t hashCount, double bitsPerEntry,
                             uint64_t totalBits, uint64_t log2Bits,
                             uint64_t dataSize, BloomFlags flags,
                             std::optional<uint64_t> itemCount = std::nullopt) {
  if (capacity == 0 ||
      capacity > static_cast<uint64_t>(std::numeric_limits<long long>::max())) {
    return false;
  }
  if (hashCount == 0 ||
      hashCount > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  if (!std::isfinite(fpRate) || fpRate <= 0.0 || fpRate >= 1.0) {
    return false;
  }
  if (!std::isfinite(bitsPerEntry) || bitsPerEntry <= 0.0) {
    return false;
  }

  auto expectedBitsPerEntry = ExpectedBitsPerEntry(fpRate);
  if (!expectedBitsPerEntry ||
      !NearlyEqual(bitsPerEntry, *expectedBitsPerEntry)) {
    return false;
  }
  auto expectedHashCount = ExpectedHashCount(*expectedBitsPerEntry);
  if (!expectedHashCount || hashCount != *expectedHashCount) return false;
  auto expectedTotalBits = ExpectedTotalBits(capacity, *expectedBitsPerEntry, flags);
  if (!expectedTotalBits || totalBits != *expectedTotalBits) return false;

  auto expectedSize = ExpectedDataSize(totalBits);
  if (!expectedSize || dataSize != *expectedSize) return false;
  if (!ValidateLog2Bits(totalBits, log2Bits, flags)) return false;
  if (itemCount && *itemCount > capacity) return false;
  return true;
}

bool ValidateFilterMetaValues(uint64_t totalItems, uint64_t numLayers,
                              BloomFlags flags, uint64_t expansionFactor) {
  if (totalItems > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
      totalItems > static_cast<uint64_t>(std::numeric_limits<long long>::max())) {
    return false;
  }
  if (numLayers == 0 || numLayers > kMaxBloomLayers ||
      numLayers > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
    return false;
  }
  if (!ValidateFlags(flags)) return false;
  if (!HasFlag(flags, BloomFlags::FixedSize) && expansionFactor == 0) {
    return false;
  }
  return expansionFactor <= std::numeric_limits<unsigned>::max();
}

}  // namespace

// --- RdbWriter / RdbReader ---

void RdbWriter::PutUint(uint64_t v) { RedisModule_SaveUnsigned(io_, v); }
void RdbWriter::PutFloat(double v) { RedisModule_SaveDouble(io_, v); }
void RdbWriter::PutBlob(const uint8_t* data, uint64_t len) {
  RedisModule_SaveStringBuffer(io_, reinterpret_cast<const char*>(data), len);
}

uint64_t RdbReader::GetUint() {
  if (!ok_) return 0;
  uint64_t v = RedisModule_LoadUnsigned(io_);
  if (RedisModule_IsIOError(io_)) ok_ = false;
  return v;
}
double RdbReader::GetFloat() {
  if (!ok_) return 0.0;
  double v = RedisModule_LoadDouble(io_);
  if (RedisModule_IsIOError(io_)) ok_ = false;
  return v;
}
std::pair<char*, size_t> RdbReader::GetBlob() {
  if (!ok_) return {nullptr, 0};
  size_t len = 0;
  char* buf = RedisModule_LoadStringBuffer(io_, &len);
  if (RedisModule_IsIOError(io_)) ok_ = false;
  return {buf, len};
}

// --- BloomLayer serialization (lives here to access Redis Module API) ---
// Field order is intended to match the RedisBloom RDB wire format for
// bloom filter persistence. Full interoperability has not been verified
// against an official RedisBloom golden corpus.

void BloomLayer::WriteTo(RdbWriter& w) const {
  w.PutUint(capacity_);
  w.PutFloat(fpRate_);
  w.PutUint(hashCount_);
  w.PutFloat(bitsPerEntry_);
  w.PutUint(totalBits_);
  w.PutUint(log2Bits_);
  w.PutBlob(bitArray_, dataSize_);
}

std::optional<BloomLayer> BloomLayer::ReadFrom(RdbReader& r, BloomFlags filterFlags) {
  BloomLayer layer;
  layer.capacity_ = r.GetUint();
  layer.fpRate_ = r.GetFloat();
  uint64_t hashCount = r.GetUint();
  layer.bitsPerEntry_ = r.GetFloat();
  layer.totalBits_ = r.GetUint();
  uint64_t log2Bits = r.GetUint();
  layer.use64Bit_ = HasFlag(filterFlags, BloomFlags::Use64Bit);

  if (!r.Ok()) return std::nullopt;
  auto expectedDataSize = ExpectedDataSize(layer.totalBits_);
  if (!expectedDataSize) return std::nullopt;
  if (!ValidateLayerMetaValues(layer.capacity_, layer.fpRate_, hashCount,
                               layer.bitsPerEntry_, layer.totalBits_,
                               log2Bits, *expectedDataSize, filterFlags)) {
    return std::nullopt;
  }

  layer.hashCount_ = static_cast<uint32_t>(hashCount);
  layer.log2Bits_ = static_cast<uint8_t>(log2Bits);
  layer.dataSize_ = *expectedDataSize;

  auto [buf, bufLen] = r.GetBlob();
  if (!r.Ok() || !buf || bufLen != static_cast<size_t>(layer.dataSize_)) {
    if (buf) RedisModule_Free(buf);
    return std::nullopt;
  }
  layer.bitArray_ = static_cast<uint8_t*>(RMAlloc(layer.dataSize_));
  if (!layer.bitArray_) {
    RedisModule_Free(buf);
    return std::nullopt;
  }
  std::memcpy(layer.bitArray_, buf, bufLen);
  RedisModule_Free(buf);
  return layer;
}

WireLayerMeta BloomLayer::ToWireMeta(size_t itemCount) const {
  return {
    .dataSize = dataSize_,
    .totalBits = totalBits_,
    .itemCount = itemCount,
    .fpRate = fpRate_,
    .bitsPerEntry = bitsPerEntry_,
    .hashCount = hashCount_,
    .capacity = capacity_,
    .log2Bits = log2Bits_,
  };
}

BloomLayer BloomLayer::FromWireMeta(const WireLayerMeta& meta, BloomFlags filterFlags) {
  BloomLayer layer;
  layer.hashCount_ = meta.hashCount;
  layer.log2Bits_ = meta.log2Bits;
  layer.use64Bit_ = HasFlag(filterFlags, BloomFlags::Use64Bit);
  layer.capacity_ = meta.capacity;
  layer.fpRate_ = meta.fpRate;
  layer.bitsPerEntry_ = meta.bitsPerEntry;
  layer.totalBits_ = meta.totalBits;
  layer.dataSize_ = meta.dataSize;
  if (layer.dataSize_ <= static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
    layer.bitArray_ = static_cast<uint8_t*>(RMCalloc(static_cast<size_t>(layer.dataSize_), 1));
  }
  return layer;
}

// --- ScalingBloomFilter RDB serialization ---
// Filter-level field order: totalItems, numLayers, flags, expansion, layers[]
// is the RDB wire-format protocol. encver gates which fields are present
// (see kEncVerWithFlags, kEncVerWithExpansion in bloom_rdb.h).

void ScalingBloomFilter::WriteTo(RdbWriter& w) const {
  w.PutUint(totalItems_);
  w.PutUint(numLayers_);
  w.PutUint(ToUnderlying(flags_));
  w.PutUint(expansionFactor_);

  for (const auto& layer : Layers()) {
    layer.bloom.WriteTo(w);
    w.PutUint(layer.itemCount);
  }
}

ScalingBloomFilter* ScalingBloomFilter::ReadFrom(RdbReader& r, int encver) {
  uint64_t totalItems = r.GetUint();
  uint64_t numLayers = r.GetUint();

  uint64_t rawFlags = (encver >= kEncVerWithFlags)
    ? r.GetUint()
    : ToUnderlying(BloomFlags::Use64Bit | BloomFlags::NoRound);

  uint64_t expansionFactor = (encver >= kEncVerWithExpansion)
    ? r.GetUint()
    : 2;

  if (!r.Ok() || rawFlags > std::numeric_limits<unsigned>::max()) return nullptr;

  BloomFlags flags = (encver >= kEncVerWithFlags)
    ? FromUnderlying(static_cast<unsigned>(rawFlags))
    : (BloomFlags::Use64Bit | BloomFlags::NoRound);

  if (!ValidateFilterMetaValues(totalItems, numLayers, flags, expansionFactor)) {
    return nullptr;
  }

  RdbShell shell{
    .totalItems = static_cast<size_t>(totalItems),
    .numLayers = static_cast<size_t>(numLayers),
    .flags = flags,
    .expansionFactor = static_cast<unsigned>(expansionFactor),
  };

  auto* filter = FromRdbShell(shell);
  if (!filter) return nullptr;

  size_t countSum = 0;
  for (size_t i = 0; i < shell.numLayers; i++) {
    auto maybeLayer = BloomLayer::ReadFrom(r, shell.flags);
    if (!maybeLayer) {
      filter->~ScalingBloomFilter();
      RMFree(filter);
      return nullptr;
    }
    uint64_t countRaw = r.GetUint();
    if (!r.Ok() ||
        countRaw > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
        countRaw > maybeLayer->GetCapacity()) {
      filter->~ScalingBloomFilter();
      RMFree(filter);
      return nullptr;
    }
    size_t count = static_cast<size_t>(countRaw);
    if (count > std::numeric_limits<size_t>::max() - countSum ||
        !filter->SetLayer(i, {std::move(*maybeLayer), count})) {
      filter->~ScalingBloomFilter();
      RMFree(filter);
      return nullptr;
    }
    countSum += count;
  }

  if (countSum != shell.totalItems) {
    filter->~ScalingBloomFilter();
    RMFree(filter);
    return nullptr;
  }

  return filter;
}

// --- SCANDUMP wire format ---

size_t ComputeHeaderSize(const ScalingBloomFilter& filter) {
  if (filter.NumLayers() > kMaxBloomLayers) return 0;
  return sizeof(WireFilterHeader) + filter.NumLayers() * sizeof(WireLayerMeta);
}

size_t SerializeHeader(const ScalingBloomFilter& filter, void* output) {
  auto* hdr = static_cast<WireFilterHeader*>(output);
  hdr->totalItems = filter.TotalItems();
  hdr->numLayers = static_cast<uint32_t>(filter.NumLayers());
  hdr->flags = ToUnderlying(filter.Flags());
  hdr->expansionFactor = filter.ExpansionFactor();

  auto* meta = reinterpret_cast<WireLayerMeta*>(
    static_cast<char*>(output) + sizeof(WireFilterHeader));

  for (size_t i = 0; i < filter.NumLayers(); i++) {
    const auto& layer = filter.Layers()[i];
    meta[i] = layer.bloom.ToWireMeta(layer.itemCount);
  }

  return sizeof(WireFilterHeader) + filter.NumLayers() * sizeof(WireLayerMeta);
}

static bool ValidateLayerMeta(const WireLayerMeta& meta, BloomFlags flags) {
  return ValidateLayerMetaValues(meta.capacity, meta.fpRate, meta.hashCount,
                                 meta.bitsPerEntry, meta.totalBits,
                                 meta.log2Bits, meta.dataSize, flags,
                                 meta.itemCount);
}

ScalingBloomFilter* DeserializeHeader(const void* data, size_t length) {
  if (length < sizeof(WireFilterHeader)) return nullptr;

  const auto* hdr = static_cast<const WireFilterHeader*>(data);
  auto filterFlags = FromUnderlying(hdr->flags);
  if (!ValidateFilterMetaValues(hdr->totalItems, hdr->numLayers, filterFlags,
                                hdr->expansionFactor)) {
    return nullptr;
  }

  size_t required = sizeof(WireFilterHeader) + hdr->numLayers * sizeof(WireLayerMeta);
  if (length != required) return nullptr;

  const auto* meta = reinterpret_cast<const WireLayerMeta*>(
    static_cast<const char*>(data) + sizeof(WireFilterHeader));

  uint64_t itemSum = 0;
  for (size_t i = 0; i < hdr->numLayers; i++) {
    if (!ValidateLayerMeta(meta[i], filterFlags)) return nullptr;
    if (meta[i].itemCount > std::numeric_limits<uint64_t>::max() - itemSum) {
      return nullptr;
    }
    itemSum += meta[i].itemCount;
  }
  if (itemSum != hdr->totalItems) return nullptr;

  auto* filter = ScalingBloomFilter::FromRdbShell({
    .totalItems = static_cast<size_t>(hdr->totalItems),
    .numLayers = hdr->numLayers,
    .flags = filterFlags,
    .expansionFactor = hdr->expansionFactor,
  });
  if (!filter) return nullptr;

  for (size_t i = 0; i < hdr->numLayers; i++) {
    auto layer = BloomLayer::FromWireMeta(meta[i], filterFlags);
    if (!layer.GetBitArray()) {
      filter->~ScalingBloomFilter();
      RMFree(filter);
      return nullptr;
    }
    if (!filter->SetLayer(i, {std::move(layer), static_cast<size_t>(meta[i].itemCount)})) {
      filter->~ScalingBloomFilter();
      RMFree(filter);
      return nullptr;
    }
  }

  return filter;
}

// --- Module type callbacks ---

RedisModuleType* BloomType = nullptr;

// The filter object serializes itself via WriteTo/ReadFrom.
// These callbacks are thin adapters between the Redis Module C API
// and our C++ serialization methods.

void* RdbLoadBloom(RedisModuleIO* rdb, int encver) {
  if (encver > kCurrentEncVer) return nullptr;
  RdbReader reader(rdb);
  return ScalingBloomFilter::ReadFrom(reader, encver);
}

void RdbSaveBloom(RedisModuleIO* rdb, void* value) {
  RdbWriter writer(rdb);
  static_cast<ScalingBloomFilter*>(value)->WriteTo(writer);
}

void AofRewriteBloom(RedisModuleIO* aof, RedisModuleString* key, void* value) {
  auto* filter = static_cast<ScalingBloomFilter*>(value);

  size_t hdrBytes = ComputeHeaderSize(*filter);
  if (hdrBytes == 0 || hdrBytes > kMaxBloomHeaderBytes) {
    RedisModule_LogIOError(aof, "warning", "Failed to serialize bloom header for AOF rewrite");
    return;
  }
  std::array<char, kMaxBloomHeaderBytes> hdrBuf{};
  SerializeHeader(*filter, hdrBuf.data());

  RedisModule_EmitAOF(aof, "BF.LOADCHUNK", "slb", key, (long long)1,
                      hdrBuf.data(), hdrBytes);

  long long cursor = 2;
  for (const auto& layer : filter->Layers()) {
    RedisModule_EmitAOF(aof, "BF.LOADCHUNK", "slb", key, cursor++,
      reinterpret_cast<const char*>(layer.bloom.GetBitArray()),
      layer.bloom.GetDataSize());
  }
}

void FreeBloom(void* value) {
  if (auto* filter = static_cast<ScalingBloomFilter*>(value)) {
    filter->~ScalingBloomFilter();
    RMFree(filter);
  }
}

size_t BloomMemUsage(const void* value) {
  if (!value) return 0;
  return static_cast<const ScalingBloomFilter*>(value)->BytesUsed();
}
