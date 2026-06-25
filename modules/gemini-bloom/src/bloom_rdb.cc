#include "bloom_rdb.h"
#include "sb_chain.h"
#include "rm_alloc.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

constexpr uint32_t kMaxLayers = 1024;

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

// --- Canonical layer field validation ---
// Used by both RDB and wire deserialization paths. Rejects states that
// would cause UB (divide-by-zero, shift overflow) or silent corruption.
struct LayerFields {
  uint64_t capacity;
  double fpRate;
  uint32_t hashCount;
  double bitsPerEntry;
  uint64_t totalBits;
  uint8_t log2Bits;
  uint64_t dataSize;
};

static bool ValidateLayerFields(const LayerFields& f) {
  if (f.capacity == 0) return false;
  if (f.totalBits == 0) return false;
  if (f.hashCount == 0) return false;
  if (f.totalBits > UINT64_MAX - 7) return false;
  if (f.log2Bits >= 64) return false;
  if (f.log2Bits > 0 && f.totalBits != (1ULL << f.log2Bits)) return false;
  uint64_t expectedSize = (f.totalBits + 7) / 8;
  if (f.dataSize != expectedSize) return false;
  if (!std::isfinite(f.fpRate) || f.fpRate <= 0.0 || f.fpRate >= 1.0) return false;
  if (!std::isfinite(f.bitsPerEntry) || f.bitsPerEntry <= 0.0) return false;
  uint32_t expectedHash = std::max(1u,
    static_cast<uint32_t>(std::ceil(std::numbers::ln2 * f.bitsPerEntry)));
  if (f.hashCount != expectedHash) return false;
  return true;
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
  layer.hashCount_ = static_cast<uint32_t>(r.GetUint());
  layer.bitsPerEntry_ = r.GetFloat();
  layer.totalBits_ = r.GetUint();
  layer.log2Bits_ = static_cast<uint8_t>(r.GetUint());

  if (!r.Ok()) return std::nullopt;

  layer.dataSize_ = (layer.totalBits_ > 0) ? ((layer.totalBits_ + 7) / 8) : 0;
  layer.use64Bit_ = HasFlag(filterFlags, BloomFlags::Use64Bit);

  LayerFields fields{layer.capacity_, layer.fpRate_, layer.hashCount_,
                     layer.bitsPerEntry_, layer.totalBits_, layer.log2Bits_,
                     layer.dataSize_};
  if (!ValidateLayerFields(fields)) return std::nullopt;

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

std::optional<BloomLayer> BloomLayer::FromWireMeta(const WireLayerMeta& meta, BloomFlags filterFlags) {
  BloomLayer layer;
  layer.hashCount_ = meta.hashCount;
  layer.log2Bits_ = meta.log2Bits;
  layer.use64Bit_ = HasFlag(filterFlags, BloomFlags::Use64Bit);
  layer.capacity_ = meta.capacity;
  layer.fpRate_ = meta.fpRate;
  layer.bitsPerEntry_ = meta.bitsPerEntry;
  layer.totalBits_ = meta.totalBits;
  layer.dataSize_ = meta.dataSize;
  layer.bitArray_ = static_cast<uint8_t*>(RMCalloc(layer.dataSize_, 1));
  if (!layer.bitArray_) return std::nullopt;
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
  RdbShell shell;
  shell.totalItems = r.GetUint();
  shell.numLayers = static_cast<size_t>(r.GetUint());

  unsigned rawFlags = (encver >= kEncVerWithFlags)
    ? static_cast<unsigned>(r.GetUint())
    : ToUnderlying(BloomFlags::Use64Bit | BloomFlags::NoRound);

  if (!ValidateFlags(rawFlags)) return nullptr;
  shell.flags = FromUnderlying(rawFlags);

  shell.expansionFactor = (encver >= kEncVerWithExpansion)
    ? static_cast<unsigned>(r.GetUint())
    : 2;

  if (!r.Ok() || shell.numLayers == 0 || shell.numLayers > kMaxLayers) return nullptr;
  if (!HasFlag(shell.flags, BloomFlags::FixedSize) && shell.expansionFactor == 0)
    return nullptr;

  auto* filter = FromRdbShell(shell);
  if (!filter) return nullptr;

  uint64_t itemSum = 0;
  for (size_t i = 0; i < shell.numLayers; i++) {
    auto maybeLayer = BloomLayer::ReadFrom(r, shell.flags);
    if (!maybeLayer) {
      filter->~ScalingBloomFilter();
      RMFree(filter);
      return nullptr;
    }
    size_t count = static_cast<size_t>(r.GetUint());
    if (!r.Ok()) {
      filter->~ScalingBloomFilter();
      RMFree(filter);
      return nullptr;
    }
    if (count > maybeLayer->GetCapacity()) {
      filter->~ScalingBloomFilter();
      RMFree(filter);
      return nullptr;
    }
    if (itemSum > UINT64_MAX - count) {
      filter->~ScalingBloomFilter();
      RMFree(filter);
      return nullptr;
    }
    itemSum += count;
    filter->SetLayer(i, {std::move(*maybeLayer), count});
  }

  if (itemSum != shell.totalItems) {
    filter->~ScalingBloomFilter();
    RMFree(filter);
    return nullptr;
  }

  return filter;
}

// --- SCANDUMP wire format ---

size_t ComputeHeaderSize(const ScalingBloomFilter& filter) {
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

static bool ValidateLayerMeta(const WireLayerMeta& meta) {
  return ValidateLayerFields({meta.capacity, meta.fpRate, meta.hashCount,
                              meta.bitsPerEntry, meta.totalBits, meta.log2Bits,
                              meta.dataSize});
}

ScalingBloomFilter* DeserializeHeader(const void* data, size_t length) {
  if (length < sizeof(WireFilterHeader)) return nullptr;

  const auto* hdr = static_cast<const WireFilterHeader*>(data);
  if (hdr->numLayers == 0 || hdr->numLayers > kMaxLayers) return nullptr;
  size_t required = sizeof(WireFilterHeader) + hdr->numLayers * sizeof(WireLayerMeta);
  if (length != required) return nullptr;

  if (!ValidateFlags(hdr->flags)) return nullptr;

  auto filterFlags = FromUnderlying(hdr->flags);

  if (!HasFlag(filterFlags, BloomFlags::FixedSize) && hdr->expansionFactor == 0) {
    return nullptr;
  }

  const auto* meta = reinterpret_cast<const WireLayerMeta*>(
    static_cast<const char*>(data) + sizeof(WireFilterHeader));

  constexpr uint64_t kMaxTotalDataSize = 4ULL * 1024 * 1024 * 1024;
  uint64_t itemSum = 0;
  uint64_t totalDataSize = 0;
  for (size_t i = 0; i < hdr->numLayers; i++) {
    if (!ValidateLayerMeta(meta[i])) return nullptr;
    if (meta[i].itemCount > meta[i].capacity) return nullptr;
    if (itemSum > UINT64_MAX - meta[i].itemCount) return nullptr;
    itemSum += meta[i].itemCount;
    if (meta[i].dataSize > kMaxTotalDataSize ||
        totalDataSize > kMaxTotalDataSize - meta[i].dataSize) return nullptr;
    totalDataSize += meta[i].dataSize;
  }
  if (itemSum != hdr->totalItems) return nullptr;

  auto* filter = ScalingBloomFilter::FromRdbShell({
    .totalItems = hdr->totalItems,
    .numLayers = hdr->numLayers,
    .flags = filterFlags,
    .expansionFactor = hdr->expansionFactor,
  });
  if (!filter) return nullptr;

  for (size_t i = 0; i < hdr->numLayers; i++) {
    auto layer = BloomLayer::FromWireMeta(meta[i], filterFlags);
    if (!layer) {
      filter->~ScalingBloomFilter();
      RMFree(filter);
      return nullptr;
    }
    filter->SetLayer(i, {std::move(*layer), meta[i].itemCount});
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
  auto* hdrBuf = static_cast<char*>(RMAlloc(hdrBytes));
  if (!hdrBuf) {
    RedisModule_LogIOError(aof, "warning",
      "GeminiBloom: AOF rewrite allocation failure, key omitted");
    return;
  }
  SerializeHeader(*filter, hdrBuf);

  RedisModule_EmitAOF(aof, "BF.LOADCHUNK", "slb", key, (long long)1, hdrBuf, hdrBytes);
  RMFree(hdrBuf);

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
