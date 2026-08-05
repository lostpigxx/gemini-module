#include "cms_rdb.h"
#include "bloom_rdb.h"
#include "cms_sketch.h"
#include "rm_alloc.h"

#include <cstring>

// --- Field validation ---
// Rejects states that would cause UB (divide-by-zero, out-of-bounds) or
// silent corruption, whether coming from RDB load or LOADCHUNK.

static bool ValidateCmsFields(uint64_t width, uint64_t depth, uint64_t dataSize) {
  if (width == 0 || width > kCmsMaxWidth) return false;
  if (depth == 0 || depth > kCmsMaxDepth) return false;
  if (width * depth > kCmsMaxCounters) return false;
  if (dataSize != width * depth * sizeof(uint32_t)) return false;
  return true;
}

// --- CmsSketch RDB serialization ---
// Field order: width, depth, totalCount, counters blob. This is this
// module's own RDB wire-format protocol (command-level compatibility only;
// no byte format compatibility with any external sketch is required).

static void WriteCmsSketch(const CmsSketch& sketch, RdbWriter& w) {
  w.PutUint(sketch.Width());
  w.PutUint(sketch.Depth());
  w.PutUint(sketch.TotalCount());
  w.PutBlob(reinterpret_cast<const uint8_t*>(sketch.GetCounterArray()), sketch.GetDataSize());
}

static CmsSketch* ReadCmsSketch(RdbReader& r) {
  uint64_t rawWidth = r.GetUint();
  uint64_t rawDepth = r.GetUint();
  uint64_t totalCount = r.GetUint();

  if (!r.Ok()) return nullptr;
  if (rawWidth > UINT32_MAX || rawDepth > UINT32_MAX) return nullptr;

  auto [buf, bufLen] = r.GetBlob();
  if (!r.Ok() || !buf) {
    if (buf) RedisModule_Free(buf);
    return nullptr;
  }
  if (!ValidateCmsFields(rawWidth, rawDepth, static_cast<uint64_t>(bufLen))) {
    RedisModule_Free(buf);
    return nullptr;
  }

  auto maybeSketch = CmsSketch::Create(static_cast<uint32_t>(rawWidth), static_cast<uint32_t>(rawDepth));
  if (!maybeSketch) {
    RedisModule_Free(buf);
    return nullptr;
  }
  std::memcpy(maybeSketch->GetCounterArray(), buf, bufLen);
  RedisModule_Free(buf);
  maybeSketch->SetTotalCount(totalCount);

  auto* sketch = static_cast<CmsSketch*>(RMAlloc(sizeof(CmsSketch)));
  if (!sketch) return nullptr;
  new (sketch) CmsSketch(std::move(*maybeSketch));
  return sketch;
}

// --- LOADCHUNK wire format ---

#pragma pack(push, 1)
struct CmsWireHeader {
  uint32_t width;
  uint32_t depth;
  uint64_t totalCount;
};
#pragma pack(pop)

size_t CmsComputeChunkSize(const CmsSketch& sketch) {
  return sizeof(CmsWireHeader) + sketch.GetDataSize();
}

size_t CmsSerializeChunk(const CmsSketch& sketch, void* output) {
  auto* hdr = static_cast<CmsWireHeader*>(output);
  hdr->width = sketch.Width();
  hdr->depth = sketch.Depth();
  hdr->totalCount = sketch.TotalCount();

  std::memcpy(static_cast<char*>(output) + sizeof(CmsWireHeader), sketch.GetCounterArray(),
              sketch.GetDataSize());

  return sizeof(CmsWireHeader) + sketch.GetDataSize();
}

