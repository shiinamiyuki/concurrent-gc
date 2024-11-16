cmake_minimum_required(VERSION 3.26)
set(CMAKE_CXX_STANDARD 23)

add_library(gc STATIC gc.h gc.cpp heap.h heap.cpp)
if (CMAKE_BUILD_TYPE STREQUAL "Debug" OR  CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    target_compile_definitions(gc PUBLIC DEBUG)
endif()
add_executable(test test.cpp)
target_link_libraries(test gc)