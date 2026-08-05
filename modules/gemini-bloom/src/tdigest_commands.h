#pragma once

#ifndef REDISMODULE_API
#define REDISMODULE_API extern
#define _TDIGEST_COMMANDS_API_DEFINED
#endif

extern "C" {
#include "redismodule.h"
}

#ifdef _TDIGEST_COMMANDS_API_DEFINED
#undef REDISMODULE_API
#undef _TDIGEST_COMMANDS_API_DEFINED
#endif

int RegisterTdigestCommands(RedisModuleCtx* ctx);
