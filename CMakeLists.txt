cmake_minimum_required(VERSION 3.25)

project(vigil
  VERSION 0.1.0
  DESCRIPTION "Real-time multi-target tracker: sensor simulator, Kalman tracker, UDP link"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(VIGIL_BUILD_TESTS "Build unit and integration tests" ON)
option(VIGIL_BUILD_BENCHMARKS "Build Google Benchmark targets" ON)
option(VIGIL_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
set(VIGIL_SANITIZER "" CACHE STRING "Sanitizer to build with: '', 'address' (ASan+UBSan) or 'thread' (TSan)")
set_property(CACHE VIGIL_SANITIZER PROPERTY STRINGS "" address thread)

find_package(Eigen3 3.4 REQUIRED NO_MODULE)
find_package(Threads REQUIRED)

# Project-wide compile settings, attached to every target we build.
add_library(vigil_options INTERFACE)
target_compile_options(vigil_options INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wnon-virtual-dtor -Wold-style-cast)
if(VIGIL_WARNINGS_AS_ERRORS)
  target_compile_options(vigil_options INTERFACE -Werror)
endif()

if(VIGIL_SANITIZER STREQUAL "address")
  target_compile_options(vigil_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(vigil_options INTERFACE -fsanitize=address,undefined)
elseif(VIGIL_SANITIZER STREQUAL "thread")
  target_compile_options(vigil_options INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(vigil_options INTERFACE -fsanitize=thread)
elseif(NOT VIGIL_SANITIZER STREQUAL "")
  message(FATAL_ERROR "Unknown VIGIL_SANITIZER '${VIGIL_SANITIZER}'")
endif()

add_subdirectory(common)
add_subdirectory(sim)
add_subdirectory(tracker)

if(VIGIL_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(VIGIL_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()
