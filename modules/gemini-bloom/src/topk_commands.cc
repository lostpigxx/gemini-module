#include "topk_commands.h"
#include "topk_rdb.h"
#include "topk_sketch.h"
#include "bloom_filter.h"
#include "rm_alloc.h"

#include <climits>
#include <cstring>
#include <strings.h>
#include <string_view>
#include <vector>

// Defaults per the public TOPK.RESERVE documentation (redis.io): width=8,
// depth=7, decay=0.9.
constexpr uint32_t kTopkDefaultWidth = 8;
constexpr uint32_t kTopkDefaultDepth = 7;
constexpr double kTopkDefaultDecay = 0.9;
// Per the public TOPK.INCRBY documentation: increment must be in [1, 100000]
// "to avoid server freeze".
constexpr long long kTopkMaxIncrement = 100000;

// --- Helpers ---

static TopkSketch* GetSketch(RedisModuleKey* key) {
  if (RedisModule_ModuleTypeGetType(key) != TopkType) return nullptr;
  return static_cast<TopkSketch*>(RedisModule_ModuleTypeGetValue(key));
}

static bool MatchArg(std::string_view arg, std::string_view target) {
  if (arg.size() != target.size()) return false;
  return strncasecmp(arg.data(), target.data(), arg.size()) == 0;
}

static TopkSketch* SetKeyValue(RedisModuleCtx* ctx, RedisModuleKey* key, std::optional<TopkSketch>&& maybeSketch) {
  if (!maybeSketch) {
    RedisModule_ReplyWithError(ctx, "ERR allocation failure");
    return nullptr;
  }
  auto* sketch = static_cast<TopkSketch*>(RMAlloc(sizeof(TopkSketch)));
  if (!sketch) {
    RedisModule_ReplyWithError(ctx, "ERR allocation failure");
    return nullptr;
  }
  new (sketch) TopkSketch(std::move(*maybeSketch));
  if (RedisModule_ModuleTypeSetValue(key, TopkType, sketch) != REDISMODULE_OK) {
    sketch->~TopkSketch();
    RMFree(sketch);
    RedisModule_ReplyWithError(ctx, "ERR failed to set key value");
    return nullptr;
  }
  return sketch;
}

static void ReplyWithEvicted(RedisModuleCtx* ctx, const std::optional<std::string>& evicted) {
  if (evicted.has_value()) {
    RedisModule_ReplyWithStringBuffer(ctx, evicted->data(), evicted->size());
  } else {
    RedisModule_ReplyWithNull(ctx);
  }
}

// --- TOPK.RESERVE ---
static int CmdReserve(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc != 3 && argc != 6) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  long long k;
  if (RedisModule_StringToLongLong(argv[2], &k) != REDISMODULE_OK || k <= 0 ||
      static_cast<uint64_t>(k) > kTopkMaxK) {
    return RedisModule_ReplyWithError(ctx, "ERR invalid topk");
  }

  long long width = kTopkDefaultWidth;
  long long depth = kTopkDefaultDepth;
  double decay = kTopkDefaultDecay;

  if (argc == 6) {
    if (RedisModule_StringToLongLong(argv[3], &width) != REDISMODULE_OK || width <= 0 ||
        static_cast<uint64_t>(width) > kTopkMaxWidth) {
      return RedisModule_ReplyWithError(ctx, "ERR invalid width");
    }
    if (RedisModule_StringToLongLong(argv[4], &depth) != REDISMODULE_OK || depth <= 0 ||
        static_cast<uint64_t>(depth) > kTopkMaxDepth) {
      return RedisModule_ReplyWithError(ctx, "ERR invalid depth");
    }
    if (RedisModule_StringToDouble(argv[5], &decay) != REDISMODULE_OK || decay <= 0.0 || decay >= 1.0) {
      return RedisModule_ReplyWithError(ctx, "ERR invalid decay");
    }
  }

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE));
  if (RedisModule_KeyType(key) != REDISMODULE_KEYTYPE_EMPTY) {
    return RedisModule_ReplyWithError(ctx, "ERR key already exists");
  }

  auto maybeSketch = TopkSketch::Create(static_cast<uint32_t>(k), static_cast<uint32_t>(width),
                                         static_cast<uint32_t>(depth), decay);
  if (!maybeSketch) {
    return RedisModule_ReplyWithError(ctx, "ERR invalid parameters");
  }
  if (!SetKeyValue(ctx, key, std::move(maybeSketch))) return REDISMODULE_OK;

  RedisModule_ReplicateVerbatim(ctx);
  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// --- TOPK.ADD ---
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

  int numItems = argc - 2;
  RedisModule_ReplyWithArray(ctx, numItems);
  for (int i = 0; i < numItems; i++) {
    size_t len;
    const char* item = RedisModule_StringPtrLen(argv[2 + i], &len);
    ReplyWithEvicted(ctx, sketch->Add(AsBytes(item, len)));
  }

  RedisModule_ReplicateVerbatim(ctx);
  return REDISMODULE_OK;
}

