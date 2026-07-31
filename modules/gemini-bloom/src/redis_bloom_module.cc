extern "C" {
#include "redismodule.h"
}

#include "bloom_commands.h"
#include "bloom_rdb.h"
#include "bloom_config.h"
#include "cf_commands.h"
#include "cf_rdb.h"
#include "cf_config.h"

extern "C" int RedisModule_OnLoad(RedisModuleCtx* ctx,
                                   RedisModuleString** argv, int argc) {
  if (RedisModule_Init(ctx, "GeminiBloom", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR) {
    return REDISMODULE_ERR;
  }

  if (BloomConfigLoad(ctx, argv, argc) != REDISMODULE_OK) {
    return REDISMODULE_ERR;
  }

  if (CfConfigLoad(ctx, argv, argc) != REDISMODULE_OK) {
    return REDISMODULE_ERR;
  }

  if (RedisModule_SetModuleOptions) {
    RedisModule_SetModuleOptions(ctx, REDISMODULE_OPTIONS_HANDLE_IO_ERRORS);
  }

  // Data type name "MBbloom--" matches RedisBloom's wire format for RDB
  // interoperability. Module identity "GeminiBloom" is separate.
  RedisModuleTypeMethods tm = {};
  tm.version = REDISMODULE_TYPE_METHOD_VERSION;
  tm.rdb_load = RdbLoadBloom;
  tm.rdb_save = RdbSaveBloom;
  tm.aof_rewrite = AofRewriteBloom;
  tm.free = FreeBloom;
  tm.mem_usage = BloomMemUsage;
  tm.digest = DigestBloom;
  tm.copy2 = CopyBloom2;
  tm.free_effort2 = FreeEffortBloom2;
  tm.defrag = DefragBloom;

  BloomType = RedisModule_CreateDataType(ctx, "MBbloom--", kCurrentEncVer, &tm);
  if (!BloomType) {
    RedisModule_Log(ctx, "warning", "Failed to register bloom filter data type");
    return REDISMODULE_ERR;
  }

  if (RegisterBloomCommands(ctx) != REDISMODULE_OK) {
    RedisModule_Log(ctx, "warning", "Failed to register bloom filter commands");
    return REDISMODULE_ERR;
  }

  // Data type name "MBcuckoo-" follows the same naming convention as
  // "MBbloom--"; this is a private wire format (command-level compatibility
  // only, no byte-format compatibility with any external implementation).
  RedisModuleTypeMethods tm2 = {};
  tm2.version = REDISMODULE_TYPE_METHOD_VERSION;
  tm2.rdb_load = RdbLoadCuckoo;
  tm2.rdb_save = RdbSaveCuckoo;
  tm2.aof_rewrite = AofRewriteCuckoo;
  tm2.free = FreeCuckoo;
  tm2.mem_usage = CuckooMemUsage;
  tm2.digest = DigestCuckoo;
  tm2.copy2 = CopyCuckoo2;
  tm2.free_effort2 = FreeEffortCuckoo2;
  tm2.defrag = DefragCuckoo;

  CuckooType = RedisModule_CreateDataType(ctx, "MBcuckoo-", kCfEncVerCurrent, &tm2);
  if (!CuckooType) {
    RedisModule_Log(ctx, "warning", "Failed to register cuckoo filter data type");
    return REDISMODULE_ERR;
  }

  if (RegisterCuckooCommands(ctx) != REDISMODULE_OK) {
    RedisModule_Log(ctx, "warning", "Failed to register cuckoo filter commands");
    return REDISMODULE_ERR;
  }

  RedisModule_Log(ctx, "notice", "GeminiBloom module loaded");
  return REDISMODULE_OK;
}
