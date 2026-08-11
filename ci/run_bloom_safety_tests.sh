#!/usr/bin/env bash
set -euo pipefail

GEMINI_MODULE=${1:-./build/redis_bloom.so}
REDISBLOOM_MODULE=${2:-/opt/redisbloom.so}
JOBS=${JOBS:-$(nproc)}

header() { printf '\n\033[1;36m========== %s ==========\033[0m\n' "$1"; }

header "ASan + UBSan: core tests"
cmake -S . -B build-asan-ubsan -DENABLE_ASAN=ON -DENABLE_UBSAN=ON
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  cmake --build build-asan-ubsan --target redis_bloom bloom_test -j"$JOBS"

header "ASan + UBSan: Redis integration"
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1:abort_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  python3 ci/bloom_runtime_safety_test.py \
    build-asan-ubsan/redis_bloom.so \
    --label gemini-asan-ubsan \
    --preload "$(gcc -print-file-name=libasan.so)"

header "TSan: core and Redis integration"
cmake -S . -B build-tsan -DENABLE_TSAN=ON
TSAN_OPTIONS=halt_on_error=1:history_size=7:second_deadlock_stack=1 \
  cmake --build build-tsan --target redis_bloom bloom_test -j"$JOBS"
TSAN_OPTIONS=halt_on_error=1:history_size=7:second_deadlock_stack=1 \
  python3 ci/bloom_runtime_safety_test.py \
    build-tsan/redis_bloom.so \
    --label gemini-tsan \
    --preload "$(gcc -print-file-name=libtsan.so)"

header "Reference functional control"
python3 ci/bloom_runtime_safety_test.py \
  "$REDISBLOOM_MODULE" \
  --label redisbloom-control

header "Valgrind: gemini core tests"
cmake --build build --target bloom_filter_test sb_chain_test bloom_rdb_test -j"$JOBS"
for test_binary in \
  build/modules/gemini-bloom/bloom_filter_test \
  build/modules/gemini-bloom/sb_chain_test \
  build/modules/gemini-bloom/bloom_rdb_test; do
  valgrind \
    --leak-check=full \
    --show-leak-kinds=definite \
    --errors-for-leak-kinds=definite \
    --error-exitcode=86 \
    "$test_binary"
done

header "Valgrind: Redis integration and reference control"
python3 ci/bloom_runtime_safety_test.py \
  "$GEMINI_MODULE" \
  --label gemini-valgrind \
  --valgrind
python3 ci/bloom_runtime_safety_test.py \
  "$REDISBLOOM_MODULE" \
  --label redisbloom-valgrind-control \
  --valgrind

printf '\n\033[32mSanitizer and Valgrind tests PASSED\033[0m\n'
