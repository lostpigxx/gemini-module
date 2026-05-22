#pragma once

#ifndef REDISMODULE_API
#define REDISMODULE_API extern
#define _JSON_TYPE_API_DEFINED
#endif

extern "C" {
#include "redismodule.h"
}

#ifdef _JSON_TYPE_API_DEFINED
#undef REDISMODULE_API
#undef _JSON_TYPE_API_DEFINED
#endif

extern RedisModuleType* JsonModuleType;

// Compat mode: when true, use "ReJSON-RL" type name + JSON text RDB
// for cross-module migration with RedisJSON.
// Controlled by MODULE LOAD ... COMPAT argument.
extern bool JsonCompatMode;

// Binary format (native): encver 2
// Text format (compat):   encver 3 — deliberately higher so that our module
//   can still load old binary-format (encver 2) RDBs produced before the user
//   switched to compat mode.
constexpr int kJsonEncVerBinary = 2;
constexpr int kJsonEncVerCompat = 3;

// Max encver we advertise to Redis (must be the highest we support)
constexpr int kJsonEncVerMax = 3;

void* RdbLoadJson(RedisModuleIO* rdb, int encver);
void RdbSaveJson(RedisModuleIO* rdb, void* value);
void AofRewriteJson(RedisModuleIO* aof, RedisModuleString* key, void* value);
void FreeJson(void* value);
size_t JsonMemUsage(const void* value);
