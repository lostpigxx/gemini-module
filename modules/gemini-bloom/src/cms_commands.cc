#include "cms_commands.h"
#include "cms_rdb.h"
#include "cms_sketch.h"
#include "bloom_filter.h"
#include "rm_alloc.h"

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

static CmsSketch* GetSketch(RedisModuleKey* key) {
  if (RedisModule_ModuleTypeGetType(key) != CmsType) return nullptr;
  return static_cast<CmsSketch*>(RedisModule_ModuleTypeGetValue(key));
}

static bool MatchArg(std::string_view arg, std::string_view target) {
  if (arg.size() != target.size()) return false;
  return strncasecmp(arg.data(), target.data(), arg.size()) == 0;
}

static CmsSketch* SetKeyValue(RedisModuleCtx* ctx, RedisModuleKey* key, std::optional<CmsSketch>&& maybeSketch) {
  if (!maybeSketch) {
    RedisModule_ReplyWithError(ctx, "ERR allocation failure");
    return nullptr;
  }
  auto* sketch = static_cast<CmsSketch*>(RMAlloc(sizeof(CmsSketch)));
  if (!sketch) {
    RedisModule_ReplyWithError(ctx, "ERR allocation failure");
    return nullptr;
  }
  new (sketch) CmsSketch(std::move(*maybeSketch));
  if (RedisModule_ModuleTypeSetValue(key, CmsType, sketch) != REDISMODULE_OK) {
    sketch->~CmsSketch();
    RMFree(sketch);
    RedisModule_ReplyWithError(ctx, "ERR failed to set key value");
    return nullptr;
  }
  return sketch;
}

// --- CMS.INITBYDIM ---
static int CmdInitbydim(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc != 4) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  long long width, depth;
  if (RedisModule_StringToLongLong(argv[2], &width) != REDISMODULE_OK || width <= 0 ||
      static_cast<uint64_t>(width) > kCmsMaxWidth) {
    return RedisModule_ReplyWithError(ctx, "ERR invalid width");
  }
  if (RedisModule_StringToLongLong(argv[3], &depth) != REDISMODULE_OK || depth <= 0 ||
      static_cast<uint64_t>(depth) > kCmsMaxDepth) {
    return RedisModule_ReplyWithError(ctx, "ERR invalid depth");
  }

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE));
  if (RedisModule_KeyType(key) != REDISMODULE_KEYTYPE_EMPTY) {
    return RedisModule_ReplyWithError(ctx, "ERR key already exists");
  }

  auto maybeSketch = CmsSketch::Create(static_cast<uint32_t>(width), static_cast<uint32_t>(depth));
  if (!SetKeyValue(ctx, key, std::move(maybeSketch))) return REDISMODULE_OK;

  RedisModule_ReplicateVerbatim(ctx);
  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// --- CMS.INITBYPROB ---
static int CmdInitbyprob(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc != 4) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  double error, probability;
  if (RedisModule_StringToDouble(argv[2], &error) != REDISMODULE_OK) {
    return RedisModule_ReplyWithError(ctx, "ERR invalid error rate");
  }
  if (RedisModule_StringToDouble(argv[3], &probability) != REDISMODULE_OK) {
    return RedisModule_ReplyWithError(ctx, "ERR invalid probability");
  }

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE));
  if (RedisModule_KeyType(key) != REDISMODULE_KEYTYPE_EMPTY) {
    return RedisModule_ReplyWithError(ctx, "ERR key already exists");
  }

  auto maybeSketch = CmsSketch::CreateByProb(error, probability);
  if (!maybeSketch) {
    return RedisModule_ReplyWithError(ctx, "ERR invalid error/probability values");
  }
  if (!SetKeyValue(ctx, key, std::move(maybeSketch))) return REDISMODULE_OK;

  RedisModule_ReplicateVerbatim(ctx);
  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// --- CMS.INCRBY ---
