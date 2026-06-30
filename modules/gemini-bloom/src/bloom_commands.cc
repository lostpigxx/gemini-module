#include "bloom_commands.h"
#include "bloom_rdb.h"
#include "bloom_config.h"
#include "sb_chain.h"
#include "rm_alloc.h"

#include <climits>
#include <cstring>
#include <strings.h>
#include <string_view>

// --- Helpers ---

static ScalingBloomFilter* GetFilter(RedisModuleKey* key) {
  if (RedisModule_ModuleTypeGetType(key) != BloomType) return nullptr;
  return static_cast<ScalingBloomFilter*>(RedisModule_ModuleTypeGetValue(key));
}

static ScalingBloomFilter* AllocFilter(uint64_t cap, double rate,
                                        unsigned expansion, bool fixed) {
  auto flg = BloomFlags::Use64Bit | BloomFlags::NoRound;
  if (fixed) flg = flg | BloomFlags::FixedSize;
  auto* mem = static_cast<ScalingBloomFilter*>(RMAlloc(sizeof(ScalingBloomFilter)));
  if (!mem) return nullptr;
  new (mem) ScalingBloomFilter(cap, rate, flg, expansion);
  if (!mem->IsValid()) {
    mem->~ScalingBloomFilter();
    RMFree(mem);
    return nullptr;
  }
  return mem;
}

static ScalingBloomFilter* AllocDefaultFilter() {
  return AllocFilter(g_bloomConfig.defaultCapacity, g_bloomConfig.defaultErrorRate,
                      g_bloomConfig.defaultExpansion, false);
}

static ScalingBloomFilter* OpenOrCreate(RedisModuleCtx* ctx, RedisModuleString* keyName,
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
    if (RedisModule_ModuleTypeSetValue(key, BloomType, filter) != REDISMODULE_OK) {
      filter->~ScalingBloomFilter();
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

static bool MatchArg(std::string_view arg, std::string_view target) {
  if (arg.size() != target.size()) return false;
  return strncasecmp(arg.data(), target.data(), arg.size()) == 0;
}

static const char* FilterFullError(const ScalingBloomFilter* filter) {
  return HasFlag(filter->Flags(), BloomFlags::FixedSize)
    ? "ERR non scaling filter is full"
    : "ERR filter expansion failed";
}

static bool RejectIfLoading(RedisModuleCtx* ctx, const ScalingBloomFilter* filter) {
  if (filter->IsLoading()) {
    RedisModule_ReplyWithError(ctx, "ERR filter is being loaded");
    return true;
  }
  return false;
}

// --- BF.RESERVE ---
static int CmdReserve(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc < 4) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  double rate;
  if (RedisModule_StringToDouble(argv[2], &rate) != REDISMODULE_OK ||
      rate <= 0.0 || rate >= 1.0) {
    return RedisModule_ReplyWithError(ctx, "ERR false positive rate must be in (0, 1)");
  }

  long long cap;
  if (RedisModule_StringToLongLong(argv[3], &cap) != REDISMODULE_OK || cap <= 0 ||
      static_cast<uint64_t>(cap) > kMaxCapacity) {
    return RedisModule_ReplyWithError(ctx, "ERR expected a positive capacity value");
  }

  unsigned expansion = g_bloomConfig.defaultExpansion;
  bool fixed = false;
  bool expansionSeen = false;
  bool expansionSet = false;
  bool nonscalingSet = false;

  for (int i = 4; i < argc; i++) {
    size_t len;
    const char* arg = RedisModule_StringPtrLen(argv[i], &len);
    auto sv = std::string_view{arg, len};

    if (MatchArg(sv, "EXPANSION")) {
      if (expansionSeen)
        return RedisModule_ReplyWithError(ctx, "ERR duplicate EXPANSION option");
      expansionSeen = true;
      if (++i >= argc)
        return RedisModule_ReplyWithError(ctx, "ERR EXPANSION expects a numeric argument");
      long long val;
      if (RedisModule_StringToLongLong(argv[i], &val) != REDISMODULE_OK ||
          val < 0 || static_cast<unsigned long long>(val) > kMaxExpansion) {
        return RedisModule_ReplyWithError(ctx, "ERR bad expansion");
      }
      if (val == 0) {
        fixed = true;
      } else {
        expansion = static_cast<unsigned>(val);
        expansionSet = true;
      }
    } else if (MatchArg(sv, "NONSCALING")) {
      if (nonscalingSet)
        return RedisModule_ReplyWithError(ctx, "ERR duplicate NONSCALING option");
      nonscalingSet = true;
      fixed = true;
    } else {
      return RedisModule_ReplyWithError(ctx, "ERR unrecognized option");
    }
  }

  if (fixed && expansionSet) {
    return RedisModule_ReplyWithError(ctx, "ERR Nonscaling filters cannot expand");
  }

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE));
  int keyType = RedisModule_KeyType(key);
  if (keyType != REDISMODULE_KEYTYPE_EMPTY) {
    if (keyType == REDISMODULE_KEYTYPE_MODULE && GetFilter(key))
      return RedisModule_ReplyWithError(ctx, "ERR item exists");
    return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
  }

  auto* filter = AllocFilter(static_cast<uint64_t>(cap), rate, expansion, fixed);
  if (!filter) {
    return RedisModule_ReplyWithError(ctx, "ERR allocation failure");
  }

  if (RedisModule_ModuleTypeSetValue(key, BloomType, filter) != REDISMODULE_OK) {
    filter->~ScalingBloomFilter();
    RMFree(filter);
    return RedisModule_ReplyWithError(ctx, "ERR failed to set key value");
  }
  RedisModule_ReplicateVerbatim(ctx);
  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// --- BF.ADD ---
