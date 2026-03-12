cmake_minimum_required(VERSION 3.16)
project(AmpStageStudio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(tube_stage_test
  tools/tube_stage_test.cpp
)

target_include_directories(tube_stage_test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(tube_stage_test PRIVATE
    -O3
    -Wall
    -Wextra
    -Wpedantic
  )
endif()
