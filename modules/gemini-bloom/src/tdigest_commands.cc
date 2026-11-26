#include "tdigest_commands.h"
#include "tdigest_rdb.h"
#include "tdigest_sketch.h"
#include "rm_alloc.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <strings.h>
#include <string_view>

namespace {
template <typename T, size_t InlineCap = 32>
class SmallBuf {
public:
  SmallBuf() = default;
  ~SmallBuf() {
    if (heap_) RMFree(heap_);
  }
  SmallBuf(const SmallBuf&) = delete;
  SmallBuf& operator=(const SmallBuf&) = delete;

  bool Init(size_t n) {
    size_ = n;
    if (n <= InlineCap) {
      ptr_ = stack_;
      return true;
    }
    if (n > SIZE_MAX / sizeof(T)) return false;
    heap_ = static_cast<T*>(RMAlloc(n * sizeof(T)));
    if (!heap_) return false;
    ptr_ = heap_;
    return true;
  }

  T* data() { return ptr_; }
  size_t size() const { return size_; }
  T& operator[](size_t i) { return ptr_[i]; }

private:
  T stack_[InlineCap];
  T* heap_ = nullptr;
  T* ptr_ = stack_;
  size_t size_ = 0;
};
}  // namespace

// --- Helpers ---

static TdigestSketch* GetSketch(RedisModuleKey* key) {
  if (RedisModule_ModuleTypeGetType(key) != TdigestType) return nullptr;
  return static_cast<TdigestSketch*>(RedisModule_ModuleTypeGetValue(key));
}

static bool MatchArg(std::string_view arg, std::string_view target) {
  if (arg.size() != target.size()) return false;
  return strncasecmp(arg.data(), target.data(), arg.size()) == 0;
}

static TdigestSketch* SetKeyValue(RedisModuleCtx* ctx, RedisModuleKey* key,
                                   std::optional<TdigestSketch>&& maybeSketch) {
  if (!maybeSketch) {
    RedisModule_ReplyWithError(ctx, "ERR allocation failure");
    return nullptr;
  }
  auto* sketch = static_cast<TdigestSketch*>(RMAlloc(sizeof(TdigestSketch)));
  if (!sketch) {
    RedisModule_ReplyWithError(ctx, "ERR allocation failure");
    return nullptr;
  }
  new (sketch) TdigestSketch(std::move(*maybeSketch));
  if (RedisModule_ModuleTypeSetValue(key, TdigestType, sketch) != REDISMODULE_OK) {
    sketch->~TdigestSketch();
    RMFree(sketch);
    RedisModule_ReplyWithError(ctx, "ERR failed to set key value");
    return nullptr;
  }
  return sketch;
}

// --- TDIGEST.CREATE ---
static int CmdCreate(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc != 2 && argc != 4) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  double compression = kTdigestDefaultCompression;
  if (argc == 4) {
    size_t len;
    const char* opt = RedisModule_StringPtrLen(argv[2], &len);
    if (!MatchArg({opt, len}, "COMPRESSION")) {
      return RedisModule_ReplyWithError(ctx, "ERR unrecognized option");
    }
    if (RedisModule_StringToDouble(argv[3], &compression) != REDISMODULE_OK) {
      return RedisModule_ReplyWithError(ctx, "ERR invalid compression");
    }
  }

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE));
  if (RedisModule_KeyType(key) != REDISMODULE_KEYTYPE_EMPTY) {
    return RedisModule_ReplyWithError(ctx, "ERR key already exists");
  }

  auto maybeSketch = TdigestSketch::Create(compression);
  if (!maybeSketch) {
    return RedisModule_ReplyWithError(ctx, "ERR invalid compression");
  }
  if (!SetKeyValue(ctx, key, std::move(maybeSketch))) return REDISMODULE_OK;

  RedisModule_ReplicateVerbatim(ctx);
  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// --- TDIGEST.ADD ---
