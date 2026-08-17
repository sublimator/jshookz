cmake_minimum_required(VERSION 3.15)
project(jshookz_xrpl_codec C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(QUICKJS_DIR "${REPO_ROOT}/cpp/quickjs")
set(XDATA_DIR "${REPO_ROOT}/cpp/x-data")
set(XDATA_QJS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(QJS_CPP_DIR "${REPO_ROOT}/cpp/quickjs-cpp")
set(BOOST_INCLUDE_DIR "" CACHE PATH "Path to Boost include directory")

# Keep the embedded Xahau definitions in step with their JSON source.
include(${XDATA_DIR}/definitions.cmake)
include(${XDATA_DIR}/xdata_wasm_sources.cmake)
include(${XDATA_QJS_DIR}/xdata_qjs_sources.cmake)
include(${QJS_CPP_DIR}/quickjs_cpp_sources.cmake)

set(QUICKJS_SOURCES
    ${QUICKJS_DIR}/quickjs.c
    ${QUICKJS_DIR}/cutils.c
    ${QUICKJS_DIR}/dtoa.c
    ${QUICKJS_DIR}/libregexp.c
    ${QUICKJS_DIR}/libunicode.c
)

set(CODEC_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/standalone_wasm_bindings.c
    ${XDATA_DIR}/stubs/digest_stub.cpp
    ${JSHOOKZ_XDATA_QJS_SOURCES}
    ${JSHOOKZ_QJS_CPP_SOURCES}
)

add_executable(jshookz_xrpl_codec
    ${QUICKJS_SOURCES}
    ${JSHOOKZ_XDATA_WASM_SOURCES}
    ${CODEC_SOURCES}
)

if(TARGET generate_xdata_definitions)
    add_dependencies(jshookz_xrpl_codec generate_xdata_definitions)
endif()

target_compile_definitions(jshookz_xrpl_codec PRIVATE
    CONFIG_VERSION="2025-09-13"
    _GNU_SOURCE
    EMSCRIPTEN
    CODEC_BACKEND_XDATA
    CATL_XDATA_NO_BOOST_JSON
)

set_source_files_properties(
    ${QUICKJS_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/standalone_wasm_bindings.c
    PROPERTIES
    COMPILE_FLAGS "-O2 -fwrapv -funsigned-char -Wno-sign-compare -Wno-missing-field-initializers -Wno-unused-parameter -mllvm -wasm-enable-sjlj"
    INCLUDE_DIRECTORIES "${QUICKJS_DIR}"
)

set_source_files_properties(
    ${JSHOOKZ_XDATA_WASM_SOURCES}
    ${XDATA_DIR}/stubs/digest_stub.cpp
    ${JSHOOKZ_XDATA_QJS_SOURCES}
    ${JSHOOKZ_QJS_CPP_SOURCES}
    PROPERTIES
    COMPILE_FLAGS "-O2 -Wno-c99-designator -mllvm -wasm-enable-sjlj"
)

target_include_directories(jshookz_xrpl_codec PRIVATE
    ${XDATA_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${JSHOOKZ_XDATA_QJS_INCLUDE_DIR}
    ${JSHOOKZ_QJS_CPP_INCLUDE_DIR}
    ${JSHOOKZ_XDATA_INCLUDE_DIRS}
)

target_compile_options(jshookz_xrpl_codec PRIVATE -Wno-deprecated)
target_link_options(jshookz_xrpl_codec PRIVATE
    -Wno-deprecated
    -mexec-model=reactor
    -Wl,--export=wizer.initialize
    -Wl,--export=qjs_init
    -Wl,--export=qjs_eval
    -Wl,--export=qjs_eval_bytecode
    -Wl,--export=qjs_destroy
    -Wl,--export=qjs_get_result_ptr
    -Wl,--export=qjs_get_result_len
    -Wl,--export=malloc
    -Wl,--export=free
    -Wl,--allow-undefined
    -Wl,-z,stack-size=131072
)

set_target_properties(jshookz_xrpl_codec PROPERTIES
    SUFFIX ".wasm"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