static int CmdIncrby(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc < 4 || (argc - 2) % 2 != 0) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE));

  if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) {
    return RedisModule_ReplyWithError(ctx, "ERR key does not exist");
  }
  auto* sketch = GetSketch(key);
  if (!sketch) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

  size_t numItems = static_cast<size_t>((argc - 2) / 2);
  SmallBuf<std::span<const std::byte>> items;
  SmallBuf<int64_t> deltas;
  SmallBuf<uint64_t> results;
  if (!items.Init(numItems) || !deltas.Init(numItems) || !results.Init(numItems)) {
    return RedisModule_ReplyWithError(ctx, "ERR allocation failure");
  }
  for (size_t i = 0; i < numItems; i++) {
    long long delta;
    if (RedisModule_StringToLongLong(argv[3 + i * 2], &delta) != REDISMODULE_OK) {
      return RedisModule_ReplyWithError(ctx, "ERR invalid increment");
    }
    deltas[i] = delta;
    size_t len;
    const char* item = RedisModule_StringPtrLen(argv[2 + i * 2], &len);
    items[i] = AsBytes(item, len);
  }

  if (!sketch->IncrByBatch({items.data(), numItems}, {deltas.data(), numItems}, {results.data(), numItems})) {
    return RedisModule_ReplyWithError(ctx, "ERR overflow error");
  }

  RedisModule_ReplyWithArray(ctx, static_cast<long>(numItems));
  for (size_t i = 0; i < numItems; i++) {
    RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(results[i]));
  }

  RedisModule_ReplicateVerbatim(ctx);
  return REDISMODULE_OK;
}

// --- CMS.QUERY ---
static int CmdQuery(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc < 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ));

  if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) {
    return RedisModule_ReplyWithError(ctx, "ERR key does not exist");
  }
  auto* sketch = GetSketch(key);
  if (!sketch) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

  int count = argc - 2;
  RedisModule_ReplyWithArray(ctx, count);
  for (int i = 0; i < count; i++) {
    size_t len;
    const char* item = RedisModule_StringPtrLen(argv[2 + i], &len);
    RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(sketch->Query(AsBytes(item, len))));
  }
  return REDISMODULE_OK;
}

// --- CMS.MERGE ---
static int CmdMerge(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc < 4) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  long long numKeys;
  if (RedisModule_StringToLongLong(argv[2], &numKeys) != REDISMODULE_OK || numKeys <= 0) {
    return RedisModule_ReplyWithError(ctx, "ERR invalid numKeys");
  }

  int sourcesStart = 3;
  if (numKeys > argc - sourcesStart) return RedisModule_WrongArity(ctx);
  int weightsStart = sourcesStart + static_cast<int>(numKeys);

  SmallBuf<double> weights;
  if (!weights.Init(static_cast<size_t>(numKeys))) {
    return RedisModule_ReplyWithError(ctx, "ERR allocation failure");
  }
  for (long long i = 0; i < numKeys; i++) weights[static_cast<size_t>(i)] = 1.0;

  if (weightsStart < argc) {
    size_t len;
    const char* arg = RedisModule_StringPtrLen(argv[weightsStart], &len);
    if (!MatchArg({arg, len}, "WEIGHTS")) {
      return RedisModule_ReplyWithError(ctx, "ERR unrecognized option");
    }
    if (argc - (weightsStart + 1) != numKeys) {
      return RedisModule_WrongArity(ctx);
    }
    for (long long i = 0; i < numKeys; i++) {
      double w;
      if (RedisModule_StringToDouble(argv[weightsStart + 1 + i], &w) != REDISMODULE_OK ||
          !std::isfinite(w) || w < 0.0) {
        return RedisModule_ReplyWithError(ctx, "ERR invalid weight");
      }
      weights[static_cast<size_t>(i)] = w;
    }
  }

  auto* destKey = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE));
  if (RedisModule_KeyType(destKey) == REDISMODULE_KEYTYPE_EMPTY) {
    return RedisModule_ReplyWithError(ctx, "ERR key does not exist");
  }
  auto* dest = GetSketch(destKey);
  if (!dest) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

  SmallBuf<const CmsSketch*> sources;
  if (!sources.Init(static_cast<size_t>(numKeys))) {
    return RedisModule_ReplyWithError(ctx, "ERR allocation failure");
  }
  for (long long i = 0; i < numKeys; i++) {
    auto* srcKey = static_cast<RedisModuleKey*>(
      RedisModule_OpenKey(ctx, argv[sourcesStart + i], REDISMODULE_READ));
    if (RedisModule_KeyType(srcKey) == REDISMODULE_KEYTYPE_EMPTY) {
      return RedisModule_ReplyWithError(ctx, "ERR key does not exist");
    }
    auto* src = GetSketch(srcKey);
    if (!src) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
    sources[static_cast<size_t>(i)] = src;
  }

  if (!dest->Merge({sources.data(), static_cast<size_t>(numKeys)},
                   {weights.data(), static_cast<size_t>(numKeys)})) {
    return RedisModule_ReplyWithError(ctx, "ERR width/depth mismatch");
  }

  RedisModule_ReplicateVerbatim(ctx);
  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// --- CMS.INFO ---
