extern "C" {
#include "redismodule.h"
}

#include "json_commands.h"
#include "json_type.h"

#include <cstring>

// Data type identifiers (exactly 9 chars each, per Redis Module API).
//   Native:  independent operation, binary RDB format.
//   Compat:  matches RedisJSON's type name for cross-module RDB migration.
static constexpr const char* kTypeNameNative = "GmJsonDo-";
static constexpr const char* kTypeNameCompat = "ReJSON-RL";

extern "C" int RedisModule_OnLoad(RedisModuleCtx* ctx,
                                   RedisModuleString** argv, int argc) {
  if (RedisModule_Init(ctx, "GeminiJSON", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR) {
    return REDISMODULE_ERR;
  }

  // Parse module arguments: MODULE LOAD redis_json.so [COMPAT]
  JsonCompatMode = false;
  for (int i = 0; i < argc; i++) {
    size_t len;
    const char* arg = RedisModule_StringPtrLen(argv[i], &len);
    if (len == 6 && strncasecmp(arg, "COMPAT", 6) == 0) {
      JsonCompatMode = true;
    }
  }

  const char* type_name = JsonCompatMode ? kTypeNameCompat : kTypeNameNative;
  int encver = JsonCompatMode ? kJsonEncVerCompat : kJsonEncVerMax;

  RedisModuleTypeMethods tm = {};
  tm.version = REDISMODULE_TYPE_METHOD_VERSION;
  tm.rdb_load = RdbLoadJson;
  tm.rdb_save = RdbSaveJson;
  tm.aof_rewrite = AofRewriteJson;
  tm.free = FreeJson;
  tm.mem_usage = JsonMemUsage;

  JsonModuleType = RedisModule_CreateDataType(ctx, type_name, encver, &tm);
  if (!JsonModuleType) {
    RedisModule_Log(ctx, "warning", "Failed to register JSON data type '%s'", type_name);
    return REDISMODULE_ERR;
  }

  if (RegisterJsonCommands(ctx) != REDISMODULE_OK) {
    RedisModule_Log(ctx, "warning", "Failed to register JSON commands");
    return REDISMODULE_ERR;
  }

  if (JsonCompatMode) {
    RedisModule_Log(ctx, "notice",
      "GeminiJSON loaded in COMPAT mode (type=%s, RDB=text) — "
      "RDB files are cross-compatible with RedisJSON", type_name);
  } else {
    RedisModule_Log(ctx, "notice",
      "GeminiJSON loaded in native mode (type=%s, RDB=binary)", type_name);
  }
  return REDISMODULE_OK;
}
