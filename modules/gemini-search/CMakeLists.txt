set(SEARCH_SOURCES
  src/redis_search_module.cc
  src/index_spec.cc
  src/search_commands.cc
  src/document_store.cc
  src/tag_index.cc
  src/text_index.cc
  src/query_parser.cc
  src/numeric_index.cc
  src/vector_index.cc
  src/hnsw_index.cc
  src/search_rdb.cc
  src/stemmer.cc
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

  add_executable(document_store_test EXCLUDE_FROM_ALL
    tests/document_store_test.cc
    src/document_store.cc
  )
  target_include_directories(document_store_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(document_store_test PRIVATE
    GTest::gtest_main
  )
  target_compile_definitions(document_store_test PRIVATE GEMINI_SEARCH_TESTING)
  target_compile_options(document_store_test PRIVATE -Wall -Wextra -Wpedantic)

  add_executable(tag_index_test EXCLUDE_FROM_ALL
    tests/tag_index_test.cc
    src/tag_index.cc
  )
  target_include_directories(tag_index_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(tag_index_test PRIVATE
    GTest::gtest_main
  )
  target_compile_definitions(tag_index_test PRIVATE GEMINI_SEARCH_TESTING)
  target_compile_options(tag_index_test PRIVATE -Wall -Wextra -Wpedantic)

  add_executable(numeric_index_test EXCLUDE_FROM_ALL
    tests/numeric_index_test.cc
    src/numeric_index.cc
  )
  target_include_directories(numeric_index_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(numeric_index_test PRIVATE
    GTest::gtest_main
  )
  target_compile_definitions(numeric_index_test PRIVATE GEMINI_SEARCH_TESTING)
  target_compile_options(numeric_index_test PRIVATE -Wall -Wextra -Wpedantic)

  add_executable(hnsw_index_test EXCLUDE_FROM_ALL
    tests/hnsw_index_test.cc
    src/hnsw_index.cc
    src/vector_index.cc
    src/index_spec.cc
  )
  target_include_directories(hnsw_index_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(hnsw_index_test PRIVATE
    redis_module_api
    GTest::gtest_main
  )
  target_compile_definitions(hnsw_index_test PRIVATE GEMINI_SEARCH_TESTING)
  target_compile_options(hnsw_index_test PRIVATE -Wall -Wextra -Wpedantic)

  add_executable(vector_index_test EXCLUDE_FROM_ALL
    tests/vector_index_test.cc
    src/vector_index.cc
    src/hnsw_index.cc
    src/index_spec.cc
  )
  target_include_directories(vector_index_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(vector_index_test PRIVATE
    redis_module_api
    GTest::gtest_main
  )
  target_compile_definitions(vector_index_test PRIVATE GEMINI_SEARCH_TESTING)
  target_compile_options(vector_index_test PRIVATE -Wall -Wextra -Wpedantic)

  add_executable(text_index_test EXCLUDE_FROM_ALL
    tests/text_index_test.cc
    src/text_index.cc
    src/stemmer.cc
  )
  target_include_directories(text_index_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(text_index_test PRIVATE
    GTest::gtest_main
  )
  target_compile_definitions(text_index_test PRIVATE GEMINI_SEARCH_TESTING)
  target_compile_options(text_index_test PRIVATE -Wall -Wextra -Wpedantic)

  add_executable(query_parser_test EXCLUDE_FROM_ALL
    tests/query_parser_test.cc
    src/query_parser.cc
    src/index_spec.cc
    src/document_store.cc
    src/tag_index.cc
    src/text_index.cc
    src/stemmer.cc
    src/numeric_index.cc
  )
  target_include_directories(query_parser_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(query_parser_test PRIVATE
    redis_module_api
    GTest::gtest_main
  )
  target_compile_definitions(query_parser_test PRIVATE GEMINI_SEARCH_TESTING)
  target_compile_options(query_parser_test PRIVATE -Wall -Wextra -Wpedantic)

  add_executable(search_rdb_test EXCLUDE_FROM_ALL
    tests/search_rdb_test.cc
    src/search_rdb.cc
    src/search_commands.cc
    src/index_spec.cc
    src/document_store.cc
    src/tag_index.cc
    src/text_index.cc
    src/query_parser.cc
    src/numeric_index.cc
    src/vector_index.cc
    src/hnsw_index.cc
    src/stemmer.cc
  )
  target_include_directories(search_rdb_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  target_link_libraries(search_rdb_test PRIVATE
    redis_module_api
    GTest::gtest_main
  )
  target_compile_definitions(search_rdb_test PRIVATE GEMINI_SEARCH_TESTING)
  target_compile_options(search_rdb_test PRIVATE -Wall -Wextra -Wpedantic -Wno-c++20-designator)

  add_executable(search_asan_test EXCLUDE_FROM_ALL
    tests/search_asan_test.cc
    src/index_spec.cc
    src/document_store.cc
    src/tag_index.cc
    src/text_index.cc
    src/query_parser.cc
    src/numeric_index.cc
    src/vector_index.cc
    src/hnsw_index.cc
    src/stemmer.cc
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
    COMMAND document_store_test --gtest_color=yes
    COMMAND tag_index_test --gtest_color=yes
    COMMAND text_index_test --gtest_color=yes
    COMMAND numeric_index_test --gtest_color=yes
    COMMAND vector_index_test --gtest_color=yes
    COMMAND hnsw_index_test --gtest_color=yes
    COMMAND query_parser_test --gtest_color=yes
    COMMAND search_rdb_test --gtest_color=yes
    COMMAND search_asan_test --gtest_color=yes
    DEPENDS index_spec_test document_store_test tag_index_test text_index_test numeric_index_test vector_index_test hnsw_index_test query_parser_test search_rdb_test search_asan_test
  )
endif()
