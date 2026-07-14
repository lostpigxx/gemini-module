extern "C" {
#include "redismodule.h"
}

#include "bloom_commands.h"
#include "bloom_rdb.h"
#include "bloom_config.h"

extern "C" int RedisModule_OnLoad(RedisModuleCtx* ctx,
                                   RedisModuleString** argv, int argc) {
  if (RedisModule_Init(ctx, "GeminiBloom", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR) {
    return REDISMODULE_ERR;
  }

  if (BloomConfigLoad(ctx, argv, argc) != REDISMODULE_OK) {
    return REDISMODULE_ERR;
  }

  if (RedisModule_SetModuleOptions) {
    RedisModule_SetModuleOptions(ctx, REDISMODULE_OPTIONS_HANDLE_IO_ERRORS);
  }

  // Data type name "MBbloom--" matches RedisBloom's wire format for RDB
  // interoperability. Module identity "GeminiBloom" is separate.
  RedisModuleTypeMethods tm = {};
  tm.version = 1;
  tm.rdb_load = RdbLoadBloom;
  tm.rdb_save = RdbSaveBloom;
  tm.aof_rewrite = AofRewriteBloom;
  tm.free = FreeBloom;
  tm.mem_usage = BloomMemUsage;

  BloomType = RedisModule_CreateDataType(ctx, "MBbloom--", kCurrentEncVer, &tm);
  if (!BloomType) {
    RedisModule_Log(ctx, "warning", "Failed to register bloom filter data type");
    return REDISMODULE_ERR;
  }

  if (RegisterBloomCommands(ctx) != REDISMODULE_OK) {
    RedisModule_Log(ctx, "warning", "Failed to register bloom filter commands");
    return REDISMODULE_ERR;
  }

  RedisModule_Log(ctx, "notice", "GeminiBloom module loaded");
  return REDISMODULE_OK;
}
