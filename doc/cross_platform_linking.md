# Cross-Platform Linking: macOS ld64 vs GNU ld

## Problem

Test binaries link successfully on macOS but fail on Linux with:

```
undefined reference to `RedisModule_Alloc'
undefined reference to `RedisModule_Free'
... (hundreds of similar errors)
```

## Root Cause

`redismodule.h` declares ~350 function pointers as global variables:

```c
#ifndef REDISMODULE_API
#define REDISMODULE_API        // expands to nothing
#endif

REDISMODULE_API void * (*RedisModule_Alloc)(size_t bytes) REDISMODULE_ATTR;
```

When a header like `bloom_rdb.h` pre-defines `REDISMODULE_API` as `extern` before including `redismodule.h`, these become `extern` declarations that the linker must resolve.

The two linkers handle unresolved `extern` symbols differently:

| Linker | Behavior |
|--------|----------|
| **macOS ld64** | Silently drops unreferenced `extern` symbols — no error even if they have no definition |
| **GNU ld (Linux)** | Requires all `extern` symbols to be resolved, even if no code path references them |

## Actual Case

`sb_chain.cc` had `#include "bloom_rdb.h"` but used nothing from it. This pulled `redismodule.h` (with `extern` declarations) into the test compilation unit `sb_chain_test`. The test defines `REDIS_BLOOM_TESTING` to avoid Redis dependencies via `rm_alloc.h`, but `bloom_rdb.h` had no such guard — it unconditionally included `redismodule.h`.

**Fix**: Removed the unnecessary `#include "bloom_rdb.h"` from `sb_chain.cc`.

## Rules

1. **Never include `redismodule.h` (directly or transitively) in files compiled into test binaries.** Test targets use `REDIS_BLOOM_TESTING` to redirect allocators to `malloc`/`free`; any inclusion of `redismodule.h` defeats this by introducing unresolvable `extern` symbols on Linux.

2. **Keep includes minimal.** Only include what the file actually uses. A stale include that happens to pull in `redismodule.h` will silently pass on macOS and break on Linux.

3. **Headers that wrap `redismodule.h`** (`bloom_rdb.h`, `bloom_commands.h`, `bloom_config.h`) must not be included from pure-algorithm files (`bloom_filter.cc`, `sb_chain.cc`, `murmur2.cc`) that are shared with test targets.

4. **Verify on Linux (or with GNU ld) before merging.** macOS builds are not sufficient to catch linkage issues.
