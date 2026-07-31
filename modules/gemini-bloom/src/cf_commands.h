#pragma once

#ifndef REDISMODULE_API
#define REDISMODULE_API extern
#define _CF_CMD_API_DEFINED
#endif

extern "C" {
#include "redismodule.h"
}

#ifdef _CF_CMD_API_DEFINED
#undef REDISMODULE_API
#undef _CF_CMD_API_DEFINED
#endif

int RegisterCuckooCommands(RedisModuleCtx* ctx);
