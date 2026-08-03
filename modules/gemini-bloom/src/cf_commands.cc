#include "cf_commands.h"
#include "cf_rdb.h"
#include "cf_config.h"
#include "cf_chain.h"
#include "cuckoo_filter.h"
#include "bloom_filter.h"
#include "rm_alloc.h"

#include <climits>
#include <cstring>
#include <strings.h>

// --- Helpers ---

static CuckooChain* GetFilter(RedisModuleKey* key) {
  if (RedisModule_ModuleTypeGetType(key) != CuckooType) return nullptr;
  return static_cast<CuckooChain*>(RedisModule_ModuleTypeGetValue(key));
}

static CuckooChain* AllocFilter(uint64_t capacity, uint32_t bucketSize, uint32_t maxIterations,
                                 uint32_t expansion, uint32_t maxExpansions) {
  auto* mem = static_cast<CuckooChain*>(RMAlloc(sizeof(CuckooChain)));
  if (!mem) return nullptr;
  new (mem) CuckooChain(capacity, bucketSize, maxIterations, expansion, maxExpansions);
  if (!mem->IsValid()) {
    mem->~CuckooChain();
    RMFree(mem);
    return nullptr;
  }
  return mem;
}

static CuckooChain* AllocDefaultFilter() {
  return AllocFilter(g_cfConfig.defaultCapacity, g_cfConfig.defaultBucketSize,
                      g_cfConfig.defaultMaxIterations, g_cfConfig.defaultExpansion,
                      g_cfConfig.defaultMaxExpansions);
}

static CuckooChain* OpenOrCreate(RedisModuleCtx* ctx, RedisModuleString* keyName,
                                  RedisModuleKey** outKey, bool* created) {
  *created = false;
  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, keyName, REDISMODULE_READ | REDISMODULE_WRITE));
  *outKey = key;

  int type = RedisModule_KeyType(key);
  if (type == REDISMODULE_KEYTYPE_EMPTY) {
    auto* filter = AllocDefaultFilter();
    if (!filter) {
      RedisModule_ReplyWithError(ctx, "ERR allocation failure");
      return nullptr;
    }
    if (RedisModule_ModuleTypeSetValue(key, CuckooType, filter) != REDISMODULE_OK) {
      filter->~CuckooChain();
      RMFree(filter);
      RedisModule_ReplyWithError(ctx, "ERR failed to set key value");
      return nullptr;
    }
    *created = true;
    return filter;
  }

  if (type != REDISMODULE_KEYTYPE_MODULE) {
    RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
    return nullptr;
  }

  auto* filter = GetFilter(key);
  if (!filter) {
    RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
    return nullptr;
  }
  if (filter->IsLoading()) {
    RedisModule_ReplyWithError(ctx, "ERR filter is being loaded");
    return nullptr;
  }
  return filter;
}

static bool MatchArg(gemini_bloom::string_view arg, gemini_bloom::string_view target) {
  if (arg.size() != target.size()) return false;
  return strncasecmp(arg.data(), target.data(), arg.size()) == 0;
}

static bool RejectIfLoading(RedisModuleCtx* ctx, const CuckooChain* filter) {
  if (filter->IsLoading()) {
    RedisModule_ReplyWithError(ctx, "ERR filter is being loaded");
    return true;
  }
  return false;
}