static int CmdAdd(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc < 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE));

  if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) {
    return RedisModule_ReplyWithError(ctx, "ERR key does not exist");
  }
  auto* sketch = GetSketch(key);
  if (!sketch) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

  size_t numValues = static_cast<size_t>(argc - 2);
  SmallBuf<double> values;
  if (!values.Init(numValues)) {
    return RedisModule_ReplyWithError(ctx, "ERR allocation failure");
  }
  for (size_t i = 0; i < numValues; i++) {
    if (RedisModule_StringToDouble(argv[2 + i], &values[i]) != REDISMODULE_OK ||
        !std::isfinite(values[i])) {
      return RedisModule_ReplyWithError(ctx, "ERR invalid value");
    }
  }

  for (size_t i = 0; i < numValues; i++) {
    if (!sketch->Add(values[i])) {
      return RedisModule_ReplyWithError(ctx, "ERR allocation failure");
    }
  }

  RedisModule_ReplicateVerbatim(ctx);
  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// --- TDIGEST.QUANTILE ---
static int CmdQuantile(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc < 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ));

  if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) {
    return RedisModule_ReplyWithError(ctx, "ERR key does not exist");
  }
  auto* sketch = GetSketch(key);
  if (!sketch) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

  size_t count = static_cast<size_t>(argc - 2);
  SmallBuf<double> qs;
  if (!qs.Init(count)) return RedisModule_ReplyWithError(ctx, "ERR allocation failure");
  for (size_t i = 0; i < count; i++) {
    if (RedisModule_StringToDouble(argv[2 + i], &qs[i]) != REDISMODULE_OK) {
      return RedisModule_ReplyWithError(ctx, "ERR invalid quantile");
    }
  }

  RedisModule_ReplyWithArray(ctx, static_cast<long>(count));
  for (size_t i = 0; i < count; i++) {
    RedisModule_ReplyWithDouble(ctx, sketch->Quantile(qs[i]));
  }
  return REDISMODULE_OK;
}

// --- TDIGEST.CDF ---
static int CmdCdf(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc < 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ));

  if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) {
    return RedisModule_ReplyWithError(ctx, "ERR key does not exist");
  }
  auto* sketch = GetSketch(key);
  if (!sketch) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

  size_t count = static_cast<size_t>(argc - 2);
  SmallBuf<double> values;
  if (!values.Init(count)) return RedisModule_ReplyWithError(ctx, "ERR allocation failure");
  for (size_t i = 0; i < count; i++) {
    if (RedisModule_StringToDouble(argv[2 + i], &values[i]) != REDISMODULE_OK) {
      return RedisModule_ReplyWithError(ctx, "ERR invalid value");
    }
  }

  RedisModule_ReplyWithArray(ctx, static_cast<long>(count));
  for (size_t i = 0; i < count; i++) {
    RedisModule_ReplyWithDouble(ctx, sketch->Cdf(values[i]));
  }
  return REDISMODULE_OK;
}

// --- TDIGEST.RANK ---
static int CmdRank(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc < 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ));

  if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) {
    return RedisModule_ReplyWithError(ctx, "ERR key does not exist");
  }
  auto* sketch = GetSketch(key);
  if (!sketch) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

  size_t count = static_cast<size_t>(argc - 2);
  SmallBuf<double> values;
  if (!values.Init(count)) return RedisModule_ReplyWithError(ctx, "ERR allocation failure");
  for (size_t i = 0; i < count; i++) {
    if (RedisModule_StringToDouble(argv[2 + i], &values[i]) != REDISMODULE_OK) {
      return RedisModule_ReplyWithError(ctx, "ERR invalid value");
    }
  }

  RedisModule_ReplyWithArray(ctx, static_cast<long>(count));
  for (size_t i = 0; i < count; i++) {
    RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(sketch->Rank(values[i])));
  }
  return REDISMODULE_OK;
}