static int CmdAdd(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc != 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  RedisModuleKey* key;
  bool created;
  auto* filter = OpenOrCreate(ctx, argv[1], &key, &created);
  if (!filter) return REDISMODULE_OK;

  size_t len;
  const char* item = RedisModule_StringPtrLen(argv[2], &len);
  auto result = filter->Put(AsBytes(item, len));

  if (!result.has_value()) {
    return RedisModule_ReplyWithError(ctx, FilterFullError(filter));
  }

  if (*result || created) RedisModule_ReplicateVerbatim(ctx);
  return RedisModule_ReplyWithLongLong(ctx, *result ? 1 : 0);
}

// --- BF.MADD ---
static int CmdMadd(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc < 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  RedisModuleKey* key;
  bool created;
  auto* filter = OpenOrCreate(ctx, argv[1], &key, &created);
  if (!filter) return REDISMODULE_OK;

  int count = argc - 2;
  RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);

  bool changed = created;
  long replied = 0;
  for (int i = 0; i < count; i++) {
    size_t len;
    const char* item = RedisModule_StringPtrLen(argv[i + 2], &len);
    auto result = filter->Put(AsBytes(item, len));
    if (!result.has_value()) {
      RedisModule_ReplyWithError(ctx, FilterFullError(filter));
      replied++;
      break;
    }
    RedisModule_ReplyWithLongLong(ctx, *result ? 1 : 0);
    if (*result) changed = true;
    replied++;
  }
  RedisModule_ReplySetArrayLength(ctx, replied);

  if (changed) RedisModule_ReplicateVerbatim(ctx);
  return REDISMODULE_OK;
}

// --- BF.INSERT ---
// Parsed options are collected into a struct before any Redis state is touched.
struct InsertOptions {
  double errorRate;
  uint64_t capacity;
  unsigned expansion;
  bool noCreate = false;
  bool fixedSize = false;
  int itemsStart = -1;
};