// --- CF.RESERVE ---
static int CmdReserve(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc < 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  long long capacity;
  if (RedisModule_StringToLongLong(argv[2], &capacity) != REDISMODULE_OK || capacity <= 0 ||
      static_cast<uint64_t>(capacity) > kCfMaxBuckets) {
    return RedisModule_ReplyWithError(ctx, "ERR expected a positive capacity value");
  }

  uint32_t bucketSize = g_cfConfig.defaultBucketSize;
  uint32_t maxIterations = g_cfConfig.defaultMaxIterations;
  uint32_t expansion = g_cfConfig.defaultExpansion;
  bool bucketSizeSet = false, maxIterationsSet = false, expansionSet = false;

  for (int i = 3; i < argc; i++) {
    size_t len;
    const char* arg = RedisModule_StringPtrLen(argv[i], &len);
    auto sv = gemini_bloom::string_view{arg, len};

    if (MatchArg(sv, "BUCKETSIZE")) {
      if (bucketSizeSet) return RedisModule_ReplyWithError(ctx, "ERR duplicate BUCKETSIZE option");
      bucketSizeSet = true;
      if (++i >= argc) return RedisModule_ReplyWithError(ctx, "ERR BUCKETSIZE expects a numeric argument");
      long long val;
      if (RedisModule_StringToLongLong(argv[i], &val) != REDISMODULE_OK || val <= 0 ||
          static_cast<uint64_t>(val) > kCfMaxBucketSize) {
        return RedisModule_ReplyWithError(ctx, "ERR bad bucket size");
      }
      bucketSize = static_cast<uint32_t>(val);
    } else if (MatchArg(sv, "MAXITERATIONS")) {
      if (maxIterationsSet) return RedisModule_ReplyWithError(ctx, "ERR duplicate MAXITERATIONS option");
      maxIterationsSet = true;
      if (++i >= argc) return RedisModule_ReplyWithError(ctx, "ERR MAXITERATIONS expects a numeric argument");
      long long val;
      if (RedisModule_StringToLongLong(argv[i], &val) != REDISMODULE_OK || val <= 0 ||
          static_cast<uint64_t>(val) > kCfMaxIterations) {
        return RedisModule_ReplyWithError(ctx, "ERR bad max iterations");
      }
      maxIterations = static_cast<uint32_t>(val);
    } else if (MatchArg(sv, "EXPANSION")) {
      if (expansionSet) return RedisModule_ReplyWithError(ctx, "ERR duplicate EXPANSION option");
      expansionSet = true;
      if (++i >= argc) return RedisModule_ReplyWithError(ctx, "ERR EXPANSION expects a numeric argument");
      long long val;
      if (RedisModule_StringToLongLong(argv[i], &val) != REDISMODULE_OK || val < 0 || val > 32768) {
        return RedisModule_ReplyWithError(ctx, "ERR bad expansion");
      }
      expansion = static_cast<uint32_t>(val);
    } else {
      return RedisModule_ReplyWithError(ctx, "ERR unrecognized option");
    }
  }

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE));
  int keyType = RedisModule_KeyType(key);
  if (keyType != REDISMODULE_KEYTYPE_EMPTY) {
    if (keyType == REDISMODULE_KEYTYPE_MODULE && GetFilter(key))
      return RedisModule_ReplyWithError(ctx, "ERR item exists");
    return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
  }

  auto* filter = AllocFilter(static_cast<uint64_t>(capacity), bucketSize, maxIterations, expansion,
                              g_cfConfig.defaultMaxExpansions);
  if (!filter) {
    return RedisModule_ReplyWithError(ctx, "ERR allocation failure");
  }

  if (RedisModule_ModuleTypeSetValue(key, CuckooType, filter) != REDISMODULE_OK) {
    filter->~CuckooChain();
    RMFree(filter);
    return RedisModule_ReplyWithError(ctx, "ERR failed to set key value");
  }
  RedisModule_ReplicateVerbatim(ctx);
  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

static const char* FilterFullError() { return "ERR filter is full"; }

