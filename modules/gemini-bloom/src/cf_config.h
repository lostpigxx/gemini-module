#pragma once

#include <cstdint>

#ifndef REDISMODULE_API
#define REDISMODULE_API extern
#define _CF_CFG_API_DEFINED
#endif

extern "C" {
#include "redismodule.h"
}

#ifdef _CF_CFG_API_DEFINED
#undef REDISMODULE_API
#undef _CF_CFG_API_DEFINED
#endif

struct CfConfig {
  uint32_t defaultBucketSize = 2;
  uint64_t defaultCapacity = 1024;
  uint32_t defaultMaxIterations = 20;
  uint32_t defaultExpansion = 1;
  uint32_t defaultMaxExpansions = 32;
};

extern CfConfig g_cfConfig;

int CfConfigLoad(RedisModuleCtx* ctx, RedisModuleString** argv, int argc);