// Returns true on success; on failure, replies to the client and returns false.
static bool ParseInsertOptions(RedisModuleCtx* ctx, RedisModuleString** argv,
                                int argc, InsertOptions& opts) {
  opts.errorRate = g_bloomConfig.defaultErrorRate;
  opts.capacity = g_bloomConfig.defaultCapacity;
  opts.expansion = g_bloomConfig.defaultExpansion;
  bool expansionSeen = false;
  bool expansionPositive = false;
  bool capacitySet = false;
  bool errorSet = false;
  bool nonscalingSet = false;

  for (int i = 2; i < argc; i++) {
    size_t len;
    const char* arg = RedisModule_StringPtrLen(argv[i], &len);
    auto sv = std::string_view{arg, len};

    if (MatchArg(sv, "ERROR")) {
      if (errorSet) {
        RedisModule_ReplyWithError(ctx, "ERR duplicate ERROR option");
        return false;
      }
      if (++i >= argc) { RedisModule_WrongArity(ctx); return false; }
      double val;
      if (RedisModule_StringToDouble(argv[i], &val) != REDISMODULE_OK ||
          val <= 0.0 || val >= 1.0) {
        RedisModule_ReplyWithError(ctx, "ERR false positive rate must be in (0, 1)");
        return false;
      }
      opts.errorRate = val;
      errorSet = true;
    } else if (MatchArg(sv, "CAPACITY")) {
      if (capacitySet) {
        RedisModule_ReplyWithError(ctx, "ERR duplicate CAPACITY option");
        return false;
      }
      if (++i >= argc) { RedisModule_WrongArity(ctx); return false; }
      long long val;
      if (RedisModule_StringToLongLong(argv[i], &val) != REDISMODULE_OK || val <= 0 ||
          static_cast<uint64_t>(val) > kMaxCapacity) {
        RedisModule_ReplyWithError(ctx, "ERR expected a positive capacity value");
        return false;
      }
      opts.capacity = static_cast<uint64_t>(val);
      capacitySet = true;
    } else if (MatchArg(sv, "EXPANSION")) {
      if (expansionSeen) {
        RedisModule_ReplyWithError(ctx, "ERR duplicate EXPANSION option");
        return false;
      }
      expansionSeen = true;
      if (++i >= argc) { RedisModule_WrongArity(ctx); return false; }
      long long val;
      if (RedisModule_StringToLongLong(argv[i], &val) != REDISMODULE_OK ||
          val < 0 || static_cast<unsigned long long>(val) > kMaxExpansion) {
        RedisModule_ReplyWithError(ctx, "ERR bad expansion");
        return false;
      }
      if (val == 0) {
        opts.fixedSize = true;
      } else {
        opts.expansion = static_cast<unsigned>(val);
        expansionPositive = true;
      }
    } else if (MatchArg(sv, "NOCREATE")) {
      opts.noCreate = true;
    } else if (MatchArg(sv, "NONSCALING")) {
      if (nonscalingSet) {
        RedisModule_ReplyWithError(ctx, "ERR duplicate NONSCALING option");
        return false;
      }
      nonscalingSet = true;
      opts.fixedSize = true;
    } else if (MatchArg(sv, "ITEMS")) {
      opts.itemsStart = i + 1;
      break;
    } else {
      RedisModule_ReplyWithError(ctx, "ERR unrecognized option");
      return false;
    }
  }

  if (opts.fixedSize && expansionPositive) {
    RedisModule_ReplyWithError(ctx, "ERR NONSCALING and EXPANSION are mutually exclusive");
    return false;
  }
  if (opts.noCreate && (capacitySet || errorSet)) {
    RedisModule_ReplyWithError(ctx, "ERR NOCREATE cannot be used with CAPACITY or ERROR");
    return false;
  }
  if (opts.itemsStart < 0) {
    RedisModule_ReplyWithError(ctx, "ERR ITEMS keyword not found");
    return false;
  }
  return true;
}

