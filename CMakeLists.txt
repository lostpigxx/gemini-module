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

option(ENABLE_FUZZING "Build libFuzzer targets" OFF)
if(ENABLE_FUZZING)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "ENABLE_FUZZING requires Clang with libFuzzer support")
  endif()

  include(CheckCXXSourceCompiles)
  set(_GEMINI_OLD_CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS}")
  set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -fsanitize=fuzzer,address,undefined")
  check_cxx_source_compiles([=[
    #include <cstddef>
    #include <cstdint>
    extern "C" int LLVMFuzzerTestOneInput(const uint8_t*, size_t) { return 0; }
  ]=] GEMINI_HAS_LIBFUZZER)
  set(CMAKE_REQUIRED_FLAGS "${_GEMINI_OLD_CMAKE_REQUIRED_FLAGS}")
  unset(_GEMINI_OLD_CMAKE_REQUIRED_FLAGS)
  if(NOT GEMINI_HAS_LIBFUZZER)
    message(FATAL_ERROR "ENABLE_FUZZING requires a Clang toolchain with the libFuzzer runtime")
  endif()

  function(configure_fuzzer target)
    target_compile_options(${target} PRIVATE
      -fsanitize=fuzzer,address,undefined
      -fno-omit-frame-pointer
      -g
    )
    target_link_options(${target} PRIVATE
      -fsanitize=fuzzer,address,undefined
    )
  endfunction()
endif()

add_library(redis_module_api INTERFACE)
target_include_directories(redis_module_api INTERFACE
  ${CMAKE_SOURCE_DIR}/include
)

add_subdirectory(modules/gemini-bloom)
add_subdirectory(modules/gemini-json)
add_subdirectory(modules/gemini-search)
