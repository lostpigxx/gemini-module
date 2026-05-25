#pragma once

#ifndef GEMINI_SEARCH_TESTING
extern "C" {
#include "redismodule.h"
}
#endif

inline int FtDebugCommand(RedisModuleCtx* ctx, RedisModuleString** /*argv*/,
                          int /*argc*/) {
  RedisModule_ReplyWithSimpleString(ctx, "GeminiSearch OK");
  return REDISMODULE_OK;
}

inline int RegisterSearchCommands(RedisModuleCtx* ctx) {
  if (RedisModule_CreateCommand(ctx, "FT._DEBUG", FtDebugCommand, "readonly",
                                0, 0, 0) == REDISMODULE_ERR) {
    return REDISMODULE_ERR;
  }
  return REDISMODULE_OK;
}
