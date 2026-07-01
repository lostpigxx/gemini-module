# Stage 08 Planner Output

## Stage Objective And DESIGN Constraints

Stage 08 audits memory safety and undefined behavior for `gemini-bloom` using ASAN/UBSAN where possible, with valgrind or static fallback if sanitizer execution is blocked.

Relevant DESIGN constraints:

- Core memory is RAII-based: `BloomLayer` owns bit arrays, supports move only, and frees through `RMFree`.
- `ScalingBloomFilter` uses `FilterLayer*` plus placement new; array growth must move live layers safely and destroy old objects.
- All allocation goes through `RMAlloc` / `RMCalloc` / `RMFree`; tests switch to standard malloc/free with `REDIS_BLOOM_TESTING`.
- RDB and LOADCHUNK payloads are untrusted input: validate before casts, guard arithmetic overflow, enforce blob length and resource limits before allocation.
- Resource limits are part of the memory-safety contract: capacity `1..2^30`, expansion `0..32768`, max layers `1024`, per-layer data size `<=2GB`, total data size `<=4GB`, `bitsPerEntry <=1000`.
- `BloomFlags::Loading` is runtime-only and must not persist. Stage 07 found this contract is violated in half-loaded persistence paths, causing false negatives; sanitizer may not detect this because it is a data-integrity bug rather than necessarily memory corruption.
- DESIGN-intended differences such as RESP3 unsupported and RedisBloom SCANDUMP/LOADCHUNK non-interoperability must not be reported as sanitizer bugs.

Known carry-forward risks:

- Stage 02: `cmake --build ... --target bloom_test` can fail on macOS due missing GTest dylib RPATH; direct GTest execution with resolved library path was the reliable baseline.
- Stage 03: static findings remain open for missing RDB/wire per-layer 2GB cap and accepting expansion above `kMaxExpansion`.
- Stage 07: P1 LOADCHUNK false-negative findings remain open and should be referenced separately from sanitizer memory findings.

## Build/Test Commands To Attempt

Use a clean Stage 08 build directory, preferably under `/private/tmp` or `/tmp` if the repo-local `build*` cache is stale.

Environment snapshot first: `pwd`, `git status --short`, `git rev-parse HEAD`, `uname -a`, `cmake --version`, compiler versions, Redis version, Redis CLI version, and `tclsh` version.

Primary ASAN/UBSAN configure/build:

```bash
cmake -B build-asan \
  -DENABLE_ASAN=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan -j$(nproc)
cmake --build build-asan -j$(nproc) --target bloom_test
```

Direct GTest execution if the aggregate target fails:

```bash
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:strict_string_checks=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
./build-asan/modules/gemini-bloom/tests/bloom_filter_test
```

Repeat for `sb_chain_test` and `bloom_rdb_test`.

TCL integration with sanitizer module:

```bash
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:strict_string_checks=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
tclsh modules/gemini-bloom/tests/tcl/bloom_test.tcl ./build-asan/redis_bloom.so
```

If Redis cannot load ASAN runtime, attempt `LD_PRELOAD` on Linux or `DYLD_INSERT_LIBRARIES` on macOS where feasible.

Valgrind fallback if available:

```bash
valgrind --version
valgrind --leak-check=full --track-origins=yes --error-exitcode=99 ./build-asan/modules/gemini-bloom/tests/bloom_filter_test
```

Static fallback if sanitizer/valgrind cannot run:

```bash
rg -n "reinterpret_cast|static_cast<|memcpy|malloc|calloc|free|RMAlloc|RMCalloc|RMFree|placement|new \\(|delete|uint32_t|uint8_t|UINT_MAX|SIZE_MAX|overflow|totalData|dataSize|LoadStringBuffer|LoadUnsigned|LOADCHUNK|SetLayer|AppendLayer" modules/gemini-bloom/src modules/gemini-bloom/tests
```

## Classification Rules

Sanitizer build failures:

- `FAIL / BUILD_PORTABILITY` if source does not compile under sanitizer due real code issue, missing include, invalid flags in project files, or sanitizer exposing compile/link errors. If it reproduces Stage 05's `UINT_MAX`/`<climits>` issue, link it to `GBV6-05-001`.
- `BLOCKED / TOOLCHAIN` if the local compiler lacks sanitizer support, CMake cannot find required sanitizer runtime, or platform restrictions prevent sanitizer linking.
- `BLOCKED / STALE_BUILD_CACHE` if repo-local build cache points to another source tree; retry clean temp build before final classification.
- `PASS` only if configure and build logs show `redis_bloom.so` and GTest binaries were built with sanitizer flags.

GTest failures:

- `FAIL / MEMORY_SAFETY` for ASAN reports: heap-use-after-free, stack-use-after-return, OOB read/write, double-free, alloc/dealloc mismatch, leak if reachable from test path and not expected.
- `FAIL / UB` for UBSAN reports: signed overflow, invalid shift, invalid enum, misaligned access, invalid downcast, null reference, float-cast overflow.
- `FAIL / REGRESSION` for ordinary GTest assertion failures without sanitizer report, unless already classified as known test-infra.
- `TEST_INFRA / KNOWN` for aggregate target failing only due Stage 02 GTest dylib RPATH, provided direct binaries run and evidence proves the reason.
- `PASS` only when direct sanitizer GTests exit zero and logs contain no sanitizer/UBSAN findings.

TCL Redis module loading failures:

- `BLOCKED / SANITIZER_RUNTIME_LOAD` if Redis cannot load the ASAN-built module because sanitizer runtime is missing, not preloaded, blocked by loader policy, or has incompatible dynamic-library paths.
- Before declaring blocked, attempt `LD_PRELOAD` on Linux or runtime path discovery if feasible.
- `FAIL / MEMORY_SAFETY` if Redis loads the module and sanitizer reports a real module memory error.
- `FAIL / RUNTIME_REGRESSION` if Redis loads the module but non-design-intended TCL behavior fails without sanitizer report.
- `DESIGN_INTENDED` for known expected gaps, including RESP3 unsupported and RedisBloom byte-offset SCANDUMP incompatibility.
- Do not mark ASAN TCL as `PASS` if the module never loaded.

Valgrind availability:

- `BLOCKED / VALGRIND_UNAVAILABLE` if `valgrind` is not installed.
- `PASS / ALTERNATIVE_MEMORY_CHECK` if valgrind runs selected binaries with exit zero and no definitely-lost leaks or invalid memory accesses.
- `FAIL / MEMORY_SAFETY` if valgrind reports invalid read/write, use-after-free, double-free, or definite leaks attributable to module code.

Static fallback:

- Use only when sanitizer and valgrind are blocked or partial.
- Classify sanitizer runtime coverage as `BLOCKED` or `NOT_VERIFIED`; do not upgrade static review to sanitizer `PASS`.
- Static fallback may produce `FAIL` findings with concrete file/function evidence.
- Revisit Stage 03/07 hotspots: `ValidateLayerFields`, narrowing casts, `AppendLayer`, `SetLayer`, `BloomLayer` move/destructor, `LOADCHUNK` chunk ordering, and Loading flag persistence.

## Required Evidence Layout

Required agent files:

- `.codex/gemini-bloom-audit/v6/agents/stage08/rehydrate_log.md`
- `.codex/gemini-bloom-audit/v6/agents/stage08/planner_output.md`
- `.codex/gemini-bloom-audit/v6/agents/stage08/main_execution.md`
- `.codex/gemini-bloom-audit/v6/agents/stage08/stage_result.md`
- `.codex/gemini-bloom-audit/v6/agents/stage08/reviewer_output.md`

Required evidence root:

- `commands.txt`
- `stdout.log`
- `stderr.log`
- `exit_codes.txt`
- `env_snapshot.txt`
- `evidence_index.md`

Sanitizer build evidence:

- `asan_build/configure_stdout.log`
- `asan_build/configure_stderr.log`
- `asan_build/build_stdout.log`
- `asan_build/build_stderr.log`
- `asan_build/build_exit_code.txt`
- `asan_build/artifact_info.txt`
- `asan_build/compile_commands_presence.txt`

ASAN/UBSAN GTest evidence:

- `asan_gtest/bloom_test_target_stdout.log`
- `asan_gtest/bloom_test_target_stderr.log`
- `asan_gtest/bloom_filter_test_stdout.log`
- `asan_gtest/bloom_filter_test_stderr.log`
- `asan_gtest/sb_chain_test_stdout.log`
- `asan_gtest/sb_chain_test_stderr.log`
- `asan_gtest/bloom_rdb_test_stdout.log`
- `asan_gtest/bloom_rdb_test_stderr.log`
- `asan_gtest/gtest_summary.md`

ASAN/UBSAN TCL evidence:

- `asan_tcl/bloom_test_tcl_stdout.log`
- `asan_tcl/bloom_test_tcl_stderr.log`
- `asan_tcl/tcl_exit_code.txt`
- `asan_tcl/redis_server_logs/`
- `asan_tcl/module_load_failure.log`
- `asan_tcl/tcl_summary.md`

Required findings/fallback:

- `ubsan_findings.md`
- `sanitizer_findings.md`
- `blocked_sanitizer.md` if any sanitizer path is blocked
- `valgrind/valgrind_version.txt`
- `valgrind/valgrind_summary.md`
- `static_fallback/static_scan.log`
- `static_fallback/memory_ub_hotspot_review.md`

`evidence_index.md` must map every conclusion to exact logs.

## False Positive / False Negative Risks

- Leak reports may include Redis server shutdown or sanitizer runtime allocations; attribute only when stack traces point to `modules/gemini-bloom`.
- TCL expected gaps can look like failures; preserve `DESIGN_INTENDED`.
- Loader/RPATH failures can mimic sanitizer failures; separate module-load issues from memory findings.
- UBSAN may flag system libraries; do not file module findings without module stack frames.
- GTest uses `REDIS_BLOOM_TESTING` allocation behavior, not RedisModule allocator behavior.
- TCL ASAN may be blocked by Redis dynamic loading.
- Sanitizers may not catch logical data-integrity failures such as Stage 07.
- Existing tests may not exercise huge allocations or near-limit metadata.

## PASS / BLOCKED Criteria

PASS criteria:

- Required evidence files and directories exist.
- ASAN/UBSAN configure/build/test commands have stdout/stderr/exit codes.
- Direct sanitizer GTests pass cleanly or every failure is classified.
- TCL sanitizer attempt is recorded and classified; module-load failure is not written as PASS.
- Any sanitizer/UBSAN/valgrind finding has stack trace or source evidence.
- Static fallback is included if sanitizer or valgrind coverage is blocked.
- `stage_result.md` states confidence impact and carries forward Stage 03/07 findings.

BLOCKED criteria:

- Sanitizer build/runtime cannot proceed due toolchain/runtime/platform limitations after fallback attempts.
- Redis cannot load ASAN module and preload/runtime-path attempts fail or are unavailable.
- Valgrind is unavailable or unsupported while sanitizer runtime coverage is incomplete.
- In BLOCKED cases, write `blocked_sanitizer.md`, classify exact blocked components, and mark confidence degraded. Stage 08 may continue after BLOCKED because the control batch permits it.
