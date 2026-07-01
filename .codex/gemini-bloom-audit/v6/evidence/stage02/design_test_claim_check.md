# Stage 02 DESIGN Test Claim Check

## Test files

| DESIGN.md claim | Repository status | Evidence |
|---|---|---|
| `modules/gemini-bloom/tests/bloom_filter_test.cc` exists | Present | Stage 01 repo tree; Stage 02 GTest build |
| `modules/gemini-bloom/tests/sb_chain_test.cc` exists | Present | Stage 01 repo tree; Stage 02 GTest build |
| `modules/gemini-bloom/tests/bloom_rdb_test.cc` exists | Present | Stage 01 repo tree; Stage 02 GTest build |
| `modules/gemini-bloom/tests/tcl/bloom_test.tcl` exists | Present | Stage 01 repo tree; Stage 02 TCL run |

## CMake target

`modules/gemini-bloom/CMakeLists.txt` contains:

```text
29:find_package(GTest QUIET)
80:  add_custom_target(bloom_test
81:    COMMAND bloom_filter_test --gtest_color=yes
82:    COMMAND sb_chain_test --gtest_color=yes
83:    COMMAND bloom_rdb_test --gtest_color=yes
```

The target exists and is designed to run all three GTest binaries. On this macOS host it fails at runtime due missing GTest dylib RPATH; direct rerun with `DYLD_LIBRARY_PATH=/opt/anaconda3/lib` proves the tests themselves pass.

## GTest counts

| DESIGN.md claimed suite | Claimed count | Observed count | Result |
|---|---:|---:|---|
| `bloom_filter_test` | 28 | 28 | PASS |
| `sb_chain_test` | 21 | 21 | PASS |
| `bloom_rdb_test` | 65 | 65 | PASS |

Observed counts match DESIGN.md and all 114 direct GTests passed.

## TCL count and expected gaps

DESIGN.md claims TCL integration has 150 tests and includes 6 expected-fail expected-gap tests for RESP3 and SCANDUMP byte-offset cursor.

Observed:

```text
Results: 144 passed, 6 failed
```

The 6 failures are exactly the expected gaps:

- 5 RESP3 expected gaps.
- 1 SCANDUMP byte-offset cursor expected compatibility gap.

Classification:

- Runtime behavior aligns with DESIGN_INTENDED boundaries.
- The TCL harness still exits nonzero, so the documented TCL command is not a clean PASS command on this host.

## DESIGN claim mismatches / risks

| ID | Classification | Description |
|---|---|---|
| GBV6-02-001 | TEST_INFRA | `bloom_test` CMake target fails to execute on macOS because GTest dylib RPATH is missing. |
| GBV6-02-002 | TEST_HARNESS | TCL expected gaps are counted as failures and cause exit code 6 even though DESIGN.md says expected gaps are not CI blockers. |

Existing Stage 00 findings remain open:

- `GBV6-00-001`: RedisBloom v2.4.20 fixture path missing.
- `GBV6-00-002`: SCANDUMP/LOADCHUNK source comment conflicts with DESIGN.md private protocol boundary.
