#include "topk_rdb.h"
#include "bloom_rdb.h"
#include "bloom_filter.h"
#include "topk_sketch.h"
#include "rm_alloc.h"

#include <cmath>
#include <cstring>

// --- Field validation ---
// Rejects states that would cause UB (divide-by-zero, out-of-bounds) or
// silent corruption, whether coming from RDB load or LOADCHUNK.

static bool ValidateTopkFields(uint64_t k, uint64_t width, uint64_t depth, double decay,
                                uint64_t cellDataSize, uint64_t numActive) {
  if (k == 0 || k > kTopkMaxK) return false;
  if (width == 0 || width > kTopkMaxWidth) return false;
  if (depth == 0 || depth > kTopkMaxDepth) return false;
  if (width * depth > kTopkMaxCells) return false;
  if (cellDataSize != width * depth * sizeof(TopkSketch::Cell)) return false;
  if (!std::isfinite(decay) || decay <= 0.0 || decay >= 1.0) return false;
  if (numActive > k) return false;
  return true;
}

// --- TopkSketch RDB serialization ---
// Field order: k, width, depth, decay, numActive, cells blob, then
// numActive (count, name-blob) pairs in descending-count order. This is
// this module's own RDB wire-format protocol (command-level compatibility
// only; no byte format compatibility with any external Top-K sketch is
// required).

static void WriteTopkSketch(const TopkSketch& sketch, RdbWriter& w) {
  w.PutUint(sketch.K());
  w.PutUint(sketch.Width());
  w.PutUint(sketch.Depth());
  w.PutFloat(sketch.Decay());
  w.PutUint(sketch.NumActive());
  w.PutBlob(reinterpret_cast<const uint8_t*>(sketch.GetCellArray()), sketch.GetCellDataSize());

  const auto* entries = sketch.GetEntries();
  for (uint32_t i = 0; i < sketch.NumActive(); i++) {
    w.PutUint(entries[i].count);
    w.PutBlob(reinterpret_cast<const uint8_t*>(entries[i].name), entries[i].nameLen);
  }
}

static TopkSketch* ReadTopkSketch(RdbReader& r) {
  uint64_t rawK = r.GetUint();
  uint64_t rawWidth = r.GetUint();
  uint64_t rawDepth = r.GetUint();
  double decay = r.GetFloat();
  uint64_t rawNumActive = r.GetUint();

  if (!r.Ok()) return nullptr;
  if (rawK > UINT32_MAX || rawWidth > UINT32_MAX || rawDepth > UINT32_MAX ||
      rawNumActive > UINT32_MAX) {
    return nullptr;
  }

  auto [cellBuf, cellBufLen] = r.GetBlob();
  if (!r.Ok() || !cellBuf) {
    if (cellBuf) RedisModule_Free(cellBuf);
    return nullptr;
  }
  if (!ValidateTopkFields(rawK, rawWidth, rawDepth, decay, static_cast<uint64_t>(cellBufLen), rawNumActive)) {
    RedisModule_Free(cellBuf);
    return nullptr;
  }

  auto maybeSketch = TopkSketch::Create(static_cast<uint32_t>(rawK), static_cast<uint32_t>(rawWidth),
                                         static_cast<uint32_t>(rawDepth), decay);
  if (!maybeSketch) {
    RedisModule_Free(cellBuf);
    return nullptr;
  }
  std::memcpy(maybeSketch->GetCellArray(), cellBuf, cellBufLen);
  RedisModule_Free(cellBuf);

  uint64_t prevCount = UINT64_MAX;
  for (uint32_t i = 0; i < rawNumActive; i++) {
    uint64_t count = r.GetUint();
    auto [nameBuf, nameLen] = r.GetBlob();
    if (!r.Ok() || !nameBuf) {
      if (nameBuf) RedisModule_Free(nameBuf);
      return nullptr;
    }
    // Entries must be persisted in non-increasing count order — this
    // maintains the invariant Add()/IncrBy() rely on (the last active
    // entry always holds the current top-k minimum).
    if (nameLen > kTopkMaxNameLen || count > prevCount) {
      RedisModule_Free(nameBuf);
      return nullptr;
    }
    prevCount = count;
    maybeSketch->AppendEntryForLoad(AsBytes(nameBuf, nameLen), count);
    RedisModule_Free(nameBuf);
  }

  auto* sketch = static_cast<TopkSketch*>(RMAlloc(sizeof(TopkSketch)));
  if (!sketch) return nullptr;
  new (sketch) TopkSketch(std::move(*maybeSketch));
  return sketch;
}