static int CmdInsert(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc < 4) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  InsertOptions opts;
  if (!ParseInsertOptions(ctx, argv, argc, opts)) return REDISMODULE_OK;
  if (opts.itemsStart >= argc) {
    return RedisModule_WrongArity(ctx);
  }

  int count = argc - opts.itemsStart;

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE));

  ScalingBloomFilter* filter = nullptr;
  int keyType = RedisModule_KeyType(key);

  if (keyType == REDISMODULE_KEYTYPE_EMPTY) {
    if (opts.noCreate) {
      return RedisModule_ReplyWithError(ctx, "ERR key does not exist");
    }
    filter = AllocFilter(opts.capacity, opts.errorRate, opts.expansion, opts.fixedSize);
    if (!filter) {
      return RedisModule_ReplyWithError(ctx, "ERR allocation failure");
    }
    if (RedisModule_ModuleTypeSetValue(key, BloomType, filter) != REDISMODULE_OK) {
      filter->~ScalingBloomFilter();
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

  RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);

  bool changed = (keyType == REDISMODULE_KEYTYPE_EMPTY);
  long replied = 0;
  for (int i = 0; i < count; i++) {
    size_t len;
    const char* item = RedisModule_StringPtrLen(argv[opts.itemsStart + i], &len);
    auto result = filter->Put(AsBytes(item, len));
    if (!result.has_value()) {
      RedisModule_ReplyWithError(ctx, FilterFullError(filter));
      replied++;
      break;
    }
    RedisModule_ReplyWithLongLong(ctx, *result ? 1 : 0);
    if (*result) changed = true;
    replied++;
  }
  RedisModule_ReplySetArrayLength(ctx, replied);

  if (changed) RedisModule_ReplicateVerbatim(ctx);
  return REDISMODULE_OK;
}

// --- BF.EXISTS ---
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

// --- BF.MEXISTS ---
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
    return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
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

// --- BF.INFO ---
// Response labels are mandated by the BF.INFO protocol specification
// (https://redis.io/commands/bf.info) for client compatibility.
static int CmdInfo(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc < 2 || argc > 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ));

  if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) {
    return RedisModule_ReplyWithError(ctx, "ERR key does not exist");
  }

  auto* filter = GetFilter(key);
  if (!filter) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
  if (RejectIfLoading(ctx, filter)) return REDISMODULE_OK;

  if (argc == 3) {
    size_t len;
    const char* field = RedisModule_StringPtrLen(argv[2], &len);
    auto sv = std::string_view{field, len};

    if (MatchArg(sv, "Capacity")) {
      return RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(filter->TotalCapacity()));
    } else if (MatchArg(sv, "Size")) {
      return RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(filter->BytesUsed()));
    } else if (MatchArg(sv, "Filters")) {
      return RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(filter->NumLayers()));
    } else if (MatchArg(sv, "Items")) {
      return RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(filter->TotalItems()));
    } else if (MatchArg(sv, "Expansion")) {
      if (HasFlag(filter->Flags(), BloomFlags::FixedSize)) return RedisModule_ReplyWithNull(ctx);
      return RedisModule_ReplyWithLongLong(ctx, filter->ExpansionFactor());
    }
    return RedisModule_ReplyWithError(ctx, "ERR unknown subcommand for BF.INFO");
  }

  RedisModule_ReplyWithArray(ctx, 10);
  RedisModule_ReplyWithSimpleString(ctx, "Capacity");
  RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(filter->TotalCapacity()));
  RedisModule_ReplyWithSimpleString(ctx, "Size");
  RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(filter->BytesUsed()));
  RedisModule_ReplyWithSimpleString(ctx, "Number of filters");
  RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(filter->NumLayers()));
  RedisModule_ReplyWithSimpleString(ctx, "Number of items inserted");
  RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(filter->TotalItems()));
  RedisModule_ReplyWithSimpleString(ctx, "Expansion rate");
  if (HasFlag(filter->Flags(), BloomFlags::FixedSize)) {
    RedisModule_ReplyWithNull(ctx);
  } else {
    RedisModule_ReplyWithLongLong(ctx, filter->ExpansionFactor());
  }
  return REDISMODULE_OK;
}

// --- BF.CARD ---
static int CmdCard(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc != 2) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ));

  if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) {
    return RedisModule_ReplyWithLongLong(ctx, 0);
  }

  auto* filter = GetFilter(key);
  if (!filter) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
  if (RejectIfLoading(ctx, filter)) return REDISMODULE_OK;

  return RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(filter->TotalItems()));
}

