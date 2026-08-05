#include "tdigest_rdb.h"
#include "bloom_rdb.h"
#include "bloom_filter.h"
#include "tdigest_sketch.h"
#include "rm_alloc.h"

#include <cmath>
#include <cstring>

// --- Field validation ---
// Rejects states that would cause UB (divide-by-zero, out-of-bounds) or
// silent corruption, whether coming from RDB load or LOADCHUNK.

static bool ValidateTdigestFields(double compression, uint64_t numCentroids, uint64_t numBuffered,
                                   double mergedWeight, double unmergedWeight, double rawMin, double rawMax) {
  if (!std::isfinite(compression) || compression < kTdigestMinCompression ||
      compression > kTdigestMaxCompression) {
    return false;
  }
  if (numCentroids > kTdigestMaxCentroids || numBuffered > kTdigestMaxCentroids) return false;
  if (!std::isfinite(mergedWeight) || mergedWeight < 0.0) return false;
  if (!std::isfinite(unmergedWeight) || unmergedWeight < 0.0) return false;
  bool empty = (mergedWeight + unmergedWeight) == 0.0;
  if (empty) {
    if (numCentroids != 0 || numBuffered != 0) return false;
  } else {
    if (!std::isfinite(rawMin) || !std::isfinite(rawMax) || rawMin > rawMax) return false;
  }
  return true;
}

// --- TdigestSketch RDB serialization ---
// Field order: compression, mergedWeight, unmergedWeight, min, max,
// numCompressions, numCentroids, centroid blob (mean/weight pairs),
// numBuffered, buffer blob (mean/weight pairs). This is this module's own
// RDB wire-format protocol (command-level compatibility only; no byte
// format compatibility with any external T-Digest sketch is required).

static void WriteTdigestSketch(const TdigestSketch& sketch, RdbWriter& w) {
  w.PutFloat(sketch.Compression());
  w.PutFloat(sketch.MergedWeight());
  w.PutFloat(sketch.UnmergedWeight());
  w.PutFloat(sketch.Min());
  w.PutFloat(sketch.Max());
  w.PutUint(sketch.NumCompressions());
  w.PutUint(sketch.NumCentroids());
  w.PutBlob(reinterpret_cast<const uint8_t*>(sketch.GetCentroidArray()),
            static_cast<uint64_t>(sketch.NumCentroids()) * sizeof(TdigestSketch::Centroid));
  w.PutUint(sketch.NumBuffered());
  w.PutBlob(reinterpret_cast<const uint8_t*>(sketch.GetBufferArray()),
            static_cast<uint64_t>(sketch.NumBuffered()) * sizeof(TdigestSketch::Centroid));
}