// --- LOADCHUNK wire format ---

#pragma pack(push, 1)
struct TopkWireHeader {
  uint32_t k;
  uint32_t width;
  uint32_t depth;
  double decay;
  uint32_t numActive;
};
struct TopkWireEntryHeader {
  uint64_t count;
  uint32_t nameLen;
};
#pragma pack(pop)

size_t TopkComputeChunkSize(const TopkSketch& sketch) {
  size_t size = sizeof(TopkWireHeader) + sketch.GetCellDataSize();
  const auto* entries = sketch.GetEntries();
  for (uint32_t i = 0; i < sketch.NumActive(); i++) {
    size += sizeof(TopkWireEntryHeader) + entries[i].nameLen;
  }
  return size;
}

size_t TopkSerializeChunk(const TopkSketch& sketch, void* output) {
  auto* cursor = static_cast<char*>(output);

  auto* hdr = reinterpret_cast<TopkWireHeader*>(cursor);
  hdr->k = sketch.K();
  hdr->width = sketch.Width();
  hdr->depth = sketch.Depth();
  hdr->decay = sketch.Decay();
  hdr->numActive = sketch.NumActive();
  cursor += sizeof(TopkWireHeader);

  std::memcpy(cursor, sketch.GetCellArray(), sketch.GetCellDataSize());
  cursor += sketch.GetCellDataSize();

  const auto* entries = sketch.GetEntries();
  for (uint32_t i = 0; i < sketch.NumActive(); i++) {
    auto* entryHdr = reinterpret_cast<TopkWireEntryHeader*>(cursor);
    entryHdr->count = entries[i].count;
    entryHdr->nameLen = entries[i].nameLen;
    cursor += sizeof(TopkWireEntryHeader);
    std::memcpy(cursor, entries[i].name, entries[i].nameLen);
    cursor += entries[i].nameLen;
  }

  return static_cast<size_t>(cursor - static_cast<char*>(output));
}

TopkSketch* TopkDeserializeChunk(const void* data, size_t length) {
  if (length < sizeof(TopkWireHeader)) return nullptr;

  const auto* hdr = static_cast<const TopkWireHeader*>(data);
  const char* cursor = static_cast<const char*>(data) + sizeof(TopkWireHeader);
  size_t remaining = length - sizeof(TopkWireHeader);

  if (remaining < static_cast<uint64_t>(hdr->width) * hdr->depth * sizeof(TopkSketch::Cell)) {
    return nullptr;
  }
  uint64_t cellDataSize = static_cast<uint64_t>(hdr->width) * hdr->depth * sizeof(TopkSketch::Cell);
  if (!ValidateTopkFields(hdr->k, hdr->width, hdr->depth, hdr->decay, cellDataSize, hdr->numActive)) {
    return nullptr;
  }

  auto maybeSketch = TopkSketch::Create(hdr->k, hdr->width, hdr->depth, hdr->decay);
  if (!maybeSketch) return nullptr;

  std::memcpy(maybeSketch->GetCellArray(), cursor, cellDataSize);
  cursor += cellDataSize;
  remaining -= cellDataSize;

  uint64_t prevCount = UINT64_MAX;
  for (uint32_t i = 0; i < hdr->numActive; i++) {
    if (remaining < sizeof(TopkWireEntryHeader)) return nullptr;
    const auto* entryHdr = reinterpret_cast<const TopkWireEntryHeader*>(cursor);
    cursor += sizeof(TopkWireEntryHeader);
    remaining -= sizeof(TopkWireEntryHeader);

    if (entryHdr->nameLen > kTopkMaxNameLen || entryHdr->count > prevCount || remaining < entryHdr->nameLen) {
      return nullptr;
    }
    prevCount = entryHdr->count;
    maybeSketch->AppendEntryForLoad(AsBytes(cursor, entryHdr->nameLen), entryHdr->count);
    cursor += entryHdr->nameLen;
    remaining -= entryHdr->nameLen;
  }

  if (remaining != 0) return nullptr;

  auto* sketch = static_cast<TopkSketch*>(RMAlloc(sizeof(TopkSketch)));
  if (!sketch) return nullptr;
  new (sketch) TopkSketch(std::move(*maybeSketch));
  return sketch;
}

