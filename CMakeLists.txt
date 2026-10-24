cmake_minimum_required(VERSION 3.14)
project(minic LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(minic src/main.cpp)
target_compile_options(minic PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
add_test(NAME minic_tests
         COMMAND ${CMAKE_SOURCE_DIR}/tests/run_tests.sh $<TARGET_FILE:minic>)