static TdigestSketch* ReadTdigestSketch(RdbReader& r) {
  double compression = r.GetFloat();
  double mergedWeight = r.GetFloat();
  double unmergedWeight = r.GetFloat();
  double rawMin = r.GetFloat();
  double rawMax = r.GetFloat();
  uint64_t numCompressions = r.GetUint();
  uint64_t rawNumCentroids = r.GetUint();

  if (!r.Ok()) return nullptr;
  if (rawNumCentroids > UINT32_MAX) return nullptr;

  auto [centroidBuf, centroidBufLen] = r.GetBlob();
  if (!r.Ok()) {
    if (centroidBuf) RedisModule_Free(centroidBuf);
    return nullptr;
  }
  uint64_t rawNumBuffered = r.GetUint();
  if (!r.Ok() || rawNumBuffered > UINT32_MAX) {
    if (centroidBuf) RedisModule_Free(centroidBuf);
    return nullptr;
  }
  auto [bufferBuf, bufferBufLen] = r.GetBlob();
  if (!r.Ok()) {
    if (centroidBuf) RedisModule_Free(centroidBuf);
    if (bufferBuf) RedisModule_Free(bufferBuf);
    return nullptr;
  }

  if (static_cast<uint64_t>(centroidBufLen) != rawNumCentroids * sizeof(TdigestSketch::Centroid) ||
      static_cast<uint64_t>(bufferBufLen) != rawNumBuffered * sizeof(TdigestSketch::Centroid) ||
      !ValidateTdigestFields(compression, rawNumCentroids, rawNumBuffered, mergedWeight, unmergedWeight,
                              rawMin, rawMax)) {
    if (centroidBuf) RedisModule_Free(centroidBuf);
    if (bufferBuf) RedisModule_Free(bufferBuf);
    return nullptr;
  }

  auto maybeSketch = TdigestSketch::Create(compression);
  if (!maybeSketch) {
    if (centroidBuf) RedisModule_Free(centroidBuf);
    if (bufferBuf) RedisModule_Free(bufferBuf);
    return nullptr;
  }
  if (!maybeSketch->AllocateForLoad(static_cast<uint32_t>(rawNumCentroids),
                                     static_cast<uint32_t>(rawNumBuffered))) {
    if (centroidBuf) RedisModule_Free(centroidBuf);
    if (bufferBuf) RedisModule_Free(bufferBuf);
    return nullptr;
  }

  const auto* centroidPairs = reinterpret_cast<const TdigestSketch::Centroid*>(centroidBuf);
  for (uint64_t i = 0; i < rawNumCentroids; i++) {
    if (!std::isfinite(centroidPairs[i].mean) || !std::isfinite(centroidPairs[i].weight) ||
        centroidPairs[i].weight <= 0.0) {
      if (centroidBuf) RedisModule_Free(centroidBuf);
      if (bufferBuf) RedisModule_Free(bufferBuf);
      return nullptr;
    }
    maybeSketch->AppendCentroidForLoad(centroidPairs[i].mean, centroidPairs[i].weight);
  }
  const auto* bufferPairs = reinterpret_cast<const TdigestSketch::Centroid*>(bufferBuf);
  for (uint64_t i = 0; i < rawNumBuffered; i++) {
    if (!std::isfinite(bufferPairs[i].mean) || !std::isfinite(bufferPairs[i].weight) ||
        bufferPairs[i].weight <= 0.0) {
      if (centroidBuf) RedisModule_Free(centroidBuf);
      if (bufferBuf) RedisModule_Free(bufferBuf);
      return nullptr;
    }
    maybeSketch->AppendBufferedForLoad(bufferPairs[i].mean, bufferPairs[i].weight);
  }
  if (centroidBuf) RedisModule_Free(centroidBuf);
  if (bufferBuf) RedisModule_Free(bufferBuf);

  maybeSketch->SetBookkeepingForLoad(mergedWeight, unmergedWeight, rawMin, rawMax, numCompressions);

  auto* sketch = static_cast<TdigestSketch*>(RMAlloc(sizeof(TdigestSketch)));
  if (!sketch) return nullptr;
  new (sketch) TdigestSketch(std::move(*maybeSketch));
  return sketch;
}

// --- LOADCHUNK wire format ---

#pragma pack(push, 1)
struct TdigestWireHeader {
  double compression;
  double mergedWeight;
  double unmergedWeight;
  double rawMin;
  double rawMax;
  uint64_t numCompressions;
  uint32_t numCentroids;
  uint32_t numBuffered;
};
#pragma pack(pop)

size_t TdigestComputeChunkSize(const TdigestSketch& sketch) {
  return sizeof(TdigestWireHeader) +
         static_cast<size_t>(sketch.NumCentroids()) * sizeof(TdigestSketch::Centroid) +
         static_cast<size_t>(sketch.NumBuffered()) * sizeof(TdigestSketch::Centroid);
}

