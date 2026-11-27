#!/usr/bin/env bash
set -uo pipefail

BUILD_DIR="${BUILD_DIR:-build}"
NPROC=$(command -v nproc >/dev/null 2>&1 && nproc || sysctl -n hw.ncpu)
FAIL=0
START_TS=$(date +%s)
LOG_DIR=$(mktemp -d /tmp/gemini_ci_logs.XXXXXX)
trap 'rm -rf "$LOG_DIR"' EXIT

green()  { printf '\033[32m%s\033[0m\n' "$*"; }
red()    { printf '\033[31m%s\033[0m\n' "$*"; }
header() { printf '\n\033[1;36m========== %s ==========\033[0m\n' "$*"; }

REDIS_MAJOR=$(redis-server --version | sed -n 's/.*v=\([0-9]*\).*/\1/p')
REDIS_MAJOR=${REDIS_MAJOR:-0}

# Parallel job runner helpers
PIDS=()
JOB_NAMES=()
JOB_LOGS=()

launch_job() {
  local name="$1"
  shift
  local safe_name
  safe_name=$(printf '%s' "$name" | tr ' /():' '_____')
  local log_file="$LOG_DIR/${safe_name}.log"
  local t0
  t0=$(date +%s)
  (
    "$@" >"$log_file" 2>&1
    rc=$?
    t1=$(date +%s)
    elapsed=$((t1 - t0))
    if [ "$rc" -eq 0 ]; then
      printf '\033[32m  [PASS %3ds]\033[0m %s\n' "$elapsed" "$name"
    else
      printf '\033[31m  [FAIL %3ds]\033[0m %s\n' "$elapsed" "$name"
    fi
    exit "$rc"
  ) &
  PIDS+=($!)
  JOB_NAMES+=("$name")
  JOB_LOGS+=("$log_file")
}

wait_all_jobs() {
  local idx
  for idx in "${!PIDS[@]}"; do
    if ! wait "${PIDS[$idx]}"; then
      FAIL=1
      red "--- Failure log: ${JOB_NAMES[$idx]} ---"
      tail -n 80 "${JOB_LOGS[$idx]}" || true
      red "--- End log: ${JOB_NAMES[$idx]} ---"
    fi
  done
  PIDS=()
  JOB_NAMES=()
  JOB_LOGS=()
}

# ── Stage 1: Build .so modules & run Normal + ASan GTest ─────
header "Stage 1: Build modules & GTest (Normal + ASan, -j$NPROC)"
cmake -B "$BUILD_DIR" >/dev/null && \
  cmake --build "$BUILD_DIR" --target redis_bloom redis_json redis_search -j"$NPROC" || \
  { red "FAIL: module build"; exit 1; }

# Run normal GTest and unified ASan GTest in parallel
launch_job "GTest: bloom + json + search (normal)" \
  bash -c "cmake --build '$BUILD_DIR' --target bloom_test json_test search_test -j$(( NPROC > 2 ? NPROC / 2 : 1 ))"

if [ "$REDIS_MAJOR" -ge 7 ]; then
  launch_job "GTest: bloom + json + search (ASan)" \
    bash -c "cmake -B '${BUILD_DIR}_asan' -DENABLE_ASAN=ON >/dev/null && cmake --build '${BUILD_DIR}_asan' --target bloom_test json_test search_test -j$(( NPROC > 2 ? NPROC / 2 : 1 ))"
fi

# ── Stage 2: Launch all Tcl, Compat, Migrate, SDK, Cluster & Soak in parallel ─
header "Stage 2: Parallel Integration, Compat, Migrate, SDK, Cluster & Soak"

# Tcl suites (all 7 have isolated /tmp files and dynamic ports)
launch_job "Tcl: gemini-bloom (BF)"      tclsh modules/gemini-bloom/tests/tcl/bloom_test.tcl "./$BUILD_DIR/redis_bloom.so"
launch_job "Tcl: gemini-bloom (CF)"      tclsh modules/gemini-bloom/tests/tcl/cf_test.tcl "./$BUILD_DIR/redis_bloom.so"
launch_job "Tcl: gemini-bloom (CMS)"     tclsh modules/gemini-bloom/tests/tcl/cms_test.tcl "./$BUILD_DIR/redis_bloom.so"
launch_job "Tcl: gemini-bloom (Top-K)"   tclsh modules/gemini-bloom/tests/tcl/topk_test.tcl "./$BUILD_DIR/redis_bloom.so"
launch_job "Tcl: gemini-bloom (T-Digest)" tclsh modules/gemini-bloom/tests/tcl/tdigest_test.tcl "./$BUILD_DIR/redis_bloom.so"
launch_job "Tcl: gemini-json"            tclsh modules/gemini-json/tests/tcl/json_test.tcl "./$BUILD_DIR/redis_json.so"
if [ "$REDIS_MAJOR" -ge 7 ]; then
  launch_job "Tcl: gemini-search"        tclsh modules/gemini-search/tests/tcl/search_test.tcl "./$BUILD_DIR/redis_search.so"
