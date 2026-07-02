# Stage 02 GTest Summary

## Documented target result

Command:

```bash
cmake --build /private/tmp/gemini-bloom-stage02-build.G7fAub -j10 --target bloom_test
```

Result: `BLOCKED_TEST_INFRA`, exit code `2`.

Cause:

```text
dyld[25841]: Library not loaded: @rpath/libgtest_main.1.11.0.dylib
Referenced from: /private/tmp/gemini-bloom-stage02-build.G7fAub/modules/gemini-bloom/bloom_filter_test
Reason: no LC_RPATH's found
```

The CMake target builds all three GTest binaries, then aborts when executing the first binary because the linked GTest dylib is not discoverable through RPATH.

## Direct GTest rerun with dylib path

The GTest dylibs exist at:

```text
/opt/anaconda3/lib/libgtest.1.11.0.dylib
/opt/anaconda3/lib/libgtest_main.1.11.0.dylib
```

Direct rerun command prefix:

```bash
DYLD_LIBRARY_PATH=/opt/anaconda3/lib
```

Results:

| Binary | Tests | Result | Evidence |
|---|---:|---|---|
| `bloom_filter_test` | 28 | PASS | `bloom_filter_test_stdout.log`, `bloom_filter_test_exit_code.txt` |
| `sb_chain_test` | 21 | PASS | `sb_chain_test_stdout.log`, `sb_chain_test_exit_code.txt` |
| `bloom_rdb_test` | 65 | PASS | `bloom_rdb_test_stdout.log`, `bloom_rdb_test_exit_code.txt` |

Total direct GTest result: 114 / 114 passed.

Classification:

- Product implementation behavior covered by these GTests: `PASS` under direct dylib-path rerun.
- Documented CMake `bloom_test` target as a standalone test command on this macOS host: `FAIL` / `TEST_INFRA`.
