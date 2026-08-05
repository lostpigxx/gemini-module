#pragma once

#ifndef REDISMODULE_API
#define REDISMODULE_API extern
#define _CMS_COMMANDS_API_DEFINED
#endif

extern "C" {
#include "redismodule.h"
}

#ifdef _CMS_COMMANDS_API_DEFINED
#undef REDISMODULE_API
#undef _CMS_COMMANDS_API_DEFINED
#endif

int RegisterCmsCommands(RedisModuleCtx* ctx);
