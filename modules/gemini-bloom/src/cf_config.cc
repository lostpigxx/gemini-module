#include "cf_config.h"
#include "cuckoo_filter.h"

#include <climits>
#include <cstring>
#include <strings.h>

CfConfig g_cfConfig;

// NOTE: CfConfigLoad and BloomConfigLoad both scan the same RedisModule_OnLoad
// argv (bloom_config.cc / cf_config.cc), since the two data types share one
// module. Each function only acts on its own recognized keys and silently
// skips (continues past) the other's — bloom_config.cc recognizes the 5
// CF_* key names below (and their arity) purely to skip over them; it does
// not validate their values, since CfConfigLoad already did that here. This
// keeps validation logic for each config owned by its own file while
// avoiding either treating the other's valid keys as "unrecognized argument"
// errors. Truly unknown arguments are rejected by BloomConfigLoad, the
// final gatekeeper in RedisModule_OnLoad's call order (see cf_config.h /
// redis_bloom_module.cc).
int CfConfigLoad(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  for (int i = 0; i < argc; i++) {
    size_t len;
    const char* arg = RedisModule_StringPtrLen(argv[i], &len);

    if (len == 14 && strncasecmp(arg, "CF_BUCKET_SIZE", 14) == 0) {
      if (++i >= argc) {
        RedisModule_Log(ctx, "warning", "CF_BUCKET_SIZE requires a value");
        return REDISMODULE_ERR;
      }
      long long val;
      if (RedisModule_StringToLongLong(argv[i], &val) != REDISMODULE_OK ||
          val <= 0 || static_cast<uint64_t>(val) > kCfMaxBucketSize) {
        RedisModule_Log(ctx, "warning", "Invalid CF_BUCKET_SIZE");
        return REDISMODULE_ERR;
      }
      g_cfConfig.defaultBucketSize = static_cast<uint32_t>(val);
    } else if (len == 15 && strncasecmp(arg, "CF_INITIAL_SIZE", 15) == 0) {
      if (++i >= argc) {
        RedisModule_Log(ctx, "warning", "CF_INITIAL_SIZE requires a value");
        return REDISMODULE_ERR;
      }
      long long val;
      if (RedisModule_StringToLongLong(argv[i], &val) != REDISMODULE_OK ||
          val <= 0 || static_cast<uint64_t>(val) > (1ULL << 30)) {
        RedisModule_Log(ctx, "warning", "Invalid CF_INITIAL_SIZE");
        return REDISMODULE_ERR;
      }
      g_cfConfig.defaultCapacity = static_cast<uint64_t>(val);
    } else if (len == 17 && strncasecmp(arg, "CF_MAX_ITERATIONS", 17) == 0) {
      if (++i >= argc) {
        RedisModule_Log(ctx, "warning", "CF_MAX_ITERATIONS requires a value");
        return REDISMODULE_ERR;
      }
      long long val;
      if (RedisModule_StringToLongLong(argv[i], &val) != REDISMODULE_OK ||
          val <= 0 || static_cast<uint64_t>(val) > kCfMaxIterations) {
        RedisModule_Log(ctx, "warning", "Invalid CF_MAX_ITERATIONS");
        return REDISMODULE_ERR;
      }
      g_cfConfig.defaultMaxIterations = static_cast<uint32_t>(val);
    } else if (len == 12 && strncasecmp(arg, "CF_EXPANSION", 12) == 0) {
      if (++i >= argc) {
        RedisModule_Log(ctx, "warning", "CF_EXPANSION requires a value");
        return REDISMODULE_ERR;
      }
      long long val;
      if (RedisModule_StringToLongLong(argv[i], &val) != REDISMODULE_OK ||
          val < 0 || val > 32768) {
        RedisModule_Log(ctx, "warning", "Invalid CF_EXPANSION");
        return REDISMODULE_ERR;
      }
      g_cfConfig.defaultExpansion = static_cast<uint32_t>(val);
    } else if (len == 17 && strncasecmp(arg, "CF_MAX_EXPANSIONS", 17) == 0) {
      if (++i >= argc) {
        RedisModule_Log(ctx, "warning", "CF_MAX_EXPANSIONS requires a value");
        return REDISMODULE_ERR;
      }
      long long val;
      if (RedisModule_StringToLongLong(argv[i], &val) != REDISMODULE_OK ||
          val <= 0 || val > 65535) {
        RedisModule_Log(ctx, "warning", "Invalid CF_MAX_EXPANSIONS");
        return REDISMODULE_ERR;
      }
      g_cfConfig.defaultMaxExpansions = static_cast<uint32_t>(val);
    } else {
      // Not a CF_* argument — leave it for BloomConfigLoad to interpret
      // (or reject as unrecognized). Both configs share the same argv.
      continue;
    }
  }
  return REDISMODULE_OK;
}
