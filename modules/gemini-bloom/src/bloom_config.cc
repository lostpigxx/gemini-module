#include "bloom_config.h"
#include "bloom_filter.h"

#include <climits>
#include <cstring>
#include <strings.h>

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
          val <= 0 || static_cast<uint64_t>(val) > kMaxCapacity) {
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
          val < 1 || static_cast<unsigned long long>(val) > kMaxExpansion) {
        RedisModule_Log(ctx, "warning", "Invalid EXPANSION (must be >= 1)");
        return REDISMODULE_ERR;
      }
      g_bloomConfig.defaultExpansion = static_cast<unsigned>(val);
    } else if ((len == 14 && strncasecmp(arg, "CF_BUCKET_SIZE", 14) == 0) ||
               (len == 15 && strncasecmp(arg, "CF_INITIAL_SIZE", 15) == 0) ||
               (len == 17 && strncasecmp(arg, "CF_MAX_ITERATIONS", 17) == 0) ||
               (len == 12 && strncasecmp(arg, "CF_EXPANSION", 12) == 0) ||
               (len == 17 && strncasecmp(arg, "CF_MAX_EXPANSIONS", 17) == 0)) {
      // Recognized by CfConfigLoad (cf_config.cc), which shares this same
      // argv and validates the value. Skip over the key/value pair here so
      // BloomConfigLoad doesn't treat it as unrecognized.
      if (++i >= argc) {
        RedisModule_Log(ctx, "warning", "%.*s requires a value",
                         static_cast<int>(len), arg);
        return REDISMODULE_ERR;
      }
    } else {
      RedisModule_Log(ctx, "warning", "Unrecognized config argument: %.*s",
                       static_cast<int>(len), arg);
      return REDISMODULE_ERR;
    }
  }
  return REDISMODULE_OK;
}
