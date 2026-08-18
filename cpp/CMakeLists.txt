cmake_minimum_required(VERSION 3.15)
project(jshookz_cpp CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Host C++ only. The sealed provider stays on cpp/provider/CMakeLists.txt
# with the wasi-sdk toolchain; do not add_subdirectory it here.
include(CTest)
add_subdirectory(quickjs-cpp)