size_t TdigestSerializeChunk(const TdigestSketch& sketch, void* output) {
  auto* cursor = static_cast<char*>(output);

  auto* hdr = reinterpret_cast<TdigestWireHeader*>(cursor);
  hdr->compression = sketch.Compression();
  hdr->mergedWeight = sketch.MergedWeight();
  hdr->unmergedWeight = sketch.UnmergedWeight();
  hdr->rawMin = sketch.Min();
  hdr->rawMax = sketch.Max();
  hdr->numCompressions = sketch.NumCompressions();
  hdr->numCentroids = sketch.NumCentroids();
  hdr->numBuffered = sketch.NumBuffered();
  cursor += sizeof(TdigestWireHeader);

  size_t centroidBytes = static_cast<size_t>(sketch.NumCentroids()) * sizeof(TdigestSketch::Centroid);
  std::memcpy(cursor, sketch.GetCentroidArray(), centroidBytes);
  cursor += centroidBytes;

  size_t bufferBytes = static_cast<size_t>(sketch.NumBuffered()) * sizeof(TdigestSketch::Centroid);
  std::memcpy(cursor, sketch.GetBufferArray(), bufferBytes);
  cursor += bufferBytes;

  return static_cast<size_t>(cursor - static_cast<char*>(output));
}

TdigestSketch* TdigestDeserializeChunk(const void* data, size_t length) {
  if (length < sizeof(TdigestWireHeader)) return nullptr;

  const auto* hdr = static_cast<const TdigestWireHeader*>(data);
  const char* cursor = static_cast<const char*>(data) + sizeof(TdigestWireHeader);
  size_t remaining = length - sizeof(TdigestWireHeader);

  if (!ValidateTdigestFields(hdr->compression, hdr->numCentroids, hdr->numBuffered, hdr->mergedWeight,
                              hdr->unmergedWeight, hdr->rawMin, hdr->rawMax)) {
    return nullptr;
  }

  size_t centroidBytes = static_cast<size_t>(hdr->numCentroids) * sizeof(TdigestSketch::Centroid);
  if (remaining < centroidBytes) return nullptr;
  const auto* centroidPairs = reinterpret_cast<const TdigestSketch::Centroid*>(cursor);
  cursor += centroidBytes;
  remaining -= centroidBytes;

  size_t bufferBytes = static_cast<size_t>(hdr->numBuffered) * sizeof(TdigestSketch::Centroid);
  if (remaining < bufferBytes) return nullptr;
  const auto* bufferPairs = reinterpret_cast<const TdigestSketch::Centroid*>(cursor);
  cursor += bufferBytes;
  remaining -= bufferBytes;

  if (remaining != 0) return nullptr;

  auto maybeSketch = TdigestSketch::Create(hdr->compression);
  if (!maybeSketch) return nullptr;
  if (!maybeSketch->AllocateForLoad(hdr->numCentroids, hdr->numBuffered)) return nullptr;

  for (uint32_t i = 0; i < hdr->numCentroids; i++) {
    if (!std::isfinite(centroidPairs[i].mean) || !std::isfinite(centroidPairs[i].weight) ||
        centroidPairs[i].weight <= 0.0) {
      return nullptr;
    }
    maybeSketch->AppendCentroidForLoad(centroidPairs[i].mean, centroidPairs[i].weight);
  }
  for (uint32_t i = 0; i < hdr->numBuffered; i++) {
    if (!std::isfinite(bufferPairs[i].mean) || !std::isfinite(bufferPairs[i].weight) ||
        bufferPairs[i].weight <= 0.0) {
      return nullptr;
    }
    maybeSketch->AppendBufferedForLoad(bufferPairs[i].mean, bufferPairs[i].weight);
  }

  maybeSketch->SetBookkeepingForLoad(hdr->mergedWeight, hdr->unmergedWeight, hdr->rawMin, hdr->rawMax,
                                      hdr->numCompressions);

  auto* sketch = static_cast<TdigestSketch*>(RMAlloc(sizeof(TdigestSketch)));
  if (!sketch) return nullptr;
  new (sketch) TdigestSketch(std::move(*maybeSketch));
  return sketch;
}

// --- Module type callbacks ---

RedisModuleType* TdigestType = nullptr;

void* RdbLoadTdigest(RedisModuleIO* rdb, int encver) {
  if (encver != kTdigestEncVerCurrent) return nullptr;
  RdbReader reader(rdb);
  return ReadTdigestSketch(reader);
}

