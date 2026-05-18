#pragma once

#ifndef REDISMODULE_API
#define REDISMODULE_API extern
#define _JSON_CMD_API_DEFINED
#endif

extern "C" {
#include "redismodule.h"
}

#ifdef _JSON_CMD_API_DEFINED
#undef REDISMODULE_API
#undef _JSON_CMD_API_DEFINED
#endif

int RegisterJsonCommands(RedisModuleCtx* ctx);
