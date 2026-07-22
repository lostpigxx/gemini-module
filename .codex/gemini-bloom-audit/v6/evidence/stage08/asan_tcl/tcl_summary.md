# ASAN/UBSAN TCL Summary

Status: `BLOCKED / SANITIZER_RUNTIME_LOAD`

Redis module-load probes:

- no preload: Redis failed to load `redis_bloom.so` with unresolved `__asan_option_detect_stack_use_after_return`.
- GCC 11 preload: loader rejected `/usr/lib/gcc/x86_64-redhat-linux/11/libasan.so` and `/usr/lib/gcc/x86_64-redhat-linux/11/libubsan.so` because they are linker scripts pointing at absent `/usr/lib64` runtime files.

The full TCL suite was not run with the sanitizer module. Marking TCL sanitizer as PASS would be incorrect.

Evidence:

- `module_load_no_preload_redis.log`
- `module_load_no_preload_stderr.log`
- `module_load_gcc11_preload_redis.log`
- `module_load_gcc11_preload_stderr.log`
- `runtime_discovery.log`
- `redis_server_logs/no_preload_redis.log`
- `redis_server_logs/gcc11_preload_redis.log`