// --- TDIGEST.REVRANK ---
static int CmdRevrank(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc < 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ));

  if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) {
    return RedisModule_ReplyWithError(ctx, "ERR key does not exist");
  }
  auto* sketch = GetSketch(key);
  if (!sketch) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

  size_t count = static_cast<size_t>(argc - 2);
  SmallBuf<double> values;
  if (!values.Init(count)) return RedisModule_ReplyWithError(ctx, "ERR allocation failure");
  for (size_t i = 0; i < count; i++) {
    if (RedisModule_StringToDouble(argv[2 + i], &values[i]) != REDISMODULE_OK) {
      return RedisModule_ReplyWithError(ctx, "ERR invalid value");
    }
  }

  RedisModule_ReplyWithArray(ctx, static_cast<long>(count));
  for (size_t i = 0; i < count; i++) {
    RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(sketch->RevRank(values[i])));
  }
  return REDISMODULE_OK;
}

// --- TDIGEST.BYRANK ---
static int CmdByrank(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc < 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ));

  if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) {
    return RedisModule_ReplyWithError(ctx, "ERR key does not exist");
  }
  auto* sketch = GetSketch(key);
  if (!sketch) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

  size_t count = static_cast<size_t>(argc - 2);
  SmallBuf<long long> ranks;
  if (!ranks.Init(count)) return RedisModule_ReplyWithError(ctx, "ERR allocation failure");
  for (size_t i = 0; i < count; i++) {
    if (RedisModule_StringToLongLong(argv[2 + i], &ranks[i]) != REDISMODULE_OK) {
      return RedisModule_ReplyWithError(ctx, "ERR invalid rank");
    }
  }

  RedisModule_ReplyWithArray(ctx, static_cast<long>(count));
  for (size_t i = 0; i < count; i++) {
    RedisModule_ReplyWithDouble(ctx, sketch->ByRank(static_cast<double>(ranks[i])));
  }
  return REDISMODULE_OK;
}

// --- TDIGEST.BYREVRANK ---
static int CmdByrevrank(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc < 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ));

  if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) {
    return RedisModule_ReplyWithError(ctx, "ERR key does not exist");
  }
  auto* sketch = GetSketch(key);
  if (!sketch) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

  size_t count = static_cast<size_t>(argc - 2);
  SmallBuf<long long> ranks;
  if (!ranks.Init(count)) return RedisModule_ReplyWithError(ctx, "ERR allocation failure");
  for (size_t i = 0; i < count; i++) {
    if (RedisModule_StringToLongLong(argv[2 + i], &ranks[i]) != REDISMODULE_OK) {
      return RedisModule_ReplyWithError(ctx, "ERR invalid rank");
    }
  }

  RedisModule_ReplyWithArray(ctx, static_cast<long>(count));
  for (size_t i = 0; i < count; i++) {
    RedisModule_ReplyWithDouble(ctx, sketch->ByRevRank(static_cast<double>(ranks[i])));
  }
  return REDISMODULE_OK;
}

// --- TDIGEST.TRIMMED_MEAN ---
static int CmdTrimmedMean(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc != 4) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ));

  if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) {
    return RedisModule_ReplyWithError(ctx, "ERR key does not exist");
  }
  auto* sketch = GetSketch(key);
  if (!sketch) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

  double lowQ, highQ;
  if (RedisModule_StringToDouble(argv[2], &lowQ) != REDISMODULE_OK) {
    return RedisModule_ReplyWithError(ctx, "ERR invalid low cut quantile");
  }
  if (RedisModule_StringToDouble(argv[3], &highQ) != REDISMODULE_OK) {
    return RedisModule_ReplyWithError(ctx, "ERR invalid high cut quantile");
  }

  return RedisModule_ReplyWithDouble(ctx, sketch->TrimmedMean(lowQ, highQ));
}

