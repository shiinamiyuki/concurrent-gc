cmake_minimum_required(VERSION 3.26)
set(CMAKE_CXX_STANDARD 23)

add_library(gc STATIC src/gc.h src/gc.cpp src/heap.h src/heap.cpp)
if (CMAKE_BUILD_TYPE STREQUAL "Debug" OR  CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    target_compile_definitions(gc PUBLIC DEBUG)
endif()
add_executable(test src/test.cpp)
target_link_libraries(test gc)