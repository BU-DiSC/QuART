cmake_minimum_required(VERSION 3.10)
project(QuIT_runner CXX)

set(CMAKE_CXX_STANDARD 17)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build" FORCE)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O3")

# quick-insertion-tree is cloned into this directory by the run scripts.
# CMAKE_CURRENT_SOURCE_DIR is experiments/4.1-bods/, so
# #include "quick-insertion-tree/src/bptree/..." resolves correctly.

add_executable(quit_2k run_quit.cpp)
target_compile_definitions(quit_2k PRIVATE
    LOL_FAT VARIABLE_SPLIT REDISTRIBUTE LOL_RESET INMEMORY BLOCK_SIZE_BYTES=2048 QUIT_FAT)
target_include_directories(quit_2k PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(quit_4k run_quit.cpp)
target_compile_definitions(quit_4k PRIVATE
    LOL_FAT VARIABLE_SPLIT REDISTRIBUTE LOL_RESET INMEMORY BLOCK_SIZE_BYTES=4096 QUIT_FAT)
target_include_directories(quit_4k PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(bptree run_quit.cpp)
target_compile_definitions(bptree PRIVATE INMEMORY BLOCK_SIZE_BYTES=2048)
target_include_directories(bptree PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