static int CmdInfo(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc != 2) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ));

  if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) {
    return RedisModule_ReplyWithError(ctx, "ERR key does not exist");
  }
  auto* sketch = GetSketch(key);
  if (!sketch) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

  RedisModule_ReplyWithArray(ctx, 6);
  RedisModule_ReplyWithSimpleString(ctx, "width");
  RedisModule_ReplyWithLongLong(ctx, sketch->Width());
  RedisModule_ReplyWithSimpleString(ctx, "depth");
  RedisModule_ReplyWithLongLong(ctx, sketch->Depth());
  RedisModule_ReplyWithSimpleString(ctx, "count");
  RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(sketch->TotalCount()));
  return REDISMODULE_OK;
}

// --- CMS.LOADCHUNK (private, AOF-only) ---
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

  auto* sketch = CmsDeserializeChunk(data, dataLen);
  if (!sketch) {
    return RedisModule_ReplyWithError(ctx, "ERR corrupted payload");
  }
  if (RedisModule_ModuleTypeSetValue(key, CmsType, sketch) != REDISMODULE_OK) {
    sketch->~CmsSketch();
    RMFree(sketch);
    return RedisModule_ReplyWithError(ctx, "ERR failed to set key value");
  }

  RedisModule_ReplicateVerbatim(ctx);
  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// --- Command Registration ---
int RegisterCmsCommands(RedisModuleCtx* ctx) {
  struct CmdEntry {
    const char* name;
    RedisModuleCmdFunc handler;
    const char* flags;
  };

  CmdEntry commands[] = {
    {"CMS.INITBYDIM",  CmdInitbydim,  "write deny-oom"},
    {"CMS.INITBYPROB", CmdInitbyprob, "write deny-oom"},
    {"CMS.INCRBY",     CmdIncrby,     "write deny-oom"},
    {"CMS.QUERY",      CmdQuery,      "readonly"},
    {"CMS.MERGE",      CmdMerge,      "write deny-oom"},
    {"CMS.INFO",       CmdInfo,       "readonly"},
    {"CMS.LOADCHUNK",  CmdLoadchunk,  "write deny-oom"},
  };

  for (auto& cmd : commands) {
    if (RedisModule_CreateCommand(ctx, cmd.name, cmd.handler, cmd.flags, 1, 1, 1) == REDISMODULE_ERR) {
      return REDISMODULE_ERR;
    }
  }

  // Redis 7 dynamic KeySpecs for CMS.MERGE:
  //   CMS.MERGE dest numKeys src1 [src2 ...] [WEIGHTS w1 [w2 ...]]
  if (RedisModule_GetCommand && RedisModule_SetCommandInfo) {
    RedisModuleCommand* mergeCmd = RedisModule_GetCommand(ctx, "CMS.MERGE");
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
