#include "bloom_rdb.h"
#include "sb_chain.h"
#include "rm_alloc.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstring>
#include <numbers>

constexpr uint32_t kMaxLayers = 1024;
constexpr size_t kMaxWireHeaderBytes =
  sizeof(WireFilterHeader) + kMaxLayers * sizeof(WireLayerMeta);

// --- RdbWriter / RdbReader ---

void RdbWriter::PutUint(uint64_t v) { RedisModule_SaveUnsigned(io_, v); }
void RdbWriter::PutFloat(double v) { RedisModule_SaveDouble(io_, v); }
void RdbWriter::PutBlob(const uint8_t* data, uint64_t len) {
  RedisModule_SaveStringBuffer(io_, reinterpret_cast<const char*>(data), len);
}

uint64_t RdbReader::GetUint() {
  if (!ok_) return 0;
  uint64_t v = RedisModule_LoadUnsigned(io_);
  if (RedisModule_IsIOError && RedisModule_IsIOError(io_)) ok_ = false;
  return v;
}
double RdbReader::GetFloat() {
  if (!ok_) return 0.0;
  double v = RedisModule_LoadDouble(io_);
  if (RedisModule_IsIOError && RedisModule_IsIOError(io_)) ok_ = false;
  return v;
}
std::pair<char*, size_t> RdbReader::GetBlob() {
  if (!ok_) return {nullptr, 0};
  size_t len = 0;
  char* buf = RedisModule_LoadStringBuffer(io_, &len);
  if (RedisModule_IsIOError && RedisModule_IsIOError(io_)) ok_ = false;
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
  if (f.dataSize > kMaxLayerDataSize) return false;
  if (!std::isfinite(f.fpRate) || f.fpRate <= 0.0 || f.fpRate >= 1.0) return false;
  if (!std::isfinite(f.bitsPerEntry) || f.bitsPerEntry <= 0.0 ||
      f.bitsPerEntry > kMaxBitsPerEntry) return false;
  uint32_t expectedHash = std::max(1u,
    static_cast<uint32_t>(std::ceil(std::numbers::ln2 * f.bitsPerEntry)));
  if (f.hashCount != expectedHash) return false;
  return true;
}

// --- BloomLayer serialization (lives here to access Redis Module API) ---
// Field order matches the RedisBloom RDB wire format for bloom filter
// persistence. Verified against RedisBloom v2.4.20 (Redis 6.2.17).

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
  uint64_t rawHashCount = r.GetUint();
  layer.bitsPerEntry_ = r.GetFloat();
  layer.totalBits_ = r.GetUint();
  uint64_t rawLog2Bits = r.GetUint();

  if (!r.Ok()) return std::nullopt;
  if (rawHashCount > UINT32_MAX || rawLog2Bits > 255) return std::nullopt;
  layer.hashCount_ = static_cast<uint32_t>(rawHashCount);
  layer.log2Bits_ = static_cast<uint8_t>(rawLog2Bits);

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
  w.PutUint(ToUnderlying(flags_) & kRdbFlagsMask);
  w.PutUint(expansionFactor_);
  // Only a Loading filter writes this extra field, so complete filters keep the
  // exact byte layout RedisBloom DUMP/RESTORE interop expects. Without it the
  // restored key would report the header's item count over zero-filled layers.
  if (IsLoading()) w.PutUint(chunksLoaded_);

  for (const auto& layer : Layers()) {
    layer.bloom.WriteTo(w);
    w.PutUint(layer.itemCount);
  }
}

