# UBSAN Findings

Status: `NOT_VERIFIED`

No UBSAN runtime finding was produced. UBSAN coverage is not available because:

- GCC sanitizer configure failed before build due missing sanitizer runtime.
- Clang could build the module but Redis could not load it without a loadable ASAN/UBSAN runtime.
- No sanitizer GTest binary was generated.

This file records absence of runtime evidence, not absence of UB in the implementation.

