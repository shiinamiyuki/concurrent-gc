# UWaterloo CS842 Project: A Concurrent Garbage Collector for C++

## Build
A C++23 compiler is required to build the project. The project is built using CMake. To build the project, run the following commands:
```bash
mkdir build
cd build
cmake .. -GNinja
ninja
```

## Running Tests
```bash
build/test # some microbenchmarks
wget https://raw.githubusercontent.com/json-iterator/test-data/refs/heads/master/large-file.json
build/test_json # json parsing example
build/test_raytrace # raytracing example

```