// --- CF.ADD / CF.ADDNX ---
static int AddCommon(RedisModuleCtx* ctx, RedisModuleString** argv, int argc, bool nx) {
  if (argc != 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  RedisModuleKey* key;
  bool created;
  auto* filter = OpenOrCreate(ctx, argv[1], &key, &created);
  if (!filter) return REDISMODULE_OK;

  size_t len;
  const char* item = RedisModule_StringPtrLen(argv[2], &len);
  auto itemSpan = AsBytes(item, len);

  if (nx && !created && filter->Contains(itemSpan)) {
    return RedisModule_ReplyWithLongLong(ctx, 0);
  }

  auto result = filter->Add(itemSpan);
  if (!result.has_value()) {
    return RedisModule_ReplyWithError(ctx, FilterFullError());
  }

  RedisModule_ReplicateVerbatim(ctx);
  return RedisModule_ReplyWithLongLong(ctx, 1);
}

static int CmdAdd(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  return AddCommon(ctx, argv, argc, false);
}

static int CmdAddnx(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  return AddCommon(ctx, argv, argc, true);
}

// --- CF.INSERT / CF.INSERTNX ---
struct CfInsertOptions {
  uint64_t capacity;
  bool noCreate = false;
  int itemsStart = -1;
};

static bool ParseInsertOptions(RedisModuleCtx* ctx, RedisModuleString** argv, int argc,
                                CfInsertOptions& opts) {
  opts.capacity = g_cfConfig.defaultCapacity;
  bool capacitySet = false;

  for (int i = 2; i < argc; i++) {
    size_t len;
    const char* arg = RedisModule_StringPtrLen(argv[i], &len);
    auto sv = gemini_bloom::string_view{arg, len};

    if (MatchArg(sv, "CAPACITY")) {
      if (capacitySet) {
        RedisModule_ReplyWithError(ctx, "ERR duplicate CAPACITY option");
        return false;
      }
      if (++i >= argc) { RedisModule_WrongArity(ctx); return false; }
      long long val;
      if (RedisModule_StringToLongLong(argv[i], &val) != REDISMODULE_OK || val <= 0 ||
          static_cast<uint64_t>(val) > kCfMaxBuckets) {
        RedisModule_ReplyWithError(ctx, "ERR expected a positive capacity value");
        return false;
      }
      opts.capacity = static_cast<uint64_t>(val);
      capacitySet = true;
    } else if (MatchArg(sv, "NOCREATE")) {
      opts.noCreate = true;
    } else if (MatchArg(sv, "ITEMS")) {
      opts.itemsStart = i + 1;
      break;
    } else {
      RedisModule_ReplyWithError(ctx, "ERR unrecognized option");
      return false;
    }
  }

  if (opts.noCreate && capacitySet) {
    RedisModule_ReplyWithError(ctx, "ERR NOCREATE cannot be used with CAPACITY");
    return false;
  }
  if (opts.itemsStart < 0) {
    RedisModule_ReplyWithError(ctx, "ERR ITEMS keyword not found");
    return false;
  }
  return true;
}

static int InsertCommon(RedisModuleCtx* ctx, RedisModuleString** argv, int argc, bool nx) {
  if (argc < 4) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  CfInsertOptions opts;
  if (!ParseInsertOptions(ctx, argv, argc, opts)) return REDISMODULE_OK;
  if (opts.itemsStart >= argc) {
    return RedisModule_WrongArity(ctx);
  }

  int count = argc - opts.itemsStart;

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE));

  CuckooChain* filter = nullptr;
  int keyType = RedisModule_KeyType(key);

  if (keyType == REDISMODULE_KEYTYPE_EMPTY) {
    if (opts.noCreate) {
      return RedisModule_ReplyWithError(ctx, "ERR key does not exist");
    }
    filter = AllocFilter(opts.capacity, g_cfConfig.defaultBucketSize, g_cfConfig.defaultMaxIterations,
                          g_cfConfig.defaultExpansion, g_cfConfig.defaultMaxExpansions);
    if (!filter) {
      return RedisModule_ReplyWithError(ctx, "ERR allocation failure");
    }
    if (RedisModule_ModuleTypeSetValue(key, CuckooType, filter) != REDISMODULE_OK) {
      filter->~CuckooChain();
      RMFree(filter);
      return RedisModule_ReplyWithError(ctx, "ERR failed to set key value");
    }
  } else if (keyType == REDISMODULE_KEYTYPE_MODULE) {
    filter = GetFilter(key);
    if (!filter) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
    if (RejectIfLoading(ctx, filter)) return REDISMODULE_OK;
  } else {
    return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
  }

  RedisModule_ReplyWithArray(ctx, count);

  bool changed = false;
  for (int i = 0; i < count; i++) {
    size_t len;
    const char* item = RedisModule_StringPtrLen(argv[opts.itemsStart + i], &len);
    auto itemSpan = AsBytes(item, len);

    if (nx && filter->Contains(itemSpan)) {
      RedisModule_ReplyWithLongLong(ctx, 0);
      continue;
    }

    auto result = filter->Add(itemSpan);
    if (!result.has_value()) {
      // Unlike BF.INSERT, CF.INSERT/CF.INSERTNX do not stop early on a full
      // filter — each remaining item is reported individually as -1 and the
      // loop continues, per the public CF.INSERT documentation.
      RedisModule_ReplyWithLongLong(ctx, -1);
      continue;
    }
    RedisModule_ReplyWithLongLong(ctx, 1);
    changed = true;
  }

  if (changed) RedisModule_ReplicateVerbatim(ctx);
  return REDISMODULE_OK;
}

