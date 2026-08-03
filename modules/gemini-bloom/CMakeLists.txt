set(BLOOM_SOURCES
  src/redis_bloom_module.cc
  src/bloom_filter.cc
  src/sb_chain.cc
  src/bloom_commands.cc
  src/bloom_rdb.cc
  src/bloom_config.cc
  src/murmur2.cc
  src/cuckoo_filter.cc
  src/cf_chain.cc
  src/cf_commands.cc
  src/cf_rdb.cc
  src/cf_config.cc
)

add_library(redis_bloom SHARED ${BLOOM_SOURCES})
set_target_properties(redis_bloom PROPERTIES
  CXX_STANDARD 14
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

target_include_directories(redis_bloom PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(redis_bloom PRIVATE redis_module_api)

target_compile_options(redis_bloom PRIVATE
  -Wall -Wextra -Wpedantic -fno-exceptions
)

set_target_properties(redis_bloom PROPERTIES
  PREFIX ""
  SUFFIX ".so"
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

find_package(GTest QUIET)
if(GTest_FOUND)
  function(configure_bloom_test_target target)
    set_target_properties(${target} PROPERTIES
      CXX_STANDARD 14
      CXX_STANDARD_REQUIRED ON
      CXX_EXTENSIONS OFF
    )
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_definitions(${target} PRIVATE REDIS_BLOOM_TESTING)
  endfunction()

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
  configure_bloom_test_target(bloom_filter_test)

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
  configure_bloom_test_target(sb_chain_test)

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
  configure_bloom_test_target(bloom_rdb_test)

  add_executable(cuckoo_filter_test EXCLUDE_FROM_ALL
    tests/cuckoo_filter_test.cc
    src/cuckoo_filter.cc
    src/murmur2.cc
  )
  target_include_directories(cuckoo_filter_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(cuckoo_filter_test PRIVATE
    redis_module_api
    GTest::gtest_main
  )
  configure_bloom_test_target(cuckoo_filter_test)

  add_executable(cf_chain_test EXCLUDE_FROM_ALL
    tests/cf_chain_test.cc
    src/cf_chain.cc
    src/cuckoo_filter.cc
    src/murmur2.cc
  )
  target_include_directories(cf_chain_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(cf_chain_test PRIVATE
    redis_module_api
    GTest::gtest_main
  )
  configure_bloom_test_target(cf_chain_test)

  add_executable(cf_rdb_test EXCLUDE_FROM_ALL
    tests/cf_rdb_test.cc
    src/cf_rdb.cc
    src/bloom_rdb.cc
    src/sb_chain.cc
    src/bloom_filter.cc
    src/cf_chain.cc
    src/cuckoo_filter.cc
    src/murmur2.cc
  )
  target_include_directories(cf_rdb_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  target_link_libraries(cf_rdb_test PRIVATE
    redis_module_api
    GTest::gtest_main
  )
  configure_bloom_test_target(cf_rdb_test)

  add_custom_target(bloom_test
    COMMAND bloom_filter_test --gtest_color=yes
    COMMAND sb_chain_test --gtest_color=yes
    COMMAND bloom_rdb_test --gtest_color=yes
    COMMAND cuckoo_filter_test --gtest_color=yes
    COMMAND cf_chain_test --gtest_color=yes
    COMMAND cf_rdb_test --gtest_color=yes
    DEPENDS bloom_filter_test sb_chain_test bloom_rdb_test
            cuckoo_filter_test cf_chain_test cf_rdb_test
  )
endif()
