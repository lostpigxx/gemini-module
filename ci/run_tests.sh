#!/usr/bin/env bash
set -uo pipefail

BUILD_DIR="${BUILD_DIR:-build}"
FAIL=0

green()  { printf '\033[32m%s\033[0m\n' "$*"; }
red()    { printf '\033[31m%s\033[0m\n' "$*"; }
header() { printf '\n\033[1;36m========== %s ==========\033[0m\n' "$*"; }

REDIS_MAJOR=$(redis-server --version | sed -n 's/.*v=\([0-9]*\).*/\1/p')
REDIS_MAJOR=${REDIS_MAJOR:-0}

# ── GTest unit tests ─────────────────────────────────────────
header "GTest: gemini-bloom"
cmake --build "$BUILD_DIR" --target bloom_test -j"$(nproc)" || \
  { red "FAIL: bloom gtest"; FAIL=1; }

# ── Tcl integration tests (need redis-server) ───────────────
header "Tcl: gemini-bloom"
tclsh modules/gemini-bloom/tests/tcl/bloom_test.tcl "./$BUILD_DIR/redis_bloom.so" || \
  { red "FAIL: bloom tcl"; FAIL=1; }

# Quantify per-key/module memory across 1k..50k keys, capacities, layers,
# and RDB restart. The per-key overhead regression gate is 64 bytes.
header "Memory profile: key cardinality, capacity, layers, restart"
python3 ci/bloom_memory_profile_test.py "./$BUILD_DIR/redis_bloom.so" /opt/redisbloom.so || \
  { red "FAIL: bloom memory profile"; FAIL=1; }

# ── Compat test (gemini-bloom vs RedisBloom) ─────────────────
header "Compat: gemini-bloom vs RedisBloom (RESP auto)"
python3 ci/bloom_compat_test.py "./$BUILD_DIR/redis_bloom.so" /opt/redisbloom.so || \
  { red "FAIL: bloom compat"; FAIL=1; }

if [ "$REDIS_MAJOR" -ge 6 ]; then
  header "Compat: gemini-bloom vs RedisBloom (RESP2 forced)"
  RESP_PROTOCOL=2 \
    python3 ci/bloom_compat_test.py "./$BUILD_DIR/redis_bloom.so" /opt/redisbloom.so || \
    { red "FAIL: bloom compat RESP2"; FAIL=1; }
fi

# ── Soak test (gemini-bloom, 5 min default) ──────────────────
header "Soak: gemini-bloom (${SOAK_DURATION_SEC:-300}s, RESP auto)"
SOAK_DURATION_SEC="${SOAK_DURATION_SEC:-300}" \
  python3 ci/bloom_soak_test.py "./$BUILD_DIR/redis_bloom.so" || \
  { red "FAIL: bloom soak"; FAIL=1; }

if [ "$REDIS_MAJOR" -ge 6 ]; then
  header "Soak: gemini-bloom (${SOAK_DURATION_SEC:-300}s, RESP2 forced)"
  SOAK_DURATION_SEC="${SOAK_DURATION_SEC:-300}" RESP_PROTOCOL=2 \
    python3 ci/bloom_soak_test.py "./$BUILD_DIR/redis_bloom.so" || \
    { red "FAIL: bloom soak RESP2"; FAIL=1; }
fi

# Partial BF.LOADCHUNK state across AOF rewrite, RDB preamble, and BGSAVE.
# Chunks are applied after the rewrite fork so they land in the AOF tail.
header "Partial load: BF.LOADCHUNK persistence across restart"
python3 ci/bloom_partial_load_test.py "./$BUILD_DIR/redis_bloom.so" || \
  { red "FAIL: bloom partial load"; FAIL=1; }

# ── RDB migration test (gemini-bloom ↔ RedisBloom) ───────────
header "Migrate: RDB round-trip (RESP auto)"
python3 ci/bloom_migrate_test.py "./$BUILD_DIR/redis_bloom.so" /opt/redisbloom.so || \
  { red "FAIL: bloom migrate"; FAIL=1; }

if [ "$REDIS_MAJOR" -ge 6 ]; then
  header "Migrate: RDB round-trip (RESP2 forced)"
  RESP_PROTOCOL=2 \
    python3 ci/bloom_migrate_test.py "./$BUILD_DIR/redis_bloom.so" /opt/redisbloom.so || \
    { red "FAIL: bloom migrate RESP2"; FAIL=1; }
fi

# ── Summary ──────────────────────────────────────────────────
echo
if [ "$FAIL" -ne 0 ]; then
  red "Some tests FAILED"
  exit 1
else
  green "All tests PASSED"
fi
