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
  src/cms_sketch.cc
  src/cms_rdb.cc
  src/cms_commands.cc
  src/topk_sketch.cc
  src/topk_rdb.cc
  src/topk_commands.cc
  src/tdigest_sketch.cc
  src/tdigest_rdb.cc
  src/tdigest_commands.cc
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
  target_compile_definitions(cuckoo_filter_test PRIVATE REDIS_BLOOM_TESTING)
  target_compile_options(cuckoo_filter_test PRIVATE -Wall -Wextra -Wpedantic)

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
  target_compile_definitions(cf_chain_test PRIVATE REDIS_BLOOM_TESTING)
  target_compile_options(cf_chain_test PRIVATE -Wall -Wextra -Wpedantic)

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
  target_compile_definitions(cf_rdb_test PRIVATE REDIS_BLOOM_TESTING)
  target_compile_options(cf_rdb_test PRIVATE -Wall -Wextra -Wpedantic -Wno-c++20-designator)

  add_executable(cms_sketch_test EXCLUDE_FROM_ALL
    tests/cms_sketch_test.cc
    src/cms_sketch.cc
    src/murmur2.cc
  )
  target_include_directories(cms_sketch_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(cms_sketch_test PRIVATE
    redis_module_api
    GTest::gtest_main
  )
  target_compile_definitions(cms_sketch_test PRIVATE REDIS_BLOOM_TESTING)
  target_compile_options(cms_sketch_test PRIVATE -Wall -Wextra -Wpedantic)

  add_executable(cms_rdb_test EXCLUDE_FROM_ALL
    tests/cms_rdb_test.cc
    src/cms_rdb.cc
    src/bloom_rdb.cc
    src/sb_chain.cc
    src/bloom_filter.cc
    src/cms_sketch.cc
    src/murmur2.cc
  )
  target_include_directories(cms_rdb_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  target_link_libraries(cms_rdb_test PRIVATE
    redis_module_api
    GTest::gtest_main
  )
  target_compile_definitions(cms_rdb_test PRIVATE REDIS_BLOOM_TESTING)
  target_compile_options(cms_rdb_test PRIVATE -Wall -Wextra -Wpedantic -Wno-c++20-designator)

  add_executable(topk_sketch_test EXCLUDE_FROM_ALL
    tests/topk_sketch_test.cc
    src/topk_sketch.cc
    src/murmur2.cc
  )
  target_include_directories(topk_sketch_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(topk_sketch_test PRIVATE
    redis_module_api
    GTest::gtest_main
  )
  target_compile_definitions(topk_sketch_test PRIVATE REDIS_BLOOM_TESTING)
  target_compile_options(topk_sketch_test PRIVATE -Wall -Wextra -Wpedantic)

  add_executable(topk_rdb_test EXCLUDE_FROM_ALL
    tests/topk_rdb_test.cc
    src/topk_rdb.cc
    src/bloom_rdb.cc
    src/sb_chain.cc
    src/bloom_filter.cc
    src/topk_sketch.cc
    src/murmur2.cc
  )
  target_include_directories(topk_rdb_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  target_link_libraries(topk_rdb_test PRIVATE
    redis_module_api
    GTest::gtest_main
  )
  target_compile_definitions(topk_rdb_test PRIVATE REDIS_BLOOM_TESTING)
  target_compile_options(topk_rdb_test PRIVATE -Wall -Wextra -Wpedantic -Wno-c++20-designator)

  add_executable(tdigest_sketch_test EXCLUDE_FROM_ALL
    tests/tdigest_sketch_test.cc
    src/tdigest_sketch.cc
  )
  target_include_directories(tdigest_sketch_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(tdigest_sketch_test PRIVATE
    redis_module_api
    GTest::gtest_main
  )
  target_compile_definitions(tdigest_sketch_test PRIVATE REDIS_BLOOM_TESTING)
  target_compile_options(tdigest_sketch_test PRIVATE -Wall -Wextra -Wpedantic)

  add_executable(tdigest_rdb_test EXCLUDE_FROM_ALL
    tests/tdigest_rdb_test.cc
    src/tdigest_rdb.cc
    src/bloom_rdb.cc
    src/sb_chain.cc
    src/bloom_filter.cc
    src/tdigest_sketch.cc
    src/murmur2.cc
  )
  target_include_directories(tdigest_rdb_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  target_link_libraries(tdigest_rdb_test PRIVATE
    redis_module_api
    GTest::gtest_main
  )
  target_compile_definitions(tdigest_rdb_test PRIVATE REDIS_BLOOM_TESTING)
  target_compile_options(tdigest_rdb_test PRIVATE -Wall -Wextra -Wpedantic -Wno-c++20-designator)

  add_custom_target(bloom_test
    COMMAND bloom_filter_test --gtest_color=yes
    COMMAND sb_chain_test --gtest_color=yes
    COMMAND bloom_rdb_test --gtest_color=yes
    COMMAND cuckoo_filter_test --gtest_color=yes
    COMMAND cf_chain_test --gtest_color=yes
    COMMAND cf_rdb_test --gtest_color=yes
    COMMAND cms_sketch_test --gtest_color=yes
    COMMAND cms_rdb_test --gtest_color=yes
    COMMAND topk_sketch_test --gtest_color=yes
    COMMAND topk_rdb_test --gtest_color=yes
    COMMAND tdigest_sketch_test --gtest_color=yes
    COMMAND tdigest_rdb_test --gtest_color=yes
    DEPENDS bloom_filter_test sb_chain_test bloom_rdb_test
            cuckoo_filter_test cf_chain_test cf_rdb_test
            cms_sketch_test cms_rdb_test
            topk_sketch_test topk_rdb_test
            tdigest_sketch_test tdigest_rdb_test
  )
endif()
