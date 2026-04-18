cmake_minimum_required(VERSION 3.14)

project(dustman
  VERSION 0.0.1
  DESCRIPTION "A precise garbage collector library for C++"
  LANGUAGES CXX
)

# -----------------------------------------------------------------------------
# Project configuration
# -----------------------------------------------------------------------------

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(DUSTMAN_IS_TOP_LEVEL ON)
else()
  set(DUSTMAN_IS_TOP_LEVEL OFF)
endif()

option(DUSTMAN_BUILD_TESTS      "Build dustman tests"      ${DUSTMAN_IS_TOP_LEVEL})
option(DUSTMAN_BUILD_BENCHMARKS "Build dustman benchmarks" ${DUSTMAN_IS_TOP_LEVEL})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# -----------------------------------------------------------------------------
# The dustman library
# -----------------------------------------------------------------------------

add_library(dustman)
add_library(dustman::dustman ALIAS dustman)

target_sources(dustman PRIVATE
  src/placeholder.cpp
)

target_include_directories(dustman
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(dustman PUBLIC cxx_std_17)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(dustman PRIVATE
    -Wall -Wextra -Wpedantic
    -Wshadow -Wnon-virtual-dtor -Wold-style-cast
    -Wconversion
  )
elseif(MSVC)
  target_compile_options(dustman PRIVATE /W4)
endif()

# -----------------------------------------------------------------------------
# Tests and benchmarks (conditional)
# -----------------------------------------------------------------------------

if(DUSTMAN_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(DUSTMAN_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