fi

# RedisBloom Compat & RDB Migration (RESP3 + RESP2)
launch_job "Compat: bloom vs RedisBloom (RESP3)" python3 ci/bloom_compat_test.py "./$BUILD_DIR/redis_bloom.so" /opt/redisbloom.so
launch_job "Compat: bloom vs RedisBloom (RESP2)" env RESP_PROTOCOL=2 python3 ci/bloom_compat_test.py "./$BUILD_DIR/redis_bloom.so" /opt/redisbloom.so
launch_job "Migrate: RDB round-trip (RESP3)"     python3 ci/bloom_migrate_test.py "./$BUILD_DIR/redis_bloom.so" /opt/redisbloom.so
launch_job "Migrate: RDB round-trip (RESP2)"     env RESP_PROTOCOL=2 python3 ci/bloom_migrate_test.py "./$BUILD_DIR/redis_bloom.so" /opt/redisbloom.so

# SDK Compat & 3-node Redis 7 Cluster
if [ "$REDIS_MAJOR" -ge 7 ]; then
  launch_job "SDK Compat: redis-py (RESP2 & RESP3)" \
    python3 ci/sdk_compat_test.py "./$BUILD_DIR/redis_bloom.so" "./$BUILD_DIR/redis_json.so" "./$BUILD_DIR/redis_search.so"
  launch_job "Cluster: 3-node Redis 7 Cluster" \
    python3 ci/cluster_test.py "./$BUILD_DIR/redis_bloom.so" "./$BUILD_DIR/redis_json.so" "./$BUILD_DIR/redis_search.so"
fi

# Soak tests (all 6 in parallel!)
launch_job "Soak: gemini-bloom (${SOAK_DURATION_SEC:-300}s, RESP3)" \
  env SOAK_DURATION_SEC="${SOAK_DURATION_SEC:-300}" python3 ci/bloom_soak_test.py "./$BUILD_DIR/redis_bloom.so"
launch_job "Soak: gemini-bloom (${SOAK_DURATION_SEC:-300}s, RESP2)" \
  env SOAK_DURATION_SEC="${SOAK_DURATION_SEC:-300}" RESP_PROTOCOL=2 python3 ci/bloom_soak_test.py "./$BUILD_DIR/redis_bloom.so"

if [ "$REDIS_MAJOR" -ge 7 ]; then
  launch_job "Soak: gemini-json (${JSON_SOAK_DURATION_SEC:-30}s, RESP3)" \
    env SOAK_DURATION_SEC="${JSON_SOAK_DURATION_SEC:-30}" python3 ci/json_soak_test.py "./$BUILD_DIR/redis_json.so"
  launch_job "Soak: gemini-json (${JSON_SOAK_DURATION_SEC:-30}s, RESP2)" \
    env SOAK_DURATION_SEC="${JSON_SOAK_DURATION_SEC:-30}" RESP_PROTOCOL=2 python3 ci/json_soak_test.py "./$BUILD_DIR/redis_json.so"
  launch_job "Soak: gemini-search (${SEARCH_SOAK_DURATION_SEC:-60}s, RESP3)" \
    env SOAK_DURATION_SEC="${SEARCH_SOAK_DURATION_SEC:-60}" python3 ci/search_soak_test.py "./$BUILD_DIR/redis_search.so"
  launch_job "Soak: gemini-search (${SEARCH_SOAK_DURATION_SEC:-60}s, RESP2)" \
    env SOAK_DURATION_SEC="${SEARCH_SOAK_DURATION_SEC:-60}" RESP_PROTOCOL=2 python3 ci/search_soak_test.py "./$BUILD_DIR/redis_search.so"
fi

wait_all_jobs

# ── Summary ──────────────────────────────────────────────────
ELAPSED=$(( $(date +%s) - START_TS ))
echo
if [ "$FAIL" -ne 0 ]; then
  red "Some tests FAILED (total ${ELAPSED}s)"
  exit 1
else
  green "All tests PASSED in ${ELAPSED}s"
fi