static int CmdInsert(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  return InsertCommon(ctx, argv, argc, false);
}

static int CmdInsertnx(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  return InsertCommon(ctx, argv, argc, true);
}

// --- CF.EXISTS ---
static int CmdExists(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc != 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ));

  if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) {
    return RedisModule_ReplyWithLongLong(ctx, 0);
  }

  auto* filter = GetFilter(key);
  if (!filter) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
  if (RejectIfLoading(ctx, filter)) return REDISMODULE_OK;

  size_t len;
  const char* item = RedisModule_StringPtrLen(argv[2], &len);
  return RedisModule_ReplyWithLongLong(ctx, filter->Contains(AsBytes(item, len)) ? 1 : 0);
}

// --- CF.MEXISTS ---
static int CmdMexists(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc < 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ));

  int count = argc - 2;

  if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) {
    RedisModule_ReplyWithArray(ctx, count);
    for (int i = 0; i < count; i++) RedisModule_ReplyWithLongLong(ctx, 0);
    return REDISMODULE_OK;
  }

  auto* filter = GetFilter(key);
  if (!filter) {
    RedisModule_ReplyWithArray(ctx, count);
    for (int i = 0; i < count; i++) RedisModule_ReplyWithLongLong(ctx, 0);
    return REDISMODULE_OK;
  }
  if (RejectIfLoading(ctx, filter)) return REDISMODULE_OK;

  RedisModule_ReplyWithArray(ctx, count);
  for (int i = 0; i < count; i++) {
    size_t len;
    const char* item = RedisModule_StringPtrLen(argv[i + 2], &len);
    RedisModule_ReplyWithLongLong(ctx, filter->Contains(AsBytes(item, len)) ? 1 : 0);
  }
  return REDISMODULE_OK;
}

// --- CF.DEL ---
static int CmdDel(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc != 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE));

  if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) {
    return RedisModule_ReplyWithError(ctx, "ERR key does not exist");
  }

  auto* filter = GetFilter(key);
  if (!filter) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
  if (RejectIfLoading(ctx, filter)) return REDISMODULE_OK;

  size_t len;
  const char* item = RedisModule_StringPtrLen(argv[2], &len);
  bool deleted = filter->Delete(AsBytes(item, len));

  if (deleted) RedisModule_ReplicateVerbatim(ctx);
  return RedisModule_ReplyWithLongLong(ctx, deleted ? 1 : 0);
}

// --- CF.COUNT ---
static int CmdCount(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc != 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ));

  if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) {
    return RedisModule_ReplyWithLongLong(ctx, 0);
  }

  auto* filter = GetFilter(key);
  if (!filter) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
  if (RejectIfLoading(ctx, filter)) return REDISMODULE_OK;

  size_t len;
  const char* item = RedisModule_StringPtrLen(argv[2], &len);
  return RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(filter->Count(AsBytes(item, len))));
}

