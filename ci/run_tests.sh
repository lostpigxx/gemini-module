#!/usr/bin/env bash
set -uo pipefail

BUILD_DIR="${BUILD_DIR:-build}"
FAIL=0

green()  { printf '\033[32m%s\033[0m\n' "$*"; }
red()    { printf '\033[31m%s\033[0m\n' "$*"; }
header() { printf '\n\033[1;36m========== %s ==========\033[0m\n' "$*"; }

REDIS_MAJOR=$(redis-server --version | sed -n 's/.*v=\([0-9]*\).*/\1/p')
REDIS_MAJOR=${REDIS_MAJOR:-0}

# ── Build all module .so targets first so Tcl/soak never test stale binaries ──
header "Build: redis_bloom, redis_json, redis_search"
cmake -B "$BUILD_DIR" >/dev/null && \
  cmake --build "$BUILD_DIR" --target redis_bloom redis_json redis_search -j"$(nproc)" || \
  { red "FAIL: module build"; exit 1; }

# ── GTest unit tests ─────────────────────────────────────────
header "GTest: gemini-bloom"
cmake --build "$BUILD_DIR" --target bloom_test -j"$(nproc)" || \
  { red "FAIL: bloom gtest"; FAIL=1; }

if [ "$REDIS_MAJOR" -ge 7 ]; then
  header "ASan GTest: gemini-bloom"
  cmake -B "${BUILD_DIR}_bloom_asan" -DENABLE_ASAN=ON >/dev/null && \
    cmake --build "${BUILD_DIR}_bloom_asan" --target bloom_test -j"$(nproc)" || \
    { red "FAIL: bloom asan gtest"; FAIL=1; }
fi

header "GTest: gemini-json"
cmake --build "$BUILD_DIR" --target json_test -j"$(nproc)" || \
  { red "FAIL: json gtest"; FAIL=1; }

if [ "$REDIS_MAJOR" -ge 7 ]; then
  header "ASan GTest: gemini-json"
  cmake -B "${BUILD_DIR}_json_asan" -DENABLE_ASAN=ON >/dev/null && \
    cmake --build "${BUILD_DIR}_json_asan" --target json_test -j"$(nproc)" || \
    { red "FAIL: json asan gtest"; FAIL=1; }
fi

header "Tcl: gemini-json"
tclsh modules/gemini-json/tests/tcl/json_test.tcl "./$BUILD_DIR/redis_json.so" || \
  { red "FAIL: json tcl"; FAIL=1; }

if [ "$REDIS_MAJOR" -ge 7 ]; then
  header "Soak: gemini-json (${JSON_SOAK_DURATION_SEC:-30}s, RESP auto)"
  SOAK_DURATION_SEC="${JSON_SOAK_DURATION_SEC:-30}" \
    python3 ci/json_soak_test.py "./$BUILD_DIR/redis_json.so" || \
    { red "FAIL: json soak"; FAIL=1; }

  header "Soak: gemini-json (${JSON_SOAK_DURATION_SEC:-30}s, RESP2 forced)"
  SOAK_DURATION_SEC="${JSON_SOAK_DURATION_SEC:-30}" RESP_PROTOCOL=2 \
    python3 ci/json_soak_test.py "./$BUILD_DIR/redis_json.so" || \
    { red "FAIL: json soak RESP2"; FAIL=1; }
fi

# ── Tcl integration tests (need redis-server) ───────────────
header "Tcl: gemini-bloom"
tclsh modules/gemini-bloom/tests/tcl/bloom_test.tcl "./$BUILD_DIR/redis_bloom.so" || \
  { red "FAIL: bloom tcl"; FAIL=1; }

header "Tcl: gemini-bloom Cuckoo Filter"
tclsh modules/gemini-bloom/tests/tcl/cf_test.tcl "./$BUILD_DIR/redis_bloom.so" || \
  { red "FAIL: cuckoo filter tcl"; FAIL=1; }

header "Tcl: gemini-bloom Count-Min Sketch"
tclsh modules/gemini-bloom/tests/tcl/cms_test.tcl "./$BUILD_DIR/redis_bloom.so" || \
  { red "FAIL: count-min sketch tcl"; FAIL=1; }

header "Tcl: gemini-bloom Top-K"
tclsh modules/gemini-bloom/tests/tcl/topk_test.tcl "./$BUILD_DIR/redis_bloom.so" || \
  { red "FAIL: top-k tcl"; FAIL=1; }

header "Tcl: gemini-bloom T-Digest"
tclsh modules/gemini-bloom/tests/tcl/tdigest_test.tcl "./$BUILD_DIR/redis_bloom.so" || \
  { red "FAIL: t-digest tcl"; FAIL=1; }

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

# ── gemini-search (Redis 7 only) ─────────────────────────────
if [ "$REDIS_MAJOR" -ge 7 ]; then
  header "GTest: gemini-search"
  cmake --build "$BUILD_DIR" --target search_test -j"$(nproc)" || \
    { red "FAIL: search gtest"; FAIL=1; }

  header "ASan GTest: gemini-search"
  cmake -B "${BUILD_DIR}_search_asan" -DENABLE_ASAN=ON >/dev/null && \
    cmake --build "${BUILD_DIR}_search_asan" --target search_test -j"$(nproc)" || \
    { red "FAIL: search asan gtest"; FAIL=1; }

  header "Tcl: gemini-search"
  tclsh modules/gemini-search/tests/tcl/search_test.tcl "./$BUILD_DIR/redis_search.so" || \
    { red "FAIL: search tcl"; FAIL=1; }

  header "Soak: gemini-search (${SEARCH_SOAK_DURATION_SEC:-60}s, RESP auto)"
  SOAK_DURATION_SEC="${SEARCH_SOAK_DURATION_SEC:-60}" \
    python3 ci/search_soak_test.py "./$BUILD_DIR/redis_search.so" || \
    { red "FAIL: search soak"; FAIL=1; }

  header "Soak: gemini-search (${SEARCH_SOAK_DURATION_SEC:-60}s, RESP2 forced)"
  SOAK_DURATION_SEC="${SEARCH_SOAK_DURATION_SEC:-60}" RESP_PROTOCOL=2 \
    python3 ci/search_soak_test.py "./$BUILD_DIR/redis_search.so" || \
    { red "FAIL: search soak RESP2"; FAIL=1; }

  header "SDK Compat: redis-py (bloom + json + search, RESP2 & RESP3)"
  python3 ci/sdk_compat_test.py \
    "./$BUILD_DIR/redis_bloom.so" \
    "./$BUILD_DIR/redis_json.so" \
    "./$BUILD_DIR/redis_search.so" || \
    { red "FAIL: redis-py sdk compat"; FAIL=1; }

  header "Cluster: 3-node Redis 7 Cluster (routing, KeySpecs CROSSSLOT, live MIGRATE)"
  python3 ci/cluster_test.py \
    "./$BUILD_DIR/redis_bloom.so" \
    "./$BUILD_DIR/redis_json.so" || \
    { red "FAIL: redis cluster integration"; FAIL=1; }
fi

# ── Summary ──────────────────────────────────────────────────
echo
if [ "$FAIL" -ne 0 ]; then
  red "Some tests FAILED"
  exit 1
else
  green "All tests PASSED"
fi
