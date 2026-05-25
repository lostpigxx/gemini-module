set(SEARCH_SOURCES
  src/redis_search_module.cc
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