void RdbSaveTdigest(RedisModuleIO* rdb, void* value) {
  RdbWriter writer(rdb);
  WriteTdigestSketch(*static_cast<TdigestSketch*>(value), writer);
}

void AofRewriteTdigest(RedisModuleIO* aof, RedisModuleString* key, void* value) {
  auto* sketch = static_cast<TdigestSketch*>(value);

  size_t chunkBytes = TdigestComputeChunkSize(*sketch);
  auto* buf = static_cast<char*>(RMAlloc(chunkBytes));
  if (!buf) {
    RedisModule_LogIOError(aof, "warning",
      "GeminiBloom: T-Digest AOF rewrite allocation failure, key omitted");
    return;
  }
  TdigestSerializeChunk(*sketch, buf);
  RedisModule_EmitAOF(aof, "TDIGEST.LOADCHUNK", "sb", key, buf, chunkBytes);
  RMFree(buf);
}

void FreeTdigest(void* value) {
  if (auto* sketch = static_cast<TdigestSketch*>(value)) {
    sketch->~TdigestSketch();
    RMFree(sketch);
  }
}

size_t TdigestMemUsage(const void* value) {
  if (!value) return 0;
  auto* sketch = static_cast<const TdigestSketch*>(value);
  return sizeof(TdigestSketch) +
         static_cast<size_t>(sketch->NumCentroids()) * sizeof(TdigestSketch::Centroid) +
         static_cast<size_t>(sketch->BufferCapacity()) * sizeof(TdigestSketch::Centroid);
}

void DigestTdigest(RedisModuleDigest* digest, void* value) {
  auto* sketch = static_cast<TdigestSketch*>(value);

  RedisModule_DigestAddLongLong(digest, static_cast<long long>(sketch->Compression() * 1000.0));
  RedisModule_DigestAddStringBuffer(digest,
    reinterpret_cast<const char*>(sketch->GetCentroidArray()),
    static_cast<size_t>(sketch->NumCentroids()) * sizeof(TdigestSketch::Centroid));
  RedisModule_DigestAddStringBuffer(digest,
    reinterpret_cast<const char*>(sketch->GetBufferArray()),
    static_cast<size_t>(sketch->NumBuffered()) * sizeof(TdigestSketch::Centroid));

  RedisModule_DigestEndSequence(digest);
}

void* CopyTdigest2(RedisModuleKeyOptCtx* ctx, const void* value) {
  (void)ctx;
  auto maybeCopy = static_cast<const TdigestSketch*>(value)->Clone();
  if (!maybeCopy) return nullptr;
  auto* sketch = static_cast<TdigestSketch*>(RMAlloc(sizeof(TdigestSketch)));
  if (!sketch) return nullptr;
  new (sketch) TdigestSketch(std::move(*maybeCopy));
  return sketch;
}

size_t FreeEffortTdigest2(RedisModuleKeyOptCtx* ctx, const void* value) {
  (void)ctx;
  (void)value;
  return 3;  // sketch object + centroid array + buffer array
}

int DefragTdigest(RedisModuleDefragCtx* ctx, RedisModuleString* key, void** value) {
  (void)key;
  auto* sketch = static_cast<TdigestSketch*>(*value);

  if (auto* relocated = static_cast<TdigestSketch*>(RedisModule_DefragAlloc(ctx, sketch))) {
    sketch = relocated;
    *value = sketch;
  }

  if (auto* relocatedCentroids =
        static_cast<TdigestSketch::Centroid*>(RedisModule_DefragAlloc(ctx, sketch->GetCentroidArray()))) {
    sketch->AdoptCentroidArray(relocatedCentroids);
  }

  if (auto* relocatedBuffer =
        static_cast<TdigestSketch::Centroid*>(RedisModule_DefragAlloc(ctx, sketch->GetBufferArray()))) {
    sketch->AdoptBufferArray(relocatedBuffer);
  }

  return 0;
}