// --- Module type callbacks ---

RedisModuleType* TopkType = nullptr;

void* RdbLoadTopk(RedisModuleIO* rdb, int encver) {
  if (encver != kTopkEncVerCurrent) return nullptr;
  RdbReader reader(rdb);
  return ReadTopkSketch(reader);
}

void RdbSaveTopk(RedisModuleIO* rdb, void* value) {
  RdbWriter writer(rdb);
  WriteTopkSketch(*static_cast<TopkSketch*>(value), writer);
}

void AofRewriteTopk(RedisModuleIO* aof, RedisModuleString* key, void* value) {
  auto* sketch = static_cast<TopkSketch*>(value);

  size_t chunkBytes = TopkComputeChunkSize(*sketch);
  auto* buf = static_cast<char*>(RMAlloc(chunkBytes));
  if (!buf) {
    RedisModule_LogIOError(aof, "warning",
      "GeminiBloom: TopK AOF rewrite allocation failure, key omitted");
    return;
  }
  TopkSerializeChunk(*sketch, buf);
  RedisModule_EmitAOF(aof, "TOPK.LOADCHUNK", "sb", key, buf, chunkBytes);
  RMFree(buf);
}

void FreeTopk(void* value) {
  if (auto* sketch = static_cast<TopkSketch*>(value)) {
    sketch->~TopkSketch();
    RMFree(sketch);
  }
}

size_t TopkMemUsage(const void* value) {
  if (!value) return 0;
  auto* sketch = static_cast<const TopkSketch*>(value);
  size_t size = sizeof(TopkSketch) + sketch->GetCellDataSize() + sketch->K() * sizeof(TopkSketch::Entry);
  const auto* entries = sketch->GetEntries();
  for (uint32_t i = 0; i < sketch->NumActive(); i++) size += entries[i].nameLen;
  return size;
}

void DigestTopk(RedisModuleDigest* digest, void* value) {
  auto* sketch = static_cast<TopkSketch*>(value);

  RedisModule_DigestAddLongLong(digest, static_cast<long long>(sketch->K()));
  RedisModule_DigestAddLongLong(digest, static_cast<long long>(sketch->Width()));
  RedisModule_DigestAddLongLong(digest, static_cast<long long>(sketch->Depth()));
  RedisModule_DigestAddStringBuffer(digest,
    reinterpret_cast<const char*>(sketch->GetCellArray()), sketch->GetCellDataSize());

  const auto* entries = sketch->GetEntries();
  for (uint32_t i = 0; i < sketch->NumActive(); i++) {
    RedisModule_DigestAddStringBuffer(digest, entries[i].name, entries[i].nameLen);
    RedisModule_DigestAddLongLong(digest, static_cast<long long>(entries[i].count));
  }

  RedisModule_DigestEndSequence(digest);
}

void* CopyTopk2(RedisModuleKeyOptCtx* ctx, const void* value) {
  (void)ctx;
  auto maybeCopy = static_cast<const TopkSketch*>(value)->Clone();
  if (!maybeCopy) return nullptr;
  auto* sketch = static_cast<TopkSketch*>(RMAlloc(sizeof(TopkSketch)));
  if (!sketch) return nullptr;
  new (sketch) TopkSketch(std::move(*maybeCopy));
  return sketch;
}

size_t FreeEffortTopk2(RedisModuleKeyOptCtx* ctx, const void* value) {
  (void)ctx;
  (void)value;
  return 3;  // sketch object + cell table + entry table
}

int DefragTopk(RedisModuleDefragCtx* ctx, RedisModuleString* key, void** value) {
  (void)key;
  auto* sketch = static_cast<TopkSketch*>(*value);

  if (auto* relocated = static_cast<TopkSketch*>(RedisModule_DefragAlloc(ctx, sketch))) {
    sketch = relocated;
    *value = sketch;
  }

  if (auto* relocatedCells =
        static_cast<TopkSketch::Cell*>(RedisModule_DefragAlloc(ctx, sketch->GetCellArray()))) {
    sketch->AdoptCellArray(relocatedCells);
  }

  if (auto* relocatedEntries =
        static_cast<TopkSketch::Entry*>(RedisModule_DefragAlloc(ctx, sketch->GetEntries()))) {
    sketch->AdoptEntryArray(relocatedEntries);
  }

  return 0;
}
