# Stage 08 Result — SANITIZER_MEMORY

Status: `BLOCKED`

Continuable: yes

## Summary

Stage 08 could not complete runtime sanitizer verification in the available Docker/container environment. The GCC sanitizer path fails at CMake compiler probing because the sanitizer runtime is missing. The Clang path builds `redis_bloom.so`, but Redis cannot load the module because no loadable ASAN/UBSAN runtime is present to resolve sanitizer symbols.

The GTest sanitizer path is also blocked because this build graph produced no `bloom_test` target or direct GTest binaries. Valgrind is unavailable. Static fallback review found no new concrete UAF/OOB/double-free/UB finding, but static review does not satisfy sanitizer runtime coverage.

## Classifications

| Area | Status | Evidence |
|---|---|---|
| GCC ASAN/UBSAN configure | `BLOCKED / TOOLCHAIN` | `evidence/stage08/asan_build/default_configure_stderr.log` |
| GCC ASAN/UBSAN with `<climits>` workaround | `BLOCKED / TOOLCHAIN` | `evidence/stage08/asan_build/workaround_configure_stderr.log` |
| Clang ASAN/UBSAN build | `PASS_BUILD_ONLY` | `evidence/stage08/asan_build/artifact_info.txt` |
| GTest sanitizer execution | `BLOCKED / GTEST_TARGET_ABSENT` | `evidence/stage08/asan_gtest/gtest_summary.md` |
| TCL sanitizer execution | `BLOCKED / SANITIZER_RUNTIME_LOAD` | `evidence/stage08/asan_tcl/tcl_summary.md` |
| UBSAN runtime findings | `NOT_VERIFIED` | `evidence/stage08/ubsan_findings.md` |
| Valgrind fallback | `BLOCKED / VALGRIND_UNAVAILABLE` | `evidence/stage08/valgrind/valgrind_summary.md` |
| Static fallback | `PARTIAL / STATIC_FALLBACK` | `evidence/stage08/static_fallback/memory_ub_hotspot_review.md` |

## Findings

No new Stage 08 source-level memory-safety finding is opened.

## New Blocker

`GBV6-08-BLOCK-001`: ASAN/UBSAN runtime and valgrind execution are unavailable/incomplete in the current audit environment.

Impact:

- Runtime memory-safety confidence remains degraded.
- No sanitizer PASS should be claimed for GTest or TCL integration.
- Later final confidence must reflect that memory/UB behavior was not dynamically verified in Stage 08.

## Carry Forward

- `GBV6-03-001`: RDB/wire per-layer 2GB cap not enforced.
- `GBV6-03-002`: RDB/wire expansion accepts values above `kMaxExpansion`.
- `GBV6-07-001`: `BF.LOADCHUNK` can complete from out-of-order/repeated chunks with false negatives.
- `GBV6-07-002`: half-loaded `LOADCHUNK` keys can persist/replay as completed filters with false negatives.

## Evidence

See `.codex/gemini-bloom-audit/v6/evidence/stage08/evidence_index.md`.

## Agent Closure Note

Planner output is persisted at `.codex/gemini-bloom-audit/v6/agents/stage08/planner_output.md`. Planner/reviewer will be marked closed in `LOOP_STATE.md` after reviewer completion.