// --- TDIGEST.MIN / TDIGEST.MAX ---
static int CmdMin(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc != 2) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* key = static_cast<RedisModuleKey*>(RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ));

  if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) {
    return RedisModule_ReplyWithError(ctx, "ERR key does not exist");
  }
  auto* sketch = GetSketch(key);
  if (!sketch) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

  return RedisModule_ReplyWithDouble(ctx, sketch->Min());
}

static int CmdMax(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc != 2) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* key = static_cast<RedisModuleKey*>(RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ));

  if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) {
    return RedisModule_ReplyWithError(ctx, "ERR key does not exist");
  }
  auto* sketch = GetSketch(key);
  if (!sketch) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

  return RedisModule_ReplyWithDouble(ctx, sketch->Max());
}

// --- TDIGEST.RESET ---
static int CmdReset(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc != 2) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE));

  if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) {
    return RedisModule_ReplyWithError(ctx, "ERR key does not exist");
  }
  auto* sketch = GetSketch(key);
  if (!sketch) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

  sketch->Reset();

  RedisModule_ReplicateVerbatim(ctx);
  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// --- TDIGEST.MERGE ---
static int CmdMerge(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc < 4) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  long long numKeys;
  if (RedisModule_StringToLongLong(argv[2], &numKeys) != REDISMODULE_OK || numKeys <= 0) {
    return RedisModule_ReplyWithError(ctx, "ERR invalid numKeys");
  }

  int sourcesStart = 3;
  if (numKeys > argc - sourcesStart) return RedisModule_WrongArity(ctx);
  int cursor = sourcesStart + static_cast<int>(numKeys);

  bool override = false;
  bool haveCompression = false;
  double explicitCompression = kTdigestDefaultCompression;

  while (cursor < argc) {
    size_t len;
    const char* opt = RedisModule_StringPtrLen(argv[cursor], &len);
    if (MatchArg({opt, len}, "OVERRIDE")) {
      override = true;
      cursor++;
    } else if (MatchArg({opt, len}, "COMPRESSION")) {
      if (cursor + 1 >= argc) return RedisModule_WrongArity(ctx);
      if (RedisModule_StringToDouble(argv[cursor + 1], &explicitCompression) != REDISMODULE_OK) {
        return RedisModule_ReplyWithError(ctx, "ERR invalid compression");
      }
      haveCompression = true;
      cursor += 2;
    } else {
      return RedisModule_ReplyWithError(ctx, "ERR unrecognized option");
    }
  }

  SmallBuf<const TdigestSketch*> sources;
  if (!sources.Init(static_cast<size_t>(numKeys))) {
    return RedisModule_ReplyWithError(ctx, "ERR allocation failure");
  }
  double maxSourceCompression = 0.0;
  for (long long i = 0; i < numKeys; i++) {
    auto* srcKey = static_cast<RedisModuleKey*>(
      RedisModule_OpenKey(ctx, argv[sourcesStart + i], REDISMODULE_READ));
    if (RedisModule_KeyType(srcKey) == REDISMODULE_KEYTYPE_EMPTY) {
      return RedisModule_ReplyWithError(ctx, "ERR key does not exist");
    }
    auto* src = GetSketch(srcKey);
    if (!src) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
    sources[static_cast<size_t>(i)] = src;
    maxSourceCompression = std::max(maxSourceCompression, src->Compression());
  }

  auto* destKey = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE));
  bool destExists = RedisModule_KeyType(destKey) != REDISMODULE_KEYTYPE_EMPTY;

  if (!destExists) {
    double compression = haveCompression ? explicitCompression : maxSourceCompression;
    auto maybeSketch = TdigestSketch::Create(compression);
    if (!maybeSketch) return RedisModule_ReplyWithError(ctx, "ERR invalid compression");
    if (!maybeSketch->Merge({sources.data(), static_cast<size_t>(numKeys)})) {
      return RedisModule_ReplyWithError(ctx, "ERR allocation failure");
    }
    if (!SetKeyValue(ctx, destKey, std::move(maybeSketch))) return REDISMODULE_OK;
  } else if (override) {
    auto* dest = GetSketch(destKey);
    if (!dest) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
    double compression = haveCompression ? explicitCompression : maxSourceCompression;
    // Build into a fresh sketch so self-aliasing (dest also listed in sources)
    // does not wipe dest before reading its centroids/buffer.
    auto maybeFresh = TdigestSketch::Create(compression);
    if (!maybeFresh) return RedisModule_ReplyWithError(ctx, "ERR invalid compression");
    if (!maybeFresh->Merge({sources.data(), static_cast<size_t>(numKeys)})) {
      return RedisModule_ReplyWithError(ctx, "ERR allocation failure");
    }
    *dest = std::move(*maybeFresh);
  } else {
    auto* dest = GetSketch(destKey);
    if (!dest) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
    double savedCompression = dest->Compression();
    if (haveCompression && !dest->SetCompression(explicitCompression)) {
      return RedisModule_ReplyWithError(ctx, "ERR invalid compression");
    }
    if (!dest->Merge({sources.data(), static_cast<size_t>(numKeys)})) {
      (void)dest->SetCompression(savedCompression);
      return RedisModule_ReplyWithError(ctx, "ERR allocation failure");
    }
  }

  RedisModule_ReplicateVerbatim(ctx);
  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// --- TDIGEST.INFO ---
