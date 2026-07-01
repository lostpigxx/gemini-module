# Sanitizer Findings

Status: `BLOCKED`

No confirmed ASAN/UBSAN memory-safety finding was produced in Stage 08.

Blocked components:

- `GCC ASAN/UBSAN configure`: missing `libasan_preinit.o` and `-lasan`.
- `Clang ASAN/UBSAN Redis runtime`: module has unresolved sanitizer symbols and no loadable runtime is present.
- `GTest sanitizer execution`: no GTest target or direct GTest binary exists in this build.
- `Valgrind`: not installed.

Carry-forward findings that are memory/resource adjacent but not sanitizer findings:

- `GBV6-03-001`: RDB/wire deserialization does not enforce DESIGN's per-layer 2GB data-size cap.
- `GBV6-03-002`: RDB/wire deserialization accepts expansion values above `kMaxExpansion`.
- `GBV6-07-001`: `BF.LOADCHUNK` accepts out-of-order or repeated chunks and can complete a filter with false negatives.
- `GBV6-07-002`: half-loaded `LOADCHUNK` keys persist/replay as completed filters with false negatives.

Stage 08 adds no new source-level memory finding.

