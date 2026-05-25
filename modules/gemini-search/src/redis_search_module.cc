extern "C" {
#include "redismodule.h"
}

#include "search_commands.h"

extern "C" int RedisModule_OnLoad(RedisModuleCtx* ctx,
                                   RedisModuleString** /*argv*/, int /*argc*/) {
  if (RedisModule_Init(ctx, "GeminiSearch", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR) {
    return REDISMODULE_ERR;
  }

  if (RegisterSearchCommands(ctx) != REDISMODULE_OK) {
    RedisModule_Log(ctx, "warning", "Failed to register search commands");
    return REDISMODULE_ERR;
  }

  RedisModule_Log(ctx, "notice", "GeminiSearch module loaded");
  return REDISMODULE_OK;
}
