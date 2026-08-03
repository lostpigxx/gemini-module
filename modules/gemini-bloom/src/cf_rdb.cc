#include "cf_rdb.h"
#include "bloom_rdb.h"
#include "cf_chain.h"
#include "cuckoo_filter.h"
#include "rm_alloc.h"

#include <cstring>
#include <utility>

constexpr uint32_t kCfMaxLayers = 1024;

// --- Field validation ---
// Rejects states that would cause UB (divide-by-zero, shift overflow) or
// silent corruption, whether coming from RDB load or SCANDUMP/LOADCHUNK.

static bool ValidateCfLayerFields(uint64_t numBuckets, uint32_t bucketSize, uint64_t dataSize) {
  if (numBuckets == 0 || (numBuckets & (numBuckets - 1)) != 0) return false;
  if (numBuckets > kCfMaxBuckets) return false;
  if (bucketSize == 0 || bucketSize > kCfMaxBucketSize) return false;
  if (dataSize != numBuckets * static_cast<uint64_t>(bucketSize)) return false;
  return true;
}

static bool ValidateCfChainFields(uint32_t numLayers, uint32_t bucketSize, uint32_t maxIterations,
                                   uint32_t expansion) {
  if (numLayers == 0 || numLayers > kCfMaxLayers) return false;
  if (bucketSize == 0 || bucketSize > kCfMaxBucketSize) return false;
  if (maxIterations == 0 || maxIterations > kCfMaxIterations) return false;
  (void)expansion;  // 0 is valid (fixed-size, no auto-expansion)
  return true;
}

// --- CuckooChain RDB serialization ---
// Chain-level field order: totalItems, totalDeleted, numLayers, bucketSize,
// maxIterations, expansion, maxExpansions, layers[] is this module's own
// RDB wire-format protocol (command-level compatibility only; no byte
// format compatibility with any external cuckoo filter is required).

static void WriteCfChain(const CuckooChain& chain, RdbWriter& w) {
  w.PutUint(chain.TotalItems());
  w.PutUint(chain.TotalDeleted());
  w.PutUint(chain.NumLayers());
  w.PutUint(chain.BucketSize());
  w.PutUint(chain.MaxIterations());
  w.PutUint(chain.Expansion());
  w.PutUint(chain.MaxExpansions());
  // A chain that is still mid-LOADCHUNK has layers whose bucket arrays
  // aren't populated yet; itemSum recomputed from them on load will not
  // match totalItems_. Persist the loading state so ReadCfChain can skip
  // that cross-check and keep treating the reloaded chain as loading,
  // instead of rejecting it as corrupted.
  w.PutUint(chain.IsLoading() ? 1 : 0);
  w.PutUint(chain.ChunksLoaded());

  for (const auto& layer : chain.Layers()) {
    w.PutUint(layer.cuckoo.NumBuckets());
    w.PutBlob(layer.cuckoo.GetBucketArray(), layer.cuckoo.GetDataSize());
  }
}