CmsSketch* CmsDeserializeChunk(const void* data, size_t length) {
  if (length < sizeof(CmsWireHeader)) return nullptr;

  const auto* hdr = static_cast<const CmsWireHeader*>(data);
  uint64_t dataSize = length - sizeof(CmsWireHeader);
  if (!ValidateCmsFields(hdr->width, hdr->depth, dataSize)) return nullptr;

  auto maybeSketch = CmsSketch::Create(hdr->width, hdr->depth);
  if (!maybeSketch) return nullptr;

  std::memcpy(maybeSketch->GetCounterArray(), static_cast<const char*>(data) + sizeof(CmsWireHeader),
              dataSize);
  maybeSketch->SetTotalCount(hdr->totalCount);

  auto* sketch = static_cast<CmsSketch*>(RMAlloc(sizeof(CmsSketch)));
  if (!sketch) return nullptr;
  new (sketch) CmsSketch(std::move(*maybeSketch));
  return sketch;
}

// --- Module type callbacks ---

RedisModuleType* CmsType = nullptr;

void* RdbLoadCms(RedisModuleIO* rdb, int encver) {
  if (encver != kCmsEncVerCurrent) return nullptr;
  RdbReader reader(rdb);
  return ReadCmsSketch(reader);
}

void RdbSaveCms(RedisModuleIO* rdb, void* value) {
  RdbWriter writer(rdb);
  WriteCmsSketch(*static_cast<CmsSketch*>(value), writer);
}

void AofRewriteCms(RedisModuleIO* aof, RedisModuleString* key, void* value) {
  auto* sketch = static_cast<CmsSketch*>(value);

  size_t chunkBytes = CmsComputeChunkSize(*sketch);
  auto* buf = static_cast<char*>(RMAlloc(chunkBytes));
  if (!buf) {
    RedisModule_LogIOError(aof, "warning",
      "GeminiBloom: CMS AOF rewrite allocation failure, key omitted");
    return;
  }
  CmsSerializeChunk(*sketch, buf);
  RedisModule_EmitAOF(aof, "CMS.LOADCHUNK", "sb", key, buf, chunkBytes);
  RMFree(buf);
}

void FreeCms(void* value) {
  if (auto* sketch = static_cast<CmsSketch*>(value)) {
    sketch->~CmsSketch();
    RMFree(sketch);
  }
}

size_t CmsMemUsage(const void* value) {
  if (!value) return 0;
  auto* sketch = static_cast<const CmsSketch*>(value);
  return sizeof(CmsSketch) + sketch->GetDataSize();
}

void DigestCms(RedisModuleDigest* digest, void* value) {
  auto* sketch = static_cast<CmsSketch*>(value);

  RedisModule_DigestAddLongLong(digest, static_cast<long long>(sketch->Width()));
  RedisModule_DigestAddLongLong(digest, static_cast<long long>(sketch->Depth()));
  RedisModule_DigestAddLongLong(digest, static_cast<long long>(sketch->TotalCount()));
  RedisModule_DigestAddStringBuffer(digest,
    reinterpret_cast<const char*>(sketch->GetCounterArray()), sketch->GetDataSize());

  RedisModule_DigestEndSequence(digest);
}

void* CopyCms2(RedisModuleKeyOptCtx* ctx, const void* value) {
  (void)ctx;
  auto maybeCopy = static_cast<const CmsSketch*>(value)->Clone();
  if (!maybeCopy) return nullptr;
  auto* sketch = static_cast<CmsSketch*>(RMAlloc(sizeof(CmsSketch)));
  if (!sketch) return nullptr;
  new (sketch) CmsSketch(std::move(*maybeCopy));
  return sketch;
}

size_t FreeEffortCms2(RedisModuleKeyOptCtx* ctx, const void* value) {
  (void)ctx;
  (void)value;
  return 2;  // sketch object + counters buffer
}

int DefragCms(RedisModuleDefragCtx* ctx, RedisModuleString* key, void** value) {
  (void)key;
  auto* sketch = static_cast<CmsSketch*>(*value);

  if (auto* relocated = static_cast<CmsSketch*>(RedisModule_DefragAlloc(ctx, sketch))) {
    sketch = relocated;
    *value = sketch;
  }

  if (auto* relocatedCounters =
        static_cast<uint32_t*>(RedisModule_DefragAlloc(ctx, sketch->GetCounterArray()))) {
    sketch->AdoptCounterArray(relocatedCounters);
  }

  return 0;
}