// --- TOPK.INCRBY ---
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

  int numItems = (argc - 2) / 2;
  std::vector<long long> increments(static_cast<size_t>(numItems));
  for (int i = 0; i < numItems; i++) {
    long long increment;
    if (RedisModule_StringToLongLong(argv[3 + i * 2], &increment) != REDISMODULE_OK ||
        increment < 1 || increment > kTopkMaxIncrement) {
      return RedisModule_ReplyWithError(ctx, "ERR invalid increment");
    }
    increments[static_cast<size_t>(i)] = increment;
  }

  RedisModule_ReplyWithArray(ctx, numItems);
  for (int i = 0; i < numItems; i++) {
    size_t len;
    const char* item = RedisModule_StringPtrLen(argv[2 + i * 2], &len);
    auto evicted = sketch->IncrBy(AsBytes(item, len), static_cast<uint32_t>(increments[static_cast<size_t>(i)]));
    ReplyWithEvicted(ctx, evicted);
  }

  RedisModule_ReplicateVerbatim(ctx);
  return REDISMODULE_OK;
}

// --- TOPK.QUERY ---
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
    RedisModule_ReplyWithLongLong(ctx, sketch->Query(AsBytes(item, len)) ? 1 : 0);
  }
  return REDISMODULE_OK;
}

// --- TOPK.COUNT ---
static int CmdCount(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
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
    RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(sketch->Count(AsBytes(item, len))));
  }
  return REDISMODULE_OK;
}

// --- TOPK.LIST ---
static int CmdList(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc != 2 && argc != 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  bool withCount = false;
  if (argc == 3) {
    size_t len;
    const char* arg = RedisModule_StringPtrLen(argv[2], &len);
    if (!MatchArg({arg, len}, "WITHCOUNT")) {
      return RedisModule_ReplyWithError(ctx, "ERR unrecognized option");
    }
    withCount = true;
  }

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ));

  if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) {
    return RedisModule_ReplyWithError(ctx, "ERR key does not exist");
  }
  auto* sketch = GetSketch(key);
  if (!sketch) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

  auto list = sketch->List();
  RedisModule_ReplyWithArray(ctx, withCount ? static_cast<long>(list.size()) * 2 : static_cast<long>(list.size()));
  for (const auto& [name, itemCount] : list) {
    RedisModule_ReplyWithStringBuffer(ctx, name.data(), name.size());
    if (withCount) RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(itemCount));
  }
  return REDISMODULE_OK;
}

// --- TOPK.INFO ---
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

  RedisModule_ReplyWithArray(ctx, 8);
  RedisModule_ReplyWithSimpleString(ctx, "k");
  RedisModule_ReplyWithLongLong(ctx, sketch->K());
  RedisModule_ReplyWithSimpleString(ctx, "width");
  RedisModule_ReplyWithLongLong(ctx, sketch->Width());
  RedisModule_ReplyWithSimpleString(ctx, "depth");
  RedisModule_ReplyWithLongLong(ctx, sketch->Depth());
  RedisModule_ReplyWithSimpleString(ctx, "decay");
  RedisModule_ReplyWithDouble(ctx, sketch->Decay());
  return REDISMODULE_OK;
}

// --- TOPK.LOADCHUNK (private, AOF-only) ---
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

  auto* sketch = TopkDeserializeChunk(data, dataLen);
  if (!sketch) {
    return RedisModule_ReplyWithError(ctx, "ERR corrupted payload");
  }
  if (RedisModule_ModuleTypeSetValue(key, TopkType, sketch) != REDISMODULE_OK) {
    sketch->~TopkSketch();
    RMFree(sketch);
    return RedisModule_ReplyWithError(ctx, "ERR failed to set key value");
  }

  RedisModule_ReplicateVerbatim(ctx);
  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// --- Command Registration ---
int RegisterTopkCommands(RedisModuleCtx* ctx) {
  struct CmdEntry {
    const char* name;
    RedisModuleCmdFunc handler;
    const char* flags;
  };

  CmdEntry commands[] = {
    {"TOPK.RESERVE",   CmdReserve,   "write deny-oom"},
    {"TOPK.ADD",       CmdAdd,       "write deny-oom"},
    {"TOPK.INCRBY",    CmdIncrby,    "write deny-oom"},
    {"TOPK.QUERY",     CmdQuery,     "readonly"},
    {"TOPK.COUNT",     CmdCount,     "readonly"},
    {"TOPK.LIST",      CmdList,      "readonly"},
    {"TOPK.INFO",      CmdInfo,      "readonly"},
    {"TOPK.LOADCHUNK", CmdLoadchunk, "write deny-oom"},
  };

  for (auto& cmd : commands) {
    if (RedisModule_CreateCommand(ctx, cmd.name, cmd.handler, cmd.flags, 1, 1, 1) == REDISMODULE_ERR) {
      return REDISMODULE_ERR;
    }
  }
  return REDISMODULE_OK;
}
