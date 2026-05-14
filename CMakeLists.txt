cmake_minimum_required(VERSION 3.14)
project(gemini_modules LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(ENABLE_ASAN "Build with AddressSanitizer" OFF)
if(ENABLE_ASAN)
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address)
endif()

add_library(redis_module_api INTERFACE)
target_include_directories(redis_module_api INTERFACE
  ${CMAKE_SOURCE_DIR}/include
)

add_subdirectory(modules/gemini-bloom)
