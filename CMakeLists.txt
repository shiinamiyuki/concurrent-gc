cmake_minimum_required(VERSION 3.26)
project(current-gc)

set(CMAKE_CXX_STANDARD 23)
# enable LTO
include(CheckIPOSupported)

add_subdirectory(mimalloc)

add_library(gc STATIC src/gc.h src/gc.cpp src/rc.h src/rc.cpp src/pmr-mimalloc.h)
target_link_libraries(gc PUBLIC mimalloc-static)
# if linux, link stdc++_libbacktrace
if (UNIX AND NOT APPLE)
    target_link_libraries(gc stdc++_libbacktrace)
    target_compile_options(gc PUBLIC -Wno-format)
endif()

set_property(TARGET gc PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(gc PUBLIC DEBUG)
endif()



add_executable(test src/test.cpp)
set_property(TARGET test PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
target_link_libraries(test gc)
add_executable(test_json src/test_json.cpp)
set_property(TARGET test_json PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
target_link_libraries(test_json gc)

add_executable(test_raytrace src/test_rt.cpp)
set_property(TARGET test_raytrace PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
target_link_libraries(test_raytrace gc)