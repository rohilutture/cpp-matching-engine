cmake_minimum_required(VERSION 3.20)
project(matching_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# -DSANITIZE=address|thread|undefined  (tests only; do not benchmark sanitized builds)
set(SANITIZE "" CACHE STRING "sanitizer to enable")
add_library(me_flags INTERFACE)
target_compile_options(me_flags INTERFACE -Wall -Wextra -fno-omit-frame-pointer)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  target_compile_options(me_flags INTERFACE -O3 -march=native)
endif()
if(SANITIZE)
  target_compile_options(me_flags INTERFACE -fsanitize=${SANITIZE} -g -O1)
  target_link_options(me_flags INTERFACE -fsanitize=${SANITIZE})
endif()

include_directories(include)
find_package(Threads REQUIRED)

add_executable(me_tests tests/test_main.cpp tests/test_structs.cpp tests/test_book.cpp tests/test_diff.cpp)
target_link_libraries(me_tests PRIVATE me_flags Threads::Threads)

add_executable(bench_book bench/bench_book.cpp)
target_link_libraries(bench_book PRIVATE me_flags Threads::Threads)

add_executable(bench_pipeline bench/bench_pipeline.cpp)
target_link_libraries(bench_pipeline PRIVATE me_flags Threads::Threads)

add_executable(bench_spsc bench/bench_spsc.cpp)
target_link_libraries(bench_spsc PRIVATE me_flags Threads::Threads)