// --- CF.INFO ---
// Field order/names are mandated by the public CF.INFO documentation
// (https://redis.io/commands/cf.info) for client compatibility.
static int CmdInfo(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc != 2) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ));

  if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) {
    return RedisModule_ReplyWithError(ctx, "ERR key does not exist");
  }

  auto* filter = GetFilter(key);
  if (!filter) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
  if (RejectIfLoading(ctx, filter)) return REDISMODULE_OK;

  RedisModule_ReplyWithArray(ctx, 16);
  RedisModule_ReplyWithSimpleString(ctx, "Size");
  RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(filter->BytesUsed()));
  RedisModule_ReplyWithSimpleString(ctx, "Number of buckets");
  RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(filter->TotalBuckets()));
  RedisModule_ReplyWithSimpleString(ctx, "Number of filters");
  RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(filter->NumLayers()));
  RedisModule_ReplyWithSimpleString(ctx, "Number of items inserted");
  RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(filter->TotalItems()));
  RedisModule_ReplyWithSimpleString(ctx, "Number of items deleted");
  RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(filter->TotalDeleted()));
  RedisModule_ReplyWithSimpleString(ctx, "Bucket size");
  RedisModule_ReplyWithLongLong(ctx, filter->BucketSize());
  RedisModule_ReplyWithSimpleString(ctx, "Expansion rate");
  RedisModule_ReplyWithLongLong(ctx, filter->Expansion());
  RedisModule_ReplyWithSimpleString(ctx, "Max iterations");
  RedisModule_ReplyWithLongLong(ctx, filter->MaxIterations());
  return REDISMODULE_OK;
}

// --- CF.SCANDUMP ---
// Cursor protocol mirrors BF.SCANDUMP's shape but is independently
// formatted (no byte-format compatibility requirement — see cf_rdb.h).
//   SCANDUMP key 0   -> [1, header]
//   SCANDUMP key 1   -> [2, layer0_buckets]
//   ...
//   SCANDUMP key N+1 -> [0, ""]   (end)
static int CmdScandump(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc != 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ));

  if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) {
    return RedisModule_ReplyWithError(ctx, "ERR key does not exist");
  }

  auto* filter = GetFilter(key);
  if (!filter) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
  if (RejectIfLoading(ctx, filter)) return REDISMODULE_OK;

  long long cursor;
  if (RedisModule_StringToLongLong(argv[2], &cursor) != REDISMODULE_OK || cursor < 0) {
    return RedisModule_ReplyWithError(ctx, "ERR cursor must be non-negative");
  }

  if (cursor == 0) {
    size_t hdrBytes = CfComputeHeaderSize(*filter);
    auto* hdrBuf = static_cast<char*>(RMAlloc(hdrBytes));
    if (!hdrBuf) {
      return RedisModule_ReplyWithError(ctx, "ERR allocation failure during SCANDUMP");
    }
    RedisModule_ReplyWithArray(ctx, 2);
    CfSerializeHeader(*filter, hdrBuf);
    RedisModule_ReplyWithLongLong(ctx, 1);
    RedisModule_ReplyWithStringBuffer(ctx, hdrBuf, hdrBytes);
    RMFree(hdrBuf);
  } else if (cursor >= 1 && static_cast<size_t>(cursor - 1) < filter->NumLayers()) {
    RedisModule_ReplyWithArray(ctx, 2);
    size_t idx = static_cast<size_t>(cursor - 1);
    const auto& layer = filter->Layers()[idx];
    RedisModule_ReplyWithLongLong(ctx, cursor + 1);
    RedisModule_ReplyWithStringBuffer(ctx,
      reinterpret_cast<const char*>(layer.cuckoo.GetBucketArray()), layer.cuckoo.GetDataSize());
  } else {
    RedisModule_ReplyWithArray(ctx, 2);
    RedisModule_ReplyWithLongLong(ctx, 0);
    RedisModule_ReplyWithStringBuffer(ctx, "", 0);
  }
  return REDISMODULE_OK;
}

