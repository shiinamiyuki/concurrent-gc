cmake_minimum_required(VERSION 3.26)
set(CMAKE_CXX_STANDARD 23)

add_library(gc STATIC gc.h gc.cpp)
add_executable(test test.cpp)
target_link_libraries(test gc)