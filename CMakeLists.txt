cmake_minimum_required(VERSION 3.14)
project(redis_bloom LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Redis module shared library
add_library(redis_bloom SHARED
  src/redis_bloom_module.cc
  src/bloom_filter.cc
  src/sb_chain.cc
  src/bloom_commands.cc
  src/bloom_rdb.cc
  src/bloom_config.cc
  src/murmur2.cc
)

target_include_directories(redis_bloom PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_options(redis_bloom PRIVATE -Wall -Wextra -Wpedantic -fno-exceptions -Wno-c++20-designator)

set_target_properties(redis_bloom PROPERTIES
  PREFIX ""
  SUFFIX ".so"
)

# AddressSanitizer support
option(ENABLE_ASAN "Build with AddressSanitizer" OFF)
if(ENABLE_ASAN)
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address)
endif()

# GTest-based unit tests (excluded from default build, built on demand via `make test`)
find_package(GTest QUIET)
if(GTest_FOUND)
  add_executable(bloom_filter_test EXCLUDE_FROM_ALL
    test/bloom_filter_test.cc
    src/bloom_filter.cc
    src/murmur2.cc
  )
  target_include_directories(bloom_filter_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_definitions(bloom_filter_test PRIVATE REDIS_BLOOM_TESTING)
  target_link_libraries(bloom_filter_test PRIVATE GTest::gtest_main)
  target_compile_options(bloom_filter_test PRIVATE -Wall -Wextra -Wpedantic)

  add_executable(sb_chain_test EXCLUDE_FROM_ALL
    test/sb_chain_test.cc
    src/sb_chain.cc
    src/bloom_filter.cc
    src/murmur2.cc
  )
  target_include_directories(sb_chain_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_definitions(sb_chain_test PRIVATE REDIS_BLOOM_TESTING)
  target_link_libraries(sb_chain_test PRIVATE GTest::gtest_main)
  target_compile_options(sb_chain_test PRIVATE -Wall -Wextra -Wpedantic)

  add_custom_target(test
    COMMAND bloom_filter_test --gtest_color=yes
    COMMAND sb_chain_test --gtest_color=yes
    DEPENDS bloom_filter_test sb_chain_test
  )
endif()
