set(SEARCH_SOURCES
  src/redis_search_module.cc
  src/index_spec.cc
  src/search_commands.cc
)

add_library(redis_search SHARED ${SEARCH_SOURCES})

target_include_directories(redis_search PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(redis_search PRIVATE redis_module_api)

target_compile_options(redis_search PRIVATE
  -Wall -Wextra -Wpedantic -fno-exceptions -Wno-c++20-designator
)

set_target_properties(redis_search PROPERTIES
  PREFIX ""
  SUFFIX ".so"
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

find_package(GTest QUIET)
if(GTest_FOUND)
  add_executable(index_spec_test EXCLUDE_FROM_ALL
    tests/index_spec_test.cc
    src/index_spec.cc
  )
  target_include_directories(index_spec_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(index_spec_test PRIVATE
    redis_module_api
    GTest::gtest_main
  )
  target_compile_definitions(index_spec_test PRIVATE GEMINI_SEARCH_TESTING)
  target_compile_options(index_spec_test PRIVATE -Wall -Wextra -Wpedantic)

  add_executable(search_asan_test EXCLUDE_FROM_ALL
    tests/search_asan_test.cc
    src/index_spec.cc
  )
  target_include_directories(search_asan_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(search_asan_test PRIVATE
    redis_module_api
    GTest::gtest_main
  )
  target_compile_definitions(search_asan_test PRIVATE GEMINI_SEARCH_TESTING)
  target_compile_options(search_asan_test PRIVATE -Wall -Wextra -Wpedantic)

  add_custom_target(search_test
    COMMAND index_spec_test --gtest_color=yes
    COMMAND search_asan_test --gtest_color=yes
    DEPENDS index_spec_test search_asan_test
  )
endif()
