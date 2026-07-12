set(BLOOM_SOURCES
  src/redis_bloom_module.cc
  src/bloom_filter.cc
  src/sb_chain.cc
  src/bloom_commands.cc
  src/bloom_rdb.cc
  src/bloom_config.cc
  src/murmur2.cc
)

add_library(redis_bloom SHARED ${BLOOM_SOURCES})

target_include_directories(redis_bloom PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(redis_bloom PRIVATE redis_module_api)

target_compile_options(redis_bloom PRIVATE
  -Wall -Wextra -Wpedantic -fno-exceptions -Wno-c++20-designator
)

set_target_properties(redis_bloom PROPERTIES
  PREFIX ""
  SUFFIX ".so"
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

if(ENABLE_FUZZING)
  add_executable(bloom_fuzzer EXCLUDE_FROM_ALL
    fuzz/bloom_fuzzer.cc
    src/bloom_rdb.cc
    src/sb_chain.cc
    src/bloom_filter.cc
    src/murmur2.cc
  )
  target_include_directories(bloom_fuzzer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  target_link_libraries(bloom_fuzzer PRIVATE redis_module_api)
  target_compile_definitions(bloom_fuzzer PRIVATE REDIS_BLOOM_TESTING)
  target_compile_options(bloom_fuzzer PRIVATE -Wall -Wextra -Wpedantic)
  configure_fuzzer(bloom_fuzzer)
endif()

find_package(GTest QUIET)
if(GTest_FOUND)
  add_executable(bloom_filter_test EXCLUDE_FROM_ALL
    tests/bloom_filter_test.cc
    src/bloom_filter.cc
    src/murmur2.cc
  )
  target_include_directories(bloom_filter_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(bloom_filter_test PRIVATE
    redis_module_api
    GTest::gtest_main
  )
  target_compile_definitions(bloom_filter_test PRIVATE REDIS_BLOOM_TESTING)
  target_compile_options(bloom_filter_test PRIVATE -Wall -Wextra -Wpedantic)

  add_executable(sb_chain_test EXCLUDE_FROM_ALL
    tests/sb_chain_test.cc
    src/sb_chain.cc
    src/bloom_filter.cc
    src/murmur2.cc
  )
  target_include_directories(sb_chain_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(sb_chain_test PRIVATE
    redis_module_api
    GTest::gtest_main
  )
  target_compile_definitions(sb_chain_test PRIVATE REDIS_BLOOM_TESTING)
  target_compile_options(sb_chain_test PRIVATE -Wall -Wextra -Wpedantic)

  add_executable(bloom_rdb_test EXCLUDE_FROM_ALL
    tests/bloom_rdb_test.cc
    src/bloom_rdb.cc
    src/sb_chain.cc
    src/bloom_filter.cc
    src/murmur2.cc
  )
  target_include_directories(bloom_rdb_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  target_link_libraries(bloom_rdb_test PRIVATE
    redis_module_api
    GTest::gtest_main
  )
  target_compile_definitions(bloom_rdb_test PRIVATE REDIS_BLOOM_TESTING)
  target_compile_options(bloom_rdb_test PRIVATE -Wall -Wextra -Wpedantic -Wno-c++20-designator)

  add_custom_target(bloom_test
    COMMAND bloom_filter_test --gtest_color=yes
    COMMAND sb_chain_test --gtest_color=yes
    COMMAND bloom_rdb_test --gtest_color=yes
    DEPENDS bloom_filter_test sb_chain_test bloom_rdb_test
  )
endif()
