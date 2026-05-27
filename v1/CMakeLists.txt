# CMake minimum version
cmake_minimum_required(VERSION 3.10)

# Project name
project(MemoryPoolProject)

# Set C++ standard
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Include directories
include_directories(include)

# Source files
file(GLOB SRC_FILES "src/*.cpp")

# Add the executable for the main target
add_executable(${PROJECT_NAME} ${SRC_FILES} tests/UnitTest.cpp)
add_executable(v1_benchmark ${SRC_FILES} tests/BenchmarkTest.cpp)

# Add compile options
target_compile_options(${PROJECT_NAME} PRIVATE -g -pthread)
target_compile_options(v1_benchmark PRIVATE -g -pthread)

# Link libraries
target_link_libraries(${PROJECT_NAME} pthread)
target_link_libraries(v1_benchmark pthread)

# Link libraries (if any)
# target_link_libraries(${PROJECT_NAME} <library_name>)
