#!/usr/bin/env bash
set -uo pipefail

BUILD_DIR="${BUILD_DIR:-build}"
FAIL=0

green()  { printf '\033[32m%s\033[0m\n' "$*"; }
red()    { printf '\033[31m%s\033[0m\n' "$*"; }
header() { printf '\n\033[1;36m========== %s ==========\033[0m\n' "$*"; }

# ── GTest unit tests ─────────────────────────────────────────
header "GTest: gemini-bloom"
cmake --build "$BUILD_DIR" --target bloom_test -j"$(nproc)" || \
  { red "FAIL: bloom gtest"; FAIL=1; }

# ── Tcl integration tests (need redis-server) ───────────────
header "Tcl: gemini-bloom"
tclsh modules/gemini-bloom/tests/tcl/bloom_test.tcl "./$BUILD_DIR/redis_bloom.so" || \
  { red "FAIL: bloom tcl"; FAIL=1; }

# ── Compat test (gemini-bloom vs RedisBloom) ─────────────────
header "Compat: gemini-bloom vs RedisBloom"
python3 ci/bloom_compat_test.py "./$BUILD_DIR/redis_bloom.so" /opt/redisbloom.so || \
  { red "FAIL: bloom compat"; FAIL=1; }

# ── Soak test (gemini-bloom, 5 min default) ──────────────────
header "Soak: gemini-bloom (${SOAK_DURATION_SEC:-300}s)"
SOAK_DURATION_SEC="${SOAK_DURATION_SEC:-300}" \
  python3 ci/bloom_soak_test.py "./$BUILD_DIR/redis_bloom.so" || \
  { red "FAIL: bloom soak"; FAIL=1; }

# ── Summary ──────────────────────────────────────────────────
echo
if [ "$FAIL" -ne 0 ]; then
  red "Some tests FAILED"
  exit 1
else
  green "All tests PASSED"
fi
