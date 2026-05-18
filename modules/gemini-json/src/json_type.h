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

constexpr int kJsonEncVer = 1;

void* RdbLoadJson(RedisModuleIO* rdb, int encver);
void RdbSaveJson(RedisModuleIO* rdb, void* value);
void AofRewriteJson(RedisModuleIO* aof, RedisModuleString* key, void* value);
void FreeJson(void* value);
size_t JsonMemUsage(const void* value);