ScalingBloomFilter* ScalingBloomFilter::ReadFrom(RdbReader& r, int encver) {
  RdbShell shell;
  shell.totalItems = r.GetUint();
  shell.numLayers = static_cast<size_t>(r.GetUint());

  uint64_t rawFlags = (encver >= kEncVerWithFlags)
    ? r.GetUint()
    : ToUnderlying(BloomFlags::Use64Bit | BloomFlags::NoRound);

  if (rawFlags > UINT_MAX || !ValidateRdbFlags(static_cast<unsigned>(rawFlags)))
    return nullptr;
  shell.flags = FromUnderlying(static_cast<unsigned>(rawFlags));

  uint64_t rawExpansion = (encver >= kEncVerWithExpansion)
    ? r.GetUint()
    : 2;

  if (!r.Ok() || rawExpansion > kMaxExpansion) return nullptr;
  shell.expansionFactor = static_cast<unsigned>(rawExpansion);

  if (shell.numLayers == 0 || shell.numLayers > kMaxLayers) return nullptr;
  if (!HasFlag(shell.flags, BloomFlags::FixedSize) && shell.expansionFactor == 0)
    return nullptr;

  // Written by WriteTo immediately after the expansion factor, before the
  // layers. A cursor at or past numLayers would mean the filter was complete,
  // in which case Loading must not have been set.
  uint64_t chunksLoaded = 0;
  if (HasFlag(shell.flags, BloomFlags::Loading)) {
    chunksLoaded = r.GetUint();
    if (!r.Ok() || chunksLoaded >= shell.numLayers) return nullptr;
  }

  auto* filter = FromRdbShell(shell);
  if (!filter) return nullptr;

  uint64_t itemSum = 0;
  uint64_t totalDataSize = 0;
  for (size_t i = 0; i < shell.numLayers; i++) {
    auto maybeLayer = BloomLayer::ReadFrom(r, shell.flags);
    if (!maybeLayer) {
      filter->~ScalingBloomFilter();
      RMFree(filter);
      return nullptr;
    }
    if (maybeLayer->GetDataSize() > kMaxTotalDataSize ||
        totalDataSize > kMaxTotalDataSize - maybeLayer->GetDataSize()) {
      filter->~ScalingBloomFilter();
      RMFree(filter);
      return nullptr;
    }
    totalDataSize += maybeLayer->GetDataSize();
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

  // FromRdbShell copied the Loading bit out of the flags but reset the cursor,
  // so restore the progress the chunk sequence had actually reached.
  if (HasFlag(shell.flags, BloomFlags::Loading)) {
    filter->RestoreLoadingProgress(static_cast<size_t>(chunksLoaded));
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
  hdr->flags = ToUnderlying(filter.Flags()) & kPersistentFlagsMask;
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

  if (hdr->expansionFactor > kMaxExpansion) return nullptr;
  if (!HasFlag(filterFlags, BloomFlags::FixedSize) && hdr->expansionFactor == 0) {
    return nullptr;
  }

  const auto* meta = reinterpret_cast<const WireLayerMeta*>(
    static_cast<const char*>(data) + sizeof(WireFilterHeader));

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
  if (encver != kEncVerWithFlags && encver != kEncVerWithExpansion) return nullptr;
  RdbReader reader(rdb);
  return ScalingBloomFilter::ReadFrom(reader, encver);
}

void RdbSaveBloom(RedisModuleIO* rdb, void* value) {
  RdbWriter writer(rdb);
  static_cast<ScalingBloomFilter*>(value)->WriteTo(writer);
}

void AofRewriteBloom(RedisModuleIO* aof, RedisModuleString* key, void* value) {
  // Every key must be emitted, including a partially loaded one. RedisModule_LogIOError
  // does not set the RedisModuleIO error flag, so an early return here would drop the
  // key while Redis still reported the rewrite as successful.
  auto* filter = static_cast<ScalingBloomFilter*>(value);
  size_t hdrBytes = ComputeHeaderSize(*filter);

  // Unreachable: kMaxLayers is enforced on every construction path, and scaling stops
  // near 50 layers because the per-layer rate halves against a 1e-15 floor. Kept so a
  // corrupted layer count cannot overflow the stack buffer below.
  if (hdrBytes > kMaxWireHeaderBytes) {
    RedisModule_LogIOError(aof, "warning",
      "GeminiBloom: AOF rewrite rejected an invalid layer count");
    return;
  }
  std::array<char, kMaxWireHeaderBytes> hdrBuf;
  SerializeHeader(*filter, hdrBuf.data());

  RedisModule_EmitAOF(aof, "BF.LOADCHUNK", "slb", key, (long long)1,
    hdrBuf.data(), hdrBytes);

  // While Loading, only the layers that have actually arrived may be emitted.
  // Replaying the header re-enters Loading, so emitting just the loaded prefix
  // restores the same chunk cursor and lets BF.LOADCHUNK commands recorded in
  // the AOF tail after the rewrite fork continue from the right position.
  // Emitting every layer instead would clear Loading during replay, making
  // those tail commands fail and leaving a filter whose header claims items
  // that its zero-filled layers do not contain.
  size_t emitLayers = filter->IsLoading() ? filter->ChunksLoaded() : filter->NumLayers();

  long long cursor = 2;
  for (const auto& layer : filter->Layers().first(emitLayers)) {
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

void DigestBloom(RedisModuleDigest* digest, void* value) {
  auto* filter = static_cast<ScalingBloomFilter*>(value);

  RedisModule_DigestAddLongLong(digest, static_cast<long long>(filter->TotalItems()));
  RedisModule_DigestAddLongLong(digest, static_cast<long long>(filter->NumLayers()));
  RedisModule_DigestAddLongLong(digest, static_cast<long long>(ToUnderlying(filter->Flags())));
  RedisModule_DigestAddLongLong(digest, static_cast<long long>(filter->ExpansionFactor()));

  for (const auto& layer : filter->Layers()) {
    RedisModule_DigestAddLongLong(digest, static_cast<long long>(layer.itemCount));
    RedisModule_DigestAddLongLong(digest, static_cast<long long>(layer.bloom.GetCapacity()));
    RedisModule_DigestAddLongLong(digest, static_cast<long long>(layer.bloom.GetHashCount()));
    RedisModule_DigestAddLongLong(digest, static_cast<long long>(layer.bloom.GetTotalBits()));
    RedisModule_DigestAddStringBuffer(digest,
      reinterpret_cast<const char*>(layer.bloom.GetBitArray()), layer.bloom.GetDataSize());
  }

  RedisModule_DigestEndSequence(digest);
}

void* CopyBloom2(RedisModuleKeyOptCtx* ctx, const void* value) {
  (void)ctx;
  return static_cast<const ScalingBloomFilter*>(value)->Clone();
}

// Returns a count of the individual heap allocations owned by the filter
// (the layers array plus one bit-array buffer per layer). This is the same
// "number of allocations" semantics Redis' built-in types use for
// free_effort (e.g. dictSize()/quicklist length in lazyfree.c), and keeps
// ordinary bloom keys well under both the lazy-free threshold (64) and the
// active-defrag-max-scan-fields threshold (default 1000).
size_t FreeEffortBloom2(RedisModuleKeyOptCtx* ctx, const void* value) {
  (void)ctx;
  return static_cast<const ScalingBloomFilter*>(value)->NumLayers() + 1;
}

int DefragBloom(RedisModuleDefragCtx* ctx, RedisModuleString* key, void** value) {
  (void)key;
  auto* filter = static_cast<ScalingBloomFilter*>(*value);

  if (auto* relocated = static_cast<ScalingBloomFilter*>(RedisModule_DefragAlloc(ctx, filter))) {
    filter = relocated;
    *value = filter;
  }

  if (auto* relocatedLayers =
        static_cast<FilterLayer*>(RedisModule_DefragAlloc(ctx, filter->Layers().data()))) {
    filter->AdoptLayersArray(relocatedLayers);
  }

  for (auto& layer : filter->Layers()) {
    if (auto* relocatedBits =
          static_cast<uint8_t*>(RedisModule_DefragAlloc(ctx, layer.bloom.GetBitArray()))) {
      layer.bloom.AdoptBitArray(relocatedBits);
    }
  }

  return 0;
}
