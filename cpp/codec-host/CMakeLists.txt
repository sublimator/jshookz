cmake_minimum_required(VERSION 3.15)
project(jshookz_xrpl_codec_host CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(wasmtime CONFIG QUIET)

# Conan supplies wasmtime::wasmtime for Xahau development builds. The
# official Wasmtime C-API archive intentionally ships only include/ and lib/,
# so the standalone fixture also accepts that layout directly.
if(NOT TARGET wasmtime::wasmtime)
    set(WASMTIME_ROOT "" CACHE PATH "Path to an official Wasmtime C-API archive")
    find_path(WASMTIME_INCLUDE_DIR
        NAMES wasmtime.h
        HINTS "${WASMTIME_ROOT}/include" "$ENV{WASMTIME_HOME}/include"
        NO_DEFAULT_PATH
        REQUIRED
    )
    find_library(WASMTIME_LIBRARY
        NAMES wasmtime
        HINTS "${WASMTIME_ROOT}/lib" "$ENV{WASMTIME_HOME}/lib"
        NO_DEFAULT_PATH
        REQUIRED
    )
    add_library(wasmtime::wasmtime STATIC IMPORTED)
    set_target_properties(wasmtime::wasmtime PROPERTIES
        IMPORTED_LOCATION "${WASMTIME_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${WASMTIME_INCLUDE_DIR}"
    )
    if(UNIX AND NOT APPLE)
        set_property(TARGET wasmtime::wasmtime PROPERTY
            INTERFACE_LINK_LIBRARIES "pthread;dl;m;rt")
    endif()
endif()

if(NOT APPLE)
    find_package(OpenSSL REQUIRED COMPONENTS Crypto)
endif()

add_executable(jshookz_codec_host main.cpp)
target_link_libraries(jshookz_codec_host PRIVATE wasmtime::wasmtime)
if(WASMTIME_ROOT)
    set_property(TARGET jshookz_codec_host APPEND PROPERTY
        BUILD_RPATH "${WASMTIME_ROOT}/lib")
endif()

add_executable(jshookz_codec_bench bench.cpp)
target_link_libraries(jshookz_codec_bench PRIVATE wasmtime::wasmtime)

if(NOT APPLE)
    target_link_libraries(jshookz_codec_host PRIVATE OpenSSL::Crypto)
    target_link_libraries(jshookz_codec_bench PRIVATE OpenSSL::Crypto)
endif()
