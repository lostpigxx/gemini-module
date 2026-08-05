#pragma once

#ifndef REDISMODULE_API
#define REDISMODULE_API extern
#define _TOPK_COMMANDS_API_DEFINED
#endif

extern "C" {
#include "redismodule.h"
}

#ifdef _TOPK_COMMANDS_API_DEFINED
#undef REDISMODULE_API
#undef _TOPK_COMMANDS_API_DEFINED
#endif

int RegisterTopkCommands(RedisModuleCtx* ctx);
