# Stage 02 Findings

## GBV6-02-001

| Field | Value |
|---|---|
| Severity | P2 |
| Status | OPEN |
| Classification | TEST_INFRA / BUILD_CONFIG |
| Title | CMake `bloom_test` target fails to execute on macOS because GTest dylib RPATH is missing |
| Affected area | `modules/gemini-bloom/CMakeLists.txt`, GTest execution target |
| Evidence | `.codex/gemini-bloom-audit/v6/evidence/stage02/gtest/bloom_test_target_stderr.log`, `.codex/gemini-bloom-audit/v6/evidence/stage02/gtest/gtest_summary.md` |

The clean fallback build successfully compiles `redis_bloom.so` and all three GTest binaries, but `cmake --build /private/tmp/gemini-bloom-stage02-build.G7fAub -j10 --target bloom_test` aborts when it tries to execute `bloom_filter_test`:

```text
dyld: Library not loaded: @rpath/libgtest_main.1.11.0.dylib
Reason: no LC_RPATH's found
```

The binaries link GTest from `/opt/anaconda3/lib`, and direct execution with `DYLD_LIBRARY_PATH=/opt/anaconda3/lib` passes all tests: 28 + 21 + 65 = 114 / 114.

Impact: the documented GTest target is not self-contained on this macOS environment. CI or developers using this environment can see a failing Stage 02 command even though the tested implementation behavior passes once the dynamic library path is supplied.

Suggested follow-up: set a suitable RPATH for GTest-linked test binaries, link static GTest, or make the test target run with the resolved GTest library path.

## GBV6-02-002

| Field | Value |
|---|---|
| Severity | P2 |
| Status | OPEN |
| Classification | TEST_HARNESS / DESIGN_INTENDED_GAP_HANDLING |
| Title | TCL expected gaps are counted as failures and make the documented integration test command exit nonzero |
| Affected area | `modules/gemini-bloom/tests/tcl/bloom_test.tcl` |
| Evidence | `.codex/gemini-bloom-audit/v6/evidence/stage02/tcl/bloom_test_tcl_stdout.log`, `.codex/gemini-bloom-audit/v6/evidence/stage02/tcl/tcl_summary.md` |

The TCL integration test run reported:

```text
Results: 144 passed, 6 failed
```

All 6 failures are explicitly named expected gaps:

- `EXPECTED RESP3 GAP` for five RESP3 reply-shape checks.
- `EXPECTED COMPAT GAP` for RedisBloom byte-offset SCANDUMP cursor behavior.

These behaviors match DESIGN.md's explicit boundaries: RESP3 unsupported and RedisBloom SCANDUMP/LOADCHUNK protocol non-interoperable. However the script exits with code `6`, so the documented test command is not a clean PASS command and would block CI unless the harness treats expected gaps separately.

Impact: this is not an implementation correctness bug. It is a test harness/design-claim mismatch that weakens the "expected gaps are not CI blockers" claim and complicates Stage 02 baseline interpretation.

Suggested follow-up: mark expected gaps as expected failures/skips in the harness summary and exit status, or split negative compatibility probes from the default CI test command.
