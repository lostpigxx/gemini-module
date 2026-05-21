extern "C" {
#include "redismodule.h"
}

#include "bloom_commands.h"
#include "bloom_rdb.h"
#include "bloom_config.h"

#include <cstring>

// Data type identifiers (exactly 9 chars each, per Redis Module API).
//   Native:  independent operation.
//   Compat:  matches RedisBloom's type name for cross-module RDB migration.
static constexpr const char* kTypeNameNative = "GmBloom--";
static constexpr const char* kTypeNameCompat = "MBbloom--";

extern "C" int RedisModule_OnLoad(RedisModuleCtx* ctx,
                                   RedisModuleString** argv, int argc) {
  if (RedisModule_Init(ctx, "GeminiBloom", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR) {
    return REDISMODULE_ERR;
  }

  // Scan for COMPAT flag before passing to config loader.
  // BloomConfigLoad will ignore COMPAT since it's not a config key.
  BloomCompatMode = false;
  for (int i = 0; i < argc; i++) {
    size_t len;
    const char* arg = RedisModule_StringPtrLen(argv[i], &len);
    if (len == 6 && strncasecmp(arg, "COMPAT", 6) == 0) {
      BloomCompatMode = true;
    }
  }

  if (BloomConfigLoad(ctx, argv, argc) != REDISMODULE_OK) {
    return REDISMODULE_ERR;
  }

  const char* type_name = BloomCompatMode ? kTypeNameCompat : kTypeNameNative;

  RedisModuleTypeMethods tm = {};
  tm.version = REDISMODULE_TYPE_METHOD_VERSION;
  tm.rdb_load = RdbLoadBloom;
  tm.rdb_save = RdbSaveBloom;
  tm.aof_rewrite = AofRewriteBloom;
  tm.free = FreeBloom;
  tm.mem_usage = BloomMemUsage;

  BloomType = RedisModule_CreateDataType(ctx, type_name, kCurrentEncVer, &tm);
  if (!BloomType) {
    RedisModule_Log(ctx, "warning", "Failed to register bloom filter data type '%s'", type_name);
    return REDISMODULE_ERR;
  }

  if (RegisterBloomCommands(ctx) != REDISMODULE_OK) {
    RedisModule_Log(ctx, "warning", "Failed to register bloom filter commands");
    return REDISMODULE_ERR;
  }

  if (BloomCompatMode) {
    RedisModule_Log(ctx, "notice",
      "GeminiBloom loaded in COMPAT mode (type=%s) — "
      "RDB files are cross-compatible with RedisBloom", type_name);
  } else {
    RedisModule_Log(ctx, "notice",
      "GeminiBloom loaded in native mode (type=%s)", type_name);
  }
  return REDISMODULE_OK;
}