// --- CF.LOADCHUNK ---
static int CmdLoadchunk(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc != 4) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  long long cursor;
  if (RedisModule_StringToLongLong(argv[2], &cursor) != REDISMODULE_OK || cursor < 1) {
    return RedisModule_ReplyWithError(ctx, "ERR cursor must be a positive integer");
  }

  size_t dataLen;
  const char* data = RedisModule_StringPtrLen(argv[3], &dataLen);

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE));

  if (cursor == 1) {
    int keyType = RedisModule_KeyType(key);
    if (keyType != REDISMODULE_KEYTYPE_EMPTY) {
      if (keyType == REDISMODULE_KEYTYPE_MODULE && GetFilter(key)) {
        return RedisModule_ReplyWithError(ctx, "ERR received bad data");
      }
      return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
    }
    auto* filter = CfDeserializeHeader(data, dataLen);
    if (!filter) {
      return RedisModule_ReplyWithError(ctx, "ERR corrupted header payload");
    }
    filter->SetLoading();
    if (RedisModule_ModuleTypeSetValue(key, CuckooType, filter) != REDISMODULE_OK) {
      filter->~CuckooChain();
      RMFree(filter);
      return RedisModule_ReplyWithError(ctx, "ERR failed to set key value");
    }
  } else {
    if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) {
      return RedisModule_ReplyWithError(ctx, "ERR key does not exist");
    }
    auto* filter = GetFilter(key);
    if (!filter) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
    if (!filter->IsLoading()) {
      return RedisModule_ReplyWithError(ctx, "ERR received bad data");
    }

    size_t idx = static_cast<size_t>(cursor - 2);
    if (idx >= filter->NumLayers()) {
      RedisModule_DeleteKey(key);
      return RedisModule_ReplyWithError(ctx, "ERR cursor exceeds layer count");
    }
    if (idx != filter->ChunksLoaded()) {
      RedisModule_DeleteKey(key);
      return RedisModule_ReplyWithError(ctx, "ERR out-of-order chunk");
    }
    auto& layer = filter->Layers()[idx];
    if (dataLen != static_cast<size_t>(layer.cuckoo.GetDataSize())) {
      RedisModule_DeleteKey(key);
      return RedisModule_ReplyWithError(ctx, "ERR data length mismatch for layer");
    }
    std::memcpy(layer.cuckoo.GetBucketArray(), data, dataLen);
    layer.cuckoo.RecountItems();
    filter->IncrementChunksLoaded();
    if (filter->ChunksLoaded() == filter->NumLayers()) {
      filter->ClearLoading();
    }
  }

  RedisModule_ReplicateVerbatim(ctx);
  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// --- Command Registration ---
int RegisterCuckooCommands(RedisModuleCtx* ctx) {
  struct CmdEntry {
    const char* name;
    RedisModuleCmdFunc handler;
    const char* flags;
  };

  CmdEntry commands[] = {
    {"CF.RESERVE",   CmdReserve,   "write deny-oom"},
    {"CF.ADD",       CmdAdd,       "write deny-oom"},
    {"CF.ADDNX",     CmdAddnx,     "write deny-oom"},
    {"CF.INSERT",    CmdInsert,    "write deny-oom"},
    {"CF.INSERTNX",  CmdInsertnx,  "write deny-oom"},
    {"CF.EXISTS",    CmdExists,    "readonly"},
    {"CF.MEXISTS",   CmdMexists,   "readonly"},
    {"CF.DEL",       CmdDel,       "write"},
    {"CF.COUNT",     CmdCount,     "readonly"},
    {"CF.INFO",      CmdInfo,      "readonly"},
    {"CF.SCANDUMP",  CmdScandump,  "readonly fast"},
    {"CF.LOADCHUNK", CmdLoadchunk, "write deny-oom"},
  };

  for (auto& cmd : commands) {
    if (RedisModule_CreateCommand(ctx, cmd.name, cmd.handler, cmd.flags, 1, 1, 1) == REDISMODULE_ERR) {
      return REDISMODULE_ERR;
    }
  }
  return REDISMODULE_OK;
}
