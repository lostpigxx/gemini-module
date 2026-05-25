#pragma once

#ifndef REDISMODULE_API
#define REDISMODULE_API extern
#define _SEARCH_CMD_API_DEFINED
#endif

extern "C" {
#include "redismodule.h"
}

#ifdef _SEARCH_CMD_API_DEFINED
#undef REDISMODULE_API
#undef _SEARCH_CMD_API_DEFINED
#endif

int RegisterSearchCommands(RedisModuleCtx* ctx);