static int CmdInfo(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc != 2) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* key = static_cast<RedisModuleKey*>(RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ));

  if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) {
    return RedisModule_ReplyWithError(ctx, "ERR key does not exist");
  }
  auto* sketch = GetSketch(key);
  if (!sketch) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

  RedisModule_ReplyWithArray(ctx, 18);
  RedisModule_ReplyWithSimpleString(ctx, "Compression");
  RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(sketch->Compression()));
  RedisModule_ReplyWithSimpleString(ctx, "Capacity");
  RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(sketch->CentroidCapacity()));
  RedisModule_ReplyWithSimpleString(ctx, "Merged nodes");
  RedisModule_ReplyWithLongLong(ctx, sketch->NumCentroids());
  RedisModule_ReplyWithSimpleString(ctx, "Unmerged nodes");
  RedisModule_ReplyWithLongLong(ctx, sketch->NumBuffered());
  RedisModule_ReplyWithSimpleString(ctx, "Merged weight");
  RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(sketch->MergedWeight()));
  RedisModule_ReplyWithSimpleString(ctx, "Unmerged weight");
  RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(sketch->UnmergedWeight()));
  RedisModule_ReplyWithSimpleString(ctx, "Observations");
  RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(sketch->TotalWeight()));
  RedisModule_ReplyWithSimpleString(ctx, "Total compressions");
  RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(sketch->NumCompressions()));
  RedisModule_ReplyWithSimpleString(ctx, "Memory usage");
  RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(TdigestMemUsage(sketch)));
  return REDISMODULE_OK;
}

// --- TDIGEST.LOADCHUNK (private, AOF-only) ---
static int CmdLoadchunk(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc != 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  size_t dataLen;
  const char* data = RedisModule_StringPtrLen(argv[2], &dataLen);

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE));
  if (RedisModule_KeyType(key) != REDISMODULE_KEYTYPE_EMPTY) {
    return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
  }

  auto* sketch = TdigestDeserializeChunk(data, dataLen);
  if (!sketch) {
    return RedisModule_ReplyWithError(ctx, "ERR corrupted payload");
  }
  if (RedisModule_ModuleTypeSetValue(key, TdigestType, sketch) != REDISMODULE_OK) {
    sketch->~TdigestSketch();
    RMFree(sketch);
    return RedisModule_ReplyWithError(ctx, "ERR failed to set key value");
  }

  RedisModule_ReplicateVerbatim(ctx);
  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// --- Command Registration ---