static CuckooChain* ReadCfChain(RdbReader& r) {
  CuckooChain::RdbShell shell{};
  shell.totalItems = r.GetUint();
  shell.totalDeleted = r.GetUint();

  uint64_t rawNumLayers = r.GetUint();
  uint64_t rawBucketSize = r.GetUint();
  uint64_t rawMaxIterations = r.GetUint();
  uint64_t rawExpansion = r.GetUint();
  uint64_t rawMaxExpansions = r.GetUint();
  uint64_t rawLoading = r.GetUint();
  uint64_t rawChunksLoaded = r.GetUint();

  if (!r.Ok()) return nullptr;
  if (rawNumLayers > UINT32_MAX || rawBucketSize > UINT32_MAX ||
      rawMaxIterations > UINT32_MAX || rawExpansion > UINT32_MAX ||
      rawMaxExpansions > UINT32_MAX || rawChunksLoaded > rawNumLayers) {
    return nullptr;
  }
  bool loading = rawLoading != 0;
  shell.numLayers = static_cast<uint32_t>(rawNumLayers);
  shell.bucketSize = static_cast<uint32_t>(rawBucketSize);
  shell.maxIterations = static_cast<uint32_t>(rawMaxIterations);
  shell.expansion = static_cast<uint32_t>(rawExpansion);
  shell.maxExpansions = static_cast<uint32_t>(rawMaxExpansions);

  if (!ValidateCfChainFields(shell.numLayers, shell.bucketSize, shell.maxIterations,
                              shell.expansion)) {
    return nullptr;
  }

  auto* chain = CuckooChain::FromRdbShell(shell);
  if (!chain) return nullptr;

  uint64_t itemSum = 0;
  for (uint32_t i = 0; i < shell.numLayers; i++) {
    uint64_t numBuckets = r.GetUint();
    std::pair<char*, size_t> blob = r.GetBlob();
    char* buf = blob.first;
    size_t bufLen = blob.second;
    if (!r.Ok() || !buf) {
      if (buf) RedisModule_Free(buf);
      chain->~CuckooChain();
      RMFree(chain);
      return nullptr;
    }
    if (!ValidateCfLayerFields(numBuckets, shell.bucketSize, static_cast<uint64_t>(bufLen))) {
      RedisModule_Free(buf);
      chain->~CuckooChain();
      RMFree(chain);
      return nullptr;
    }

    auto maybeFilter = CuckooFilter::Create(numBuckets, shell.bucketSize, shell.maxIterations);
    if (!maybeFilter) {
      RedisModule_Free(buf);
      chain->~CuckooChain();
      RMFree(chain);
      return nullptr;
    }
    std::memcpy(maybeFilter->GetBucketArray(), buf, bufLen);
    RedisModule_Free(buf);
    maybeFilter->RecountItems();
    itemSum += maybeFilter->NumItems();

    chain->SetLayer(i, CfFilterLayer{std::move(*maybeFilter)});
  }

  // A chain saved mid-LOADCHUNK has layers whose bucket data was never
  // populated (still all-zero), so the recomputed itemSum legitimately
  // disagrees with the item count carried over from the source filter's
  // header. Skip the cross-check in that case and restore the loading
  // state instead, so the reloaded key still rejects reads until the
  // remaining chunks arrive, matching pre-reload behavior.
  if (loading) {
    chain->RestoreLoadingState(true, rawChunksLoaded);
    return chain;
  }

  // totalItems_/totalDeleted_ are cumulative counters that never decrease
  // (see TotalItems()/TotalDeleted() in cf_chain.h); the number of items
  // actually present in the bucket arrays is their difference, not
  // totalItems_ alone.
  if (shell.totalDeleted > shell.totalItems || itemSum != shell.totalItems - shell.totalDeleted) {
    chain->~CuckooChain();
    RMFree(chain);
    return nullptr;
  }

  return chain;
}

// --- SCANDUMP / LOADCHUNK wire format ---

size_t CfComputeHeaderSize(const CuckooChain& chain) {
  return sizeof(CfWireHeader) + chain.NumLayers() * sizeof(CfWireLayerMeta);
}

size_t CfSerializeHeader(const CuckooChain& chain, void* output) {
  auto* hdr = static_cast<CfWireHeader*>(output);
  hdr->totalItems = chain.TotalItems();
  hdr->totalDeleted = chain.TotalDeleted();
  hdr->numLayers = static_cast<uint32_t>(chain.NumLayers());
  hdr->bucketSize = chain.BucketSize();
  hdr->maxIterations = chain.MaxIterations();
  hdr->expansion = chain.Expansion();
  hdr->maxExpansions = chain.MaxExpansions();

  auto* meta = reinterpret_cast<CfWireLayerMeta*>(static_cast<char*>(output) + sizeof(CfWireHeader));
  for (size_t i = 0; i < chain.NumLayers(); i++) {
    const auto& layer = chain.Layers()[i];
    meta[i] = {layer.cuckoo.NumBuckets(), layer.cuckoo.GetDataSize()};
  }

  return sizeof(CfWireHeader) + chain.NumLayers() * sizeof(CfWireLayerMeta);
}

CuckooChain* CfDeserializeHeader(const void* data, size_t length) {
  if (length < sizeof(CfWireHeader)) return nullptr;

  const auto* hdr = static_cast<const CfWireHeader*>(data);
  if (!ValidateCfChainFields(hdr->numLayers, hdr->bucketSize, hdr->maxIterations, hdr->expansion)) {
    return nullptr;
  }
  size_t required = sizeof(CfWireHeader) + hdr->numLayers * sizeof(CfWireLayerMeta);
  if (length != required) return nullptr;

  const auto* meta = reinterpret_cast<const CfWireLayerMeta*>(
    static_cast<const char*>(data) + sizeof(CfWireHeader));

  for (uint32_t i = 0; i < hdr->numLayers; i++) {
    if (!ValidateCfLayerFields(meta[i].numBuckets, hdr->bucketSize, meta[i].dataSize)) {
      return nullptr;
    }
  }

  auto* chain = CuckooChain::FromRdbShell({
    hdr->totalItems,
    hdr->totalDeleted,
    hdr->numLayers,
    hdr->bucketSize,
    hdr->maxIterations,
    hdr->expansion,
    hdr->maxExpansions,
  });
  if (!chain) return nullptr;

  for (uint32_t i = 0; i < hdr->numLayers; i++) {
    auto maybeFilter = CuckooFilter::Create(meta[i].numBuckets, hdr->bucketSize, hdr->maxIterations);
    if (!maybeFilter) {
      chain->~CuckooChain();
      RMFree(chain);
      return nullptr;
    }
    chain->SetLayer(i, CfFilterLayer{std::move(*maybeFilter)});
  }

  return chain;
}

