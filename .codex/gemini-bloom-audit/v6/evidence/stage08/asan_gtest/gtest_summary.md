# ASAN/UBSAN GTest Summary

Status: `BLOCKED / GTEST_TARGET_ABSENT`

The Stage 08 Clang ASAN/UBSAN module build completed, but the generated CMake build graph did not contain a `bloom_test` target and no direct GTest binaries were present. The aggregate command required by the stage failed with:

```text
gmake: *** No rule to make target 'bloom_test'. Stop.
```

Evidence:

- `bloom_test_target_stdout.log`
- `bloom_test_target_stderr.log`
- `bloom_test_target_exit_code.txt`
- `gtest_binaries.txt`
- direct per-test `*_stdout.log`, `*_stderr.log`, and `*_exit_code.txt` files are explicit `NOT_RUN_GTEST_BINARY_ABSENT` records.

No ASAN/UBSAN memory-safety conclusion can be drawn from GTest execution in this environment.

