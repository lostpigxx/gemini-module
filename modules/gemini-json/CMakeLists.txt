set(JSON_SOURCES
  src/redis_json_module.cc
  src/json_value.cc
  src/json_parse.cc
  src/json_serialize.cc
  src/json_path.cc
  src/json_type.cc
  src/json_commands.cc
)

add_library(redis_json SHARED ${JSON_SOURCES})

target_include_directories(redis_json PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(redis_json PRIVATE redis_module_api)

target_compile_options(redis_json PRIVATE
  -Wall -Wextra -Wpedantic -fno-exceptions -Wno-c++20-designator -Wno-nested-anon-types -Wno-missing-field-initializers
)

set_target_properties(redis_json PROPERTIES
  PREFIX ""
  SUFFIX ".so"
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

find_package(GTest QUIET)
if(GTest_FOUND)
  set(JSON_TEST_COMMON
    src/json_value.cc
    src/json_parse.cc
    src/json_serialize.cc
    src/json_path.cc
  )

  add_executable(json_value_test EXCLUDE_FROM_ALL
    tests/json_value_test.cc
    ${JSON_TEST_COMMON}
  )
  target_include_directories(json_value_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(json_value_test PRIVATE
    redis_module_api
    GTest::gtest_main
  )
  target_compile_definitions(json_value_test PRIVATE REDIS_JSON_TESTING)
  target_compile_options(json_value_test PRIVATE -Wall -Wextra -Wpedantic)

  add_executable(json_parse_test EXCLUDE_FROM_ALL
    tests/json_parse_test.cc
    ${JSON_TEST_COMMON}
  )
  target_include_directories(json_parse_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(json_parse_test PRIVATE
    redis_module_api
    GTest::gtest_main
  )
  target_compile_definitions(json_parse_test PRIVATE REDIS_JSON_TESTING)
  target_compile_options(json_parse_test PRIVATE -Wall -Wextra -Wpedantic)

  add_executable(json_path_test EXCLUDE_FROM_ALL
    tests/json_path_test.cc
    ${JSON_TEST_COMMON}
  )
  target_include_directories(json_path_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(json_path_test PRIVATE
    redis_module_api
    GTest::gtest_main
  )
  target_compile_definitions(json_path_test PRIVATE REDIS_JSON_TESTING)
  target_compile_options(json_path_test PRIVATE -Wall -Wextra -Wpedantic)

  add_executable(json_asan_test EXCLUDE_FROM_ALL
    tests/json_asan_test.cc
    ${JSON_TEST_COMMON}
  )
  target_include_directories(json_asan_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(json_asan_test PRIVATE
    redis_module_api
    GTest::gtest_main
  )
  target_compile_definitions(json_asan_test PRIVATE REDIS_JSON_TESTING)
  target_compile_options(json_asan_test PRIVATE -Wall -Wextra -Wpedantic)

  add_executable(json_rdb_test EXCLUDE_FROM_ALL
    tests/json_rdb_test.cc
    src/json_value.cc
    src/json_parse.cc
    src/json_serialize.cc
    src/json_path.cc
    src/json_type.cc
  )
  target_include_directories(json_rdb_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  target_link_libraries(json_rdb_test PRIVATE
    redis_module_api
    GTest::gtest_main
  )
  target_compile_definitions(json_rdb_test PRIVATE REDIS_JSON_TESTING)
  target_compile_options(json_rdb_test PRIVATE -Wall -Wextra -Wpedantic -Wno-c++20-designator)

  add_custom_target(json_test
    COMMAND json_value_test --gtest_color=yes
    COMMAND json_parse_test --gtest_color=yes
    COMMAND json_path_test --gtest_color=yes
    COMMAND json_asan_test --gtest_color=yes
    COMMAND json_rdb_test --gtest_color=yes
    DEPENDS json_value_test json_parse_test json_path_test json_asan_test json_rdb_test
  )
endif()