// --- Module type callbacks ---

RedisModuleType* CuckooType = nullptr;

void* RdbLoadCuckoo(RedisModuleIO* rdb, int encver) {
  if (encver != kCfEncVerCurrent) return nullptr;
  RdbReader reader(rdb);
  return ReadCfChain(reader);
}

void RdbSaveCuckoo(RedisModuleIO* rdb, void* value) {
  RdbWriter writer(rdb);
  WriteCfChain(*static_cast<CuckooChain*>(value), writer);
}

void AofRewriteCuckoo(RedisModuleIO* aof, RedisModuleString* key, void* value) {
  auto* chain = static_cast<CuckooChain*>(value);
  if (chain->IsLoading()) {
    RedisModule_LogIOError(aof, "warning",
      "GeminiBloom: skipping AOF rewrite of partially loaded filter");
    return;
  }

  size_t hdrBytes = CfComputeHeaderSize(*chain);
  auto* hdrBuf = static_cast<char*>(RMAlloc(hdrBytes));
  if (!hdrBuf) {
    RedisModule_LogIOError(aof, "warning",
      "GeminiBloom: CF AOF rewrite allocation failure, key omitted");
    return;
  }
  CfSerializeHeader(*chain, hdrBuf);

  RedisModule_EmitAOF(aof, "CF.LOADCHUNK", "slb", key, (long long)1, hdrBuf, hdrBytes);
  RMFree(hdrBuf);

  long long cursor = 2;
  for (const auto& layer : chain->Layers()) {
    RedisModule_EmitAOF(aof, "CF.LOADCHUNK", "slb", key, cursor++,
      reinterpret_cast<const char*>(layer.cuckoo.GetBucketArray()), layer.cuckoo.GetDataSize());
  }
}

void FreeCuckoo(void* value) {
  if (auto* chain = static_cast<CuckooChain*>(value)) {
    chain->~CuckooChain();
    RMFree(chain);
  }
}

size_t CuckooMemUsage(const void* value) {
  if (!value) return 0;
  return static_cast<const CuckooChain*>(value)->BytesUsed();
}

void DigestCuckoo(RedisModuleDigest* digest, void* value) {
  auto* chain = static_cast<CuckooChain*>(value);

  RedisModule_DigestAddLongLong(digest, static_cast<long long>(chain->TotalItems()));
  RedisModule_DigestAddLongLong(digest, static_cast<long long>(chain->TotalDeleted()));
  RedisModule_DigestAddLongLong(digest, static_cast<long long>(chain->NumLayers()));
  RedisModule_DigestAddLongLong(digest, static_cast<long long>(chain->BucketSize()));
  RedisModule_DigestAddLongLong(digest, static_cast<long long>(chain->Expansion()));
  RedisModule_DigestAddLongLong(digest, static_cast<long long>(chain->MaxIterations()));

  for (const auto& layer : chain->Layers()) {
    RedisModule_DigestAddLongLong(digest, static_cast<long long>(layer.cuckoo.NumBuckets()));
    RedisModule_DigestAddStringBuffer(digest,
      reinterpret_cast<const char*>(layer.cuckoo.GetBucketArray()), layer.cuckoo.GetDataSize());
  }

  RedisModule_DigestEndSequence(digest);
}

void* CopyCuckoo2(RedisModuleKeyOptCtx* ctx, const void* value) {
  (void)ctx;
  return static_cast<const CuckooChain*>(value)->Clone();
}

// Same "number of allocations" semantics as FreeEffortBloom2: the layers
// array plus one bucket-array buffer per layer.
size_t FreeEffortCuckoo2(RedisModuleKeyOptCtx* ctx, const void* value) {
  (void)ctx;
  return static_cast<const CuckooChain*>(value)->NumLayers() + 1;
}

int DefragCuckoo(RedisModuleDefragCtx* ctx, RedisModuleString* key, void** value) {
  (void)key;
  auto* chain = static_cast<CuckooChain*>(*value);

  if (auto* relocated = static_cast<CuckooChain*>(RedisModule_DefragAlloc(ctx, chain))) {
    chain = relocated;
    *value = chain;
  }

  if (auto* relocatedLayers =
        static_cast<CfFilterLayer*>(RedisModule_DefragAlloc(ctx, chain->Layers().data()))) {
    chain->AdoptLayersArray(relocatedLayers);
  }

  for (auto& layer : chain->Layers()) {
    if (auto* relocatedBuckets =
          static_cast<uint8_t*>(RedisModule_DefragAlloc(ctx, layer.cuckoo.GetBucketArray()))) {
      layer.cuckoo.AdoptBucketArray(relocatedBuckets);
    }
  }

  return 0;
}
