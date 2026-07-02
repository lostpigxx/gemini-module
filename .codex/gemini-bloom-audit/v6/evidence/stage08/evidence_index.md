# Stage 08 Evidence Index

| Conclusion | Evidence |
|---|---|
| GCC ASAN/UBSAN configure is blocked by missing sanitizer runtime | `asan_build/default_configure_stderr.log`, `asan_build/default_configure_exit_code.txt`, `asan_build/workaround_configure_stderr.log`, `asan_build/workaround_configure_exit_code.txt` |
| Stage 05 `<climits>` workaround does not solve sanitizer runtime linking | `asan_build/workaround_configure_stderr.log` |
| Clang ASAN/UBSAN build can produce `redis_bloom.so` | `asan_build/clang_configure_exit_code.txt`, `asan_build/clang_build_exit_code.txt`, `asan_build/artifact_info.txt`, `asan_build/compile_commands_presence.txt` |
| Sanitized module cannot be loaded by Redis | `asan_build/clang_sanitizer_symbols.txt`, `asan_tcl/module_load_no_preload_redis.log`, `asan_tcl/module_load_gcc11_preload_redis.log`, `asan_tcl/module_load_failure.log` |
| LD_PRELOAD fallback cannot load sanitizer runtime | `asan_tcl/module_load_gcc11_preload_stderr.log`, `asan_tcl/runtime_discovery.log` |
| TCL ASAN/UBSAN integration was not run and must not be marked PASS | `asan_tcl/tcl_summary.md`, `asan_tcl/tcl_exit_code.txt`, `asan_tcl/bloom_test_tcl_stdout.log`, `asan_tcl/bloom_test_tcl_stderr.log` |
| GTest sanitizer execution is blocked by absent target/binaries | `asan_gtest/bloom_test_target_stderr.log`, `asan_gtest/bloom_test_target_exit_code.txt`, `asan_gtest/gtest_binaries.txt`, `asan_gtest/gtest_summary.md` |
| Valgrind is unavailable | `valgrind/valgrind_version.txt`, `valgrind/valgrind_exit_code.txt`, `valgrind/valgrind_summary.md` |
| Static fallback found no new concrete memory/UB finding | `static_fallback/static_scan.log`, `static_fallback/memory_ub_hotspot_review.md` |
| Stage 08 confidence is degraded and blocked | `blocked_sanitizer.md`, `sanitizer_findings.md`, `ubsan_findings.md`, `exit_codes.txt` |
