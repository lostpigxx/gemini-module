#pragma once

#ifndef REDISMODULE_API
#define REDISMODULE_API extern
#define _SEARCH_RDB_API_DEFINED
#endif

extern "C" {
#include "redismodule.h"
}

#ifdef _SEARCH_RDB_API_DEFINED
#undef REDISMODULE_API
#undef _SEARCH_RDB_API_DEFINED
#endif

extern RedisModuleType* SearchModuleType;

constexpr int kSearchEncVer = 4;

void* RdbLoadSearch(RedisModuleIO* rdb, int encver);
void RdbSaveSearch(RedisModuleIO* rdb, void* value);
void AofRewriteSearch(RedisModuleIO* aof, RedisModuleString* key, void* value);
void FreeSearch(void* value);
size_t SearchMemUsage(const void* value);
