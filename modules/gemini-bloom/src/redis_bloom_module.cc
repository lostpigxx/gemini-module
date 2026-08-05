extern "C" {
#include "redismodule.h"
}

#include "bloom_commands.h"
#include "bloom_rdb.h"
#include "bloom_config.h"
#include "cf_commands.h"
#include "cf_rdb.h"
#include "cf_config.h"
#include "cms_commands.h"
#include "cms_rdb.h"
#include "topk_commands.h"
#include "topk_rdb.h"
#include "tdigest_commands.h"
#include "tdigest_rdb.h"

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

  // Data type name "MBcms----" follows the same naming convention as
  // "MBbloom--"/"MBcuckoo-"; this is a private wire format (command-level
  // compatibility only, no byte-format compatibility with any external
  // implementation).
  RedisModuleTypeMethods tm3 = {};
  tm3.version = REDISMODULE_TYPE_METHOD_VERSION;
  tm3.rdb_load = RdbLoadCms;
  tm3.rdb_save = RdbSaveCms;
  tm3.aof_rewrite = AofRewriteCms;
  tm3.free = FreeCms;
  tm3.mem_usage = CmsMemUsage;
  tm3.digest = DigestCms;
  tm3.copy2 = CopyCms2;
  tm3.free_effort2 = FreeEffortCms2;
  tm3.defrag = DefragCms;

  CmsType = RedisModule_CreateDataType(ctx, "MBcms----", kCmsEncVerCurrent, &tm3);
  if (!CmsType) {
    RedisModule_Log(ctx, "warning", "Failed to register count-min sketch data type");
    return REDISMODULE_ERR;
  }

  if (RegisterCmsCommands(ctx) != REDISMODULE_OK) {
    RedisModule_Log(ctx, "warning", "Failed to register count-min sketch commands");
    return REDISMODULE_ERR;
  }

  // Data type name "MBtopk---" follows the same naming convention as
  // "MBbloom--"/"MBcuckoo-"/"MBcms----"; this is a private wire format
  // (command-level compatibility only, no byte-format compatibility with any
  // external implementation).
  RedisModuleTypeMethods tm4 = {};
  tm4.version = REDISMODULE_TYPE_METHOD_VERSION;
  tm4.rdb_load = RdbLoadTopk;
  tm4.rdb_save = RdbSaveTopk;
  tm4.aof_rewrite = AofRewriteTopk;
  tm4.free = FreeTopk;
  tm4.mem_usage = TopkMemUsage;
  tm4.digest = DigestTopk;
  tm4.copy2 = CopyTopk2;
  tm4.free_effort2 = FreeEffortTopk2;
  tm4.defrag = DefragTopk;

  TopkType = RedisModule_CreateDataType(ctx, "MBtopk---", kTopkEncVerCurrent, &tm4);
  if (!TopkType) {
    RedisModule_Log(ctx, "warning", "Failed to register top-k data type");
    return REDISMODULE_ERR;
  }

  if (RegisterTopkCommands(ctx) != REDISMODULE_OK) {
    RedisModule_Log(ctx, "warning", "Failed to register top-k commands");
    return REDISMODULE_ERR;
  }

  // Data type name "MBtdigest" follows the same naming convention as
  // "MBbloom--"/"MBcuckoo-"/"MBcms----"/"MBtopk---"; this is a private wire
  // format (command-level compatibility only, no byte-format compatibility
  // with any external implementation).
  RedisModuleTypeMethods tm5 = {};
  tm5.version = REDISMODULE_TYPE_METHOD_VERSION;
  tm5.rdb_load = RdbLoadTdigest;
  tm5.rdb_save = RdbSaveTdigest;
  tm5.aof_rewrite = AofRewriteTdigest;
  tm5.free = FreeTdigest;
  tm5.mem_usage = TdigestMemUsage;
  tm5.digest = DigestTdigest;
  tm5.copy2 = CopyTdigest2;
  tm5.free_effort2 = FreeEffortTdigest2;
  tm5.defrag = DefragTdigest;

  TdigestType = RedisModule_CreateDataType(ctx, "MBtdigest", kTdigestEncVerCurrent, &tm5);
  if (!TdigestType) {
    RedisModule_Log(ctx, "warning", "Failed to register t-digest data type");
    return REDISMODULE_ERR;
  }

  if (RegisterTdigestCommands(ctx) != REDISMODULE_OK) {
    RedisModule_Log(ctx, "warning", "Failed to register t-digest commands");
    return REDISMODULE_ERR;
  }

  RedisModule_Log(ctx, "notice", "GeminiBloom module loaded");
  return REDISMODULE_OK;
}
