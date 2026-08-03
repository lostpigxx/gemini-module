#!/usr/bin/env bash
set -euo pipefail

CXX="${CXX:-g++}"
BUILD_DIR="${BUILD_DIR:-/tmp/gemini-bloom-gcc73}"
GTEST_ROOT="${GTEST_ROOT:-/usr/src/googletest/googletest}"

if [ ! -f "$GTEST_ROOT/src/gtest-all.cc" ]; then
  GTEST_ROOT=/usr/src/gtest
fi

mkdir -p "$BUILD_DIR"

COMMON_FLAGS=(
  -std=c++14
  -Wall
  -Wextra
  -Wpedantic
  -Werror
  -Iinclude
  -Imodules/gemini-bloom/src
)

BLOOM_CORE=(
  modules/gemini-bloom/src/bloom_filter.cc
  modules/gemini-bloom/src/murmur2.cc
)

echo "Compiler: $($CXX --version | head -1)"

echo "Building redis_bloom.so with strict C++14 flags"
"$CXX" "${COMMON_FLAGS[@]}" -fno-exceptions -fPIC -shared \
  modules/gemini-bloom/src/redis_bloom_module.cc \
  modules/gemini-bloom/src/bloom_filter.cc \
  modules/gemini-bloom/src/sb_chain.cc \
  modules/gemini-bloom/src/bloom_commands.cc \
  modules/gemini-bloom/src/bloom_rdb.cc \
  modules/gemini-bloom/src/bloom_config.cc \
  modules/gemini-bloom/src/murmur2.cc \
  -o "$BUILD_DIR/redis_bloom.so"

echo "Building GTest with GCC 7.3.0"
"$CXX" -std=c++14 -w -I"$GTEST_ROOT" -I"$GTEST_ROOT/include" \
  -c "$GTEST_ROOT/src/gtest-all.cc" -o "$BUILD_DIR/gtest-all.o"
"$CXX" -std=c++14 -w -I"$GTEST_ROOT" -I"$GTEST_ROOT/include" \
  -c "$GTEST_ROOT/src/gtest_main.cc" -o "$BUILD_DIR/gtest-main.o"

TEST_FLAGS=(
  "${COMMON_FLAGS[@]}"
  -DREDIS_BLOOM_TESTING
  -I"$GTEST_ROOT/include"
  -pthread
)
GTEST_OBJECTS=("$BUILD_DIR/gtest-all.o" "$BUILD_DIR/gtest-main.o")

echo "Building gemini-bloom unit tests"
"$CXX" "${TEST_FLAGS[@]}" \
  modules/gemini-bloom/tests/bloom_filter_test.cc \
  "${BLOOM_CORE[@]}" "${GTEST_OBJECTS[@]}" \
  -o "$BUILD_DIR/bloom_filter_test"

"$CXX" "${TEST_FLAGS[@]}" \
  modules/gemini-bloom/tests/sb_chain_test.cc \
  modules/gemini-bloom/src/sb_chain.cc \
  "${BLOOM_CORE[@]}" "${GTEST_OBJECTS[@]}" \
  -o "$BUILD_DIR/sb_chain_test"

"$CXX" "${TEST_FLAGS[@]}" \
  modules/gemini-bloom/tests/bloom_rdb_test.cc \
  modules/gemini-bloom/src/bloom_rdb.cc \
  modules/gemini-bloom/src/sb_chain.cc \
  "${BLOOM_CORE[@]}" "${GTEST_OBJECTS[@]}" \
  -o "$BUILD_DIR/bloom_rdb_test"

echo "Running gemini-bloom unit tests"
"$BUILD_DIR/bloom_filter_test"
"$BUILD_DIR/sb_chain_test"
"$BUILD_DIR/bloom_rdb_test"
