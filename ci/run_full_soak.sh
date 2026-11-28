#!/usr/bin/env bash
# Standalone launcher for the Gemini Full-Stack Soak Test (Master + Replica, 3 modules).
# Intentionally NOT called by ci/run_tests.sh — trigger manually or from a nightly/release job.
#
# Usage:
#   ci/run_full_soak.sh [smoke|mini|4g1h|full|marathon] [extra orchestrator args...]
#
# Examples:
#   ci/run_full_soak.sh smoke
#   ci/run_full_soak.sh mini --seed 12345 --keep-on-fail
#   ci/run_full_soak.sh 4g1h
#   ci/run_full_soak.sh full --maxmemory-mb 4096 --duration 86400
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

MODE="${1:-full}"
if [ $# -gt 0 ]; then
  shift
fi

BUILD_DIR="${BUILD_DIR:-build}"

# Pick a python3 interpreter that has redis-py installed
PYTHON_BIN="${PYTHON_BIN:-}"
if [ -z "$PYTHON_BIN" ]; then
  for cand in "$(command -v python3 || true)" /usr/bin/python3 /opt/homebrew/bin/python3; do
    if [ -n "$cand" ] && [ -x "$cand" ] && "$cand" -c "import redis" >/dev/null 2>&1; then
      PYTHON_BIN="$cand"
      break
    fi
  done
fi

if [ -z "$PYTHON_BIN" ]; then
  echo "ERROR: No python3 with 'redis' package found. Install redis-py or set PYTHON_BIN." >&2
  exit 2
fi

# Ensure all 3 modules are built
if [ ! -f "$BUILD_DIR/redis_json.so" ] || [ ! -f "$BUILD_DIR/redis_bloom.so" ] || [ ! -f "$BUILD_DIR/redis_search.so" ]; then
  NPROC=$(command -v nproc >/dev/null 2>&1 && nproc || sysctl -n hw.ncpu)
  cmake -B "$BUILD_DIR" >/dev/null
  cmake --build "$BUILD_DIR" --target redis_bloom redis_json redis_search -j"$NPROC"
fi

exec "$PYTHON_BIN" -m ci.soak_full.orchestrator \
  --mode "$MODE" \
  --build-dir "$BUILD_DIR" \
  "$@"
