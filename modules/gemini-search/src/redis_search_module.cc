extern "C" {
#include "redismodule.h"
}

#include "search_commands.h"
#include "search_rdb.h"

extern "C" int RedisModule_OnLoad(RedisModuleCtx* ctx,
                                   RedisModuleString** /*argv*/, int /*argc*/) {
  if (RedisModule_Init(ctx, "GeminiSearch", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR) {
    return REDISMODULE_ERR;
  }

  if (RedisModule_SetModuleOptions) {
    RedisModule_SetModuleOptions(ctx, REDISMODULE_OPTIONS_HANDLE_IO_ERRORS);
  }

  RedisModuleTypeMethods tm = {};
  tm.version = REDISMODULE_TYPE_METHOD_VERSION;
  tm.rdb_load = RdbLoadSearch;
  tm.rdb_save = RdbSaveSearch;
  tm.aof_rewrite = AofRewriteSearch;
  tm.free = FreeSearch;
  tm.mem_usage = SearchMemUsage;
  tm.digest = DigestSearch;
  tm.copy2 = CopySearch2;
  tm.free_effort2 = FreeEffortSearch2;
  tm.defrag = DefragSearch;
  tm.aux_save2 = AuxSaveSearch;
  tm.aux_load = AuxLoadSearch;
  tm.aux_save_triggers = REDISMODULE_AUX_AFTER_RDB;

  SearchModuleType =
      RedisModule_CreateDataType(ctx, "GmSearch-", kSearchEncVer, &tm);
  if (!SearchModuleType) {
    RedisModule_Log(ctx, "warning", "Failed to register search data type");
    return REDISMODULE_ERR;
  }

  if (RegisterSearchCommands(ctx) != REDISMODULE_OK) {
    RedisModule_Log(ctx, "warning", "Failed to register search commands");
    return REDISMODULE_ERR;
  }

  RedisModule_Log(ctx, "notice", "GeminiSearch module loaded");
  return REDISMODULE_OK;
}
