#!/usr/bin/env bash
set -uo pipefail

BUILD_DIR="${BUILD_DIR:-build}"
FAIL=0

green()  { printf '\033[32m%s\033[0m\n' "$*"; }
red()    { printf '\033[31m%s\033[0m\n' "$*"; }
header() { printf '\n\033[1;36m========== %s ==========\033[0m\n' "$*"; }

# ── GTest unit tests (cmake custom targets build + run) ───────
header "GTest: gemini-bloom"
cmake --build "$BUILD_DIR" --target bloom_test -j"$(nproc)" || \
  { red "FAIL: bloom gtest"; FAIL=1; }

header "GTest: gemini-json"
cmake --build "$BUILD_DIR" --target json_test -j"$(nproc)" || \
  { red "FAIL: json gtest"; FAIL=1; }

header "GTest: gemini-search"
cmake --build "$BUILD_DIR" --target search_test -j"$(nproc)" || \
  { red "FAIL: search gtest"; FAIL=1; }

# ── Tcl integration tests (need redis-server) ────────────────
header "Tcl: gemini-bloom"
tclsh modules/gemini-bloom/tests/tcl/bloom_test.tcl "./$BUILD_DIR/redis_bloom.so" || \
  { red "FAIL: bloom tcl"; FAIL=1; }

header "Tcl: gemini-json"
tclsh modules/gemini-json/tests/tcl/json_test.tcl "./$BUILD_DIR/redis_json.so" || \
  { red "FAIL: json tcl"; FAIL=1; }

header "Tcl: gemini-search"
tclsh modules/gemini-search/tests/tcl/search_test.tcl "./$BUILD_DIR/redis_search.so" || \
  { red "FAIL: search tcl"; FAIL=1; }

# ── Summary ───────────────────────────────────────────────────
echo
if [ "$FAIL" -ne 0 ]; then
  red "Some tests FAILED"
  exit 1
else
  green "All tests PASSED"
fi
