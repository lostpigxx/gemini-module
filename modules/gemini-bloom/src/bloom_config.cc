#include "bloom_config.h"

#include <climits>
#include <cstring>

BloomConfig g_bloomConfig;

int BloomConfigLoad(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  for (int i = 0; i < argc; i++) {
    size_t len;
    const char* arg = RedisModule_StringPtrLen(argv[i], &len);

    if (len == 10 && strncasecmp(arg, "ERROR_RATE", 10) == 0) {
      if (++i >= argc) {
        RedisModule_Log(ctx, "warning", "ERROR_RATE requires a value");
        return REDISMODULE_ERR;
      }
      double val;
      if (RedisModule_StringToDouble(argv[i], &val) != REDISMODULE_OK ||
          val <= 0.0 || val >= 1.0) {
        RedisModule_Log(ctx, "warning", "Invalid ERROR_RATE");
        return REDISMODULE_ERR;
      }
      g_bloomConfig.defaultErrorRate = val;
    } else if (len == 12 && strncasecmp(arg, "INITIAL_SIZE", 12) == 0) {
      if (++i >= argc) {
        RedisModule_Log(ctx, "warning", "INITIAL_SIZE requires a value");
        return REDISMODULE_ERR;
      }
      long long val;
      if (RedisModule_StringToLongLong(argv[i], &val) != REDISMODULE_OK ||
          val <= 0) {
        RedisModule_Log(ctx, "warning", "Invalid INITIAL_SIZE");
        return REDISMODULE_ERR;
      }
      g_bloomConfig.defaultCapacity = static_cast<uint64_t>(val);
    } else if (len == 9 && strncasecmp(arg, "EXPANSION", 9) == 0) {
      if (++i >= argc) {
        RedisModule_Log(ctx, "warning", "EXPANSION requires a value");
        return REDISMODULE_ERR;
      }
      long long val;
      if (RedisModule_StringToLongLong(argv[i], &val) != REDISMODULE_OK ||
          val < 1 || val > UINT_MAX) {
        RedisModule_Log(ctx, "warning", "Invalid EXPANSION (must be >= 1)");
        return REDISMODULE_ERR;
      }
      g_bloomConfig.defaultExpansion = static_cast<unsigned>(val);
    } else {
      RedisModule_Log(ctx, "warning", "Unrecognized config argument: %.*s",
                       static_cast<int>(len), arg);
      return REDISMODULE_ERR;
    }
  }
  return REDISMODULE_OK;
}