int RegisterTdigestCommands(RedisModuleCtx* ctx) {
  struct CmdEntry {
    const char* name;
    RedisModuleCmdFunc handler;
    const char* flags;
  };

  CmdEntry commands[] = {
    {"TDIGEST.CREATE",       CmdCreate,       "write deny-oom"},
    {"TDIGEST.ADD",          CmdAdd,          "write deny-oom"},
    {"TDIGEST.QUANTILE",     CmdQuantile,     "readonly"},
    {"TDIGEST.CDF",          CmdCdf,          "readonly"},
    {"TDIGEST.RANK",         CmdRank,         "readonly"},
    {"TDIGEST.REVRANK",      CmdRevrank,      "readonly"},
    {"TDIGEST.BYRANK",       CmdByrank,       "readonly"},
    {"TDIGEST.BYREVRANK",    CmdByrevrank,    "readonly"},
    {"TDIGEST.TRIMMED_MEAN", CmdTrimmedMean,  "readonly"},
    {"TDIGEST.MIN",          CmdMin,          "readonly"},
    {"TDIGEST.MAX",          CmdMax,          "readonly"},
    {"TDIGEST.RESET",        CmdReset,        "write deny-oom"},
    {"TDIGEST.MERGE",        CmdMerge,        "write deny-oom"},
    {"TDIGEST.INFO",         CmdInfo,         "readonly"},
    {"TDIGEST.LOADCHUNK",    CmdLoadchunk,    "write deny-oom"},
  };

  for (auto& cmd : commands) {
    if (RedisModule_CreateCommand(ctx, cmd.name, cmd.handler, cmd.flags, 1, 1, 1) == REDISMODULE_ERR) {
      return REDISMODULE_ERR;
    }
  }

  // Redis 7 dynamic KeySpecs for TDIGEST.MERGE:
  //   TDIGEST.MERGE dest numKeys src1 [src2 ...] [COMPRESSION c] [OVERRIDE]
  if (RedisModule_GetCommand && RedisModule_SetCommandInfo) {
    RedisModuleCommand* mergeCmd = RedisModule_GetCommand(ctx, "TDIGEST.MERGE");
    if (mergeCmd) {
      static RedisModuleCommandKeySpec kMergeKeySpecs[3] = {};
      kMergeKeySpecs[0].flags = REDISMODULE_CMD_KEY_RW | REDISMODULE_CMD_KEY_UPDATE;
      kMergeKeySpecs[0].begin_search_type = REDISMODULE_KSPEC_BS_INDEX;
      kMergeKeySpecs[0].bs.index.pos = 1;
      kMergeKeySpecs[0].find_keys_type = REDISMODULE_KSPEC_FK_RANGE;
      kMergeKeySpecs[0].fk.range.lastkey = 0;
      kMergeKeySpecs[0].fk.range.keystep = 1;
      kMergeKeySpecs[0].fk.range.limit = 0;

      kMergeKeySpecs[1].flags = REDISMODULE_CMD_KEY_RO | REDISMODULE_CMD_KEY_ACCESS;
      kMergeKeySpecs[1].begin_search_type = REDISMODULE_KSPEC_BS_INDEX;
      kMergeKeySpecs[1].bs.index.pos = 2;
      kMergeKeySpecs[1].find_keys_type = REDISMODULE_KSPEC_FK_KEYNUM;
      kMergeKeySpecs[1].fk.keynum.keynumidx = 0;
      kMergeKeySpecs[1].fk.keynum.firstkey = 1;
      kMergeKeySpecs[1].fk.keynum.keystep = 1;

      RedisModuleCommandInfo info = {};
      info.version = REDISMODULE_COMMAND_INFO_VERSION;
      info.arity = -4;
      info.key_specs = kMergeKeySpecs;
      if (RedisModule_SetCommandInfo(mergeCmd, &info) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
      }
    }
  }
  return REDISMODULE_OK;
}
