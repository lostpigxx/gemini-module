# Blocked Sanitizer Coverage

Blocker ID: `GBV6-08-BLOCK-001`

Status: `BLOCKED / SANITIZER_RUNTIME_LOAD`

Stage 08 could not complete sanitizer runtime testing.

Evidence:

- GCC path: `asan_build/default_configure_stderr.log` and `asan_build/workaround_configure_stderr.log` show the compiler cannot link ASAN because `libasan_preinit.o` and `-lasan` are missing.
- Clang path: `asan_build/clang_build_exit_code.txt` is `0`, and `asan_build/artifact_info.txt` records the module artifact, but Redis load fails with unresolved ASAN symbols.
- Runtime discovery: `asan_tcl/runtime_discovery.log` shows only sanitizer linker-script placeholders and no loadable ASAN/UBSAN runtime.
- GTest path: `asan_gtest/gtest_summary.md` records missing sanitizer GTest target/binaries.
- Valgrind path: `valgrind/valgrind_summary.md` records valgrind unavailable.

Confidence impact:

- Runtime memory-safety and UBSAN confidence must remain degraded.
- Static fallback did not identify a new concrete UAF/OOB/double-free source defect, but static review does not replace sanitizer execution.
- Stage 08 is continuable because the stage failure is environmental and evidence/classification is complete.

