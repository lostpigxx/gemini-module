extern "C" {
#include "redismodule.h"
}

#include "json_commands.h"
#include "json_type.h"

extern "C" int RedisModule_OnLoad(RedisModuleCtx* ctx,
                                   RedisModuleString** /*argv*/, int /*argc*/) {
  if (RedisModule_Init(ctx, "GeminiJSON", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR) {
    return REDISMODULE_ERR;
  }

  RedisModuleTypeMethods tm = {};
  tm.version = REDISMODULE_TYPE_METHOD_VERSION;
  tm.rdb_load = RdbLoadJson;
  tm.rdb_save = RdbSaveJson;
  tm.aof_rewrite = AofRewriteJson;
  tm.free = FreeJson;
  tm.mem_usage = JsonMemUsage;

  JsonModuleType = RedisModule_CreateDataType(ctx, "GmJsonDo-", kJsonEncVer, &tm);
  if (!JsonModuleType) {
    RedisModule_Log(ctx, "warning", "Failed to register JSON data type");
    return REDISMODULE_ERR;
  }

  if (RegisterJsonCommands(ctx) != REDISMODULE_OK) {
    RedisModule_Log(ctx, "warning", "Failed to register JSON commands");
    return REDISMODULE_ERR;
  }

  RedisModule_Log(ctx, "notice", "Gemini JSON module loaded");
  return REDISMODULE_OK;
}