// --- BF.SCANDUMP ---
// Cursor protocol: SCANDUMP returns (iter, data) pairs. The returned iter is
// what the client passes to LOADCHUNK alongside data. iter=0 means end of
// iteration with empty data. cursor=0 starts the dump.
//   SCANDUMP key 0   → [1, header]
//   SCANDUMP key 1   → [2, layer0_bits]
//   SCANDUMP key 2   → [3, layer1_bits]
//   ...
//   SCANDUMP key N+1 → [0, ""]   (end)
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
    size_t hdrBytes = ComputeHeaderSize(*filter);
    auto* hdrBuf = static_cast<char*>(RMAlloc(hdrBytes));
    if (!hdrBuf) {
      return RedisModule_ReplyWithError(ctx, "ERR allocation failure during SCANDUMP");
    }
    RedisModule_ReplyWithArray(ctx, 2);
    SerializeHeader(*filter, hdrBuf);
    RedisModule_ReplyWithLongLong(ctx, 1);
    RedisModule_ReplyWithStringBuffer(ctx, hdrBuf, hdrBytes);
    RMFree(hdrBuf);
  } else if (cursor >= 1 && static_cast<size_t>(cursor - 1) < filter->NumLayers()) {
    RedisModule_ReplyWithArray(ctx, 2);
    size_t idx = static_cast<size_t>(cursor - 1);
    const auto& layer = filter->Layers()[idx];
    RedisModule_ReplyWithLongLong(ctx, cursor + 1);
    RedisModule_ReplyWithStringBuffer(ctx,
      reinterpret_cast<const char*>(layer.bloom.GetBitArray()),
      layer.bloom.GetDataSize());
  } else {
    RedisModule_ReplyWithArray(ctx, 2);
    RedisModule_ReplyWithLongLong(ctx, 0);
    RedisModule_ReplyWithStringBuffer(ctx, "", 0);
  }
  return REDISMODULE_OK;
}

// --- BF.LOADCHUNK ---
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
    auto* filter = DeserializeHeader(data, dataLen);
    if (!filter) {
      return RedisModule_ReplyWithError(ctx, "ERR corrupted header payload");
    }
    filter->SetLoading();
    if (RedisModule_ModuleTypeSetValue(key, BloomType, filter) != REDISMODULE_OK) {
      filter->~ScalingBloomFilter();
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
      return RedisModule_ReplyWithError(ctx, "ERR cursor exceeds layer count");
    }
    auto& layer = filter->Layers()[idx];
    if (dataLen != static_cast<size_t>(layer.bloom.GetDataSize())) {
      return RedisModule_ReplyWithError(ctx, "ERR data length mismatch for layer");
    }
    std::memcpy(layer.bloom.GetBitArray(), data, dataLen);
    if (idx == filter->NumLayers() - 1) {
      filter->ClearLoading();
    }
  }

  RedisModule_ReplicateVerbatim(ctx);
  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// --- Command Registration ---
int RegisterBloomCommands(RedisModuleCtx* ctx) {
  struct CmdEntry {
    const char* name;
    RedisModuleCmdFunc handler;
    const char* flags;
  };

  CmdEntry commands[] = {
    {"BF.RESERVE",   CmdReserve,   "write deny-oom"},
    {"BF.ADD",       CmdAdd,       "write deny-oom"},
    {"BF.MADD",      CmdMadd,      "write deny-oom"},
    {"BF.INSERT",    CmdInsert,    "write deny-oom"},
    {"BF.EXISTS",    CmdExists,    "readonly"},
    {"BF.MEXISTS",   CmdMexists,   "readonly"},
    {"BF.INFO",      CmdInfo,      "readonly"},
    {"BF.CARD",      CmdCard,      "readonly"},
    {"BF.SCANDUMP",  CmdScandump,  "readonly fast"},
    {"BF.LOADCHUNK", CmdLoadchunk, "write deny-oom"},
  };

  for (auto& cmd : commands) {
    if (RedisModule_CreateCommand(ctx, cmd.name, cmd.handler, cmd.flags, 1, 1, 1) == REDISMODULE_ERR) {
      return REDISMODULE_ERR;
    }
  }
  return REDISMODULE_OK;
}
