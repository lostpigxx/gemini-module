#!/usr/bin/env bash
# Unified benchmark runner for gemini-bloom, gemini-json, and gemini-search.
#
# Usage:
#   bash ci/run_benchmarks.sh [--build-dir build_bench] [--out report.md] [bloom|json|search ...]
set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build}"
OUT_FILE=""
TARGETS=()

while [ $# -gt 0 ]; do
  case "$1" in
    --build-dir)
      BUILD_DIR="$2"
      shift 2
      ;;
    --out)
      OUT_FILE="$2"
      shift 2
      ;;
    bloom|json|search)
      TARGETS+=("$1")
      shift
      ;;
    -h|--help)
      echo "Usage: $0 [--build-dir DIR] [--out FILE.md] [bloom|json|search ...]"
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      exit 1
      ;;
  esac
done

if [ ${#TARGETS[@]} -eq 0 ]; then
  TARGETS=(bloom json search)
fi

NPROC=$(command -v nproc >/dev/null 2>&1 && nproc || sysctl -n hw.ncpu)

CMAKE_TARGETS=()
for t in "${TARGETS[@]}"; do
  CMAKE_TARGETS+=("${t}_bench")
done

if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
  cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release >/dev/null
fi

echo "==> Building ${CMAKE_TARGETS[*]} in $BUILD_DIR..."
cmake --build "$BUILD_DIR" --target "${CMAKE_TARGETS[@]}" -j"$NPROC"

run_one() {
  local mod="$1"
  local bin="$BUILD_DIR/modules/gemini-${mod}/${mod}_bench"
  echo
  echo "============================================================"
  echo "  gemini-${mod} benchmark (${bin})"
  echo "============================================================"
  "$bin"
}

if [ -n "$OUT_FILE" ]; then
  {
    echo "# gemini-module Benchmark Report"
    echo
    echo "- Date: $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
    echo "- Commit: $(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
    echo "- Host: $(uname -s) $(uname -m)"
    echo
    for t in "${TARGETS[@]}"; do
      echo "## gemini-${t}"
      echo
      echo '```'
      "$BUILD_DIR/modules/gemini-${t}/${t}_bench"
      echo '```'
      echo
    done
  } | tee "$OUT_FILE"
  echo "Saved benchmark report to $OUT_FILE"
else
  for t in "${TARGETS[@]}"; do
    run_one "$t"
  done
fi
