# Stage 02 Evidence Index

| Evidence | Supports |
|---|---|
| `commands.txt` | Complete command list and exact-vs-fallback execution notes. |
| `env_snapshot.txt` | Stage 02 environment, build dirs, job count, and scope. |
| `build/configure_stderr.log` | Exact `cmake -B build` stale cache failure. |
| `build/configure_fallback_stdout.log` / `configure_fallback_exit_code.txt` | Clean fallback configure PASS. |
| `build/build_fallback_stdout.log` / `build_fallback_exit_code.txt` | Clean fallback full build PASS. |
| `build/artifact_info.txt` | `redis_bloom.so` and GTest binary existence/type. |
| `gtest/bloom_test_target_stderr.log` | CMake `bloom_test` target GTest dylib RPATH failure. |
| `gtest/gtest_summary.md` | GTest target failure classification and direct rerun results. |
| `gtest/bloom_filter_test_stdout.log` | 28 BloomLayer/hash tests PASS. |
| `gtest/sb_chain_test_stdout.log` | 21 ScalingBloomFilter tests PASS. |
| `gtest/bloom_rdb_test_stdout.log` | 65 RDB/wire tests PASS. |
| `tcl/bloom_test_tcl_stdout.log` | 144 TCL pass, 6 expected-gap failures. |
| `tcl/tcl_summary.md` | TCL classification and cleanup evidence. |
| `design_test_claim_check.md` | DESIGN.md test claims vs observed files, CMake target, GTest counts, TCL expected gaps. |
| `stdout.log` | Summary of stdout facts. |
| `stderr.log` | Summary of stderr facts. |
| `exit_codes.txt` | Command exit code and classification table. |

## Stage 02 conclusions supported

- Exact `cmake -B build` command is blocked by stale local `build` cache, not by source compile failure.
- Clean fallback configure and full build PASS on this host.
- Documented `bloom_test` target builds but fails to execute on macOS due missing GTest dylib RPATH.
- Direct GTest execution with `DYLD_LIBRARY_PATH=/opt/anaconda3/lib` PASS: 114 / 114.
- TCL runtime checks mostly PASS: 144 PASS, 6 DESIGN_INTENDED expected gaps.
- TCL harness returns nonzero for expected gaps, so the documented TCL command is not a clean PASS command.
