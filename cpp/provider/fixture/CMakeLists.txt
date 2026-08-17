cmake_minimum_required(VERSION 3.15)
project(jshookz_codec_fixture C CXX)

# Test-only embedding used to exercise QuickJS and the codec in one module.
# This is not a deployable Xahau Hook provider: it deliberately retains the
# generic fixture imports and WASI-backed native test host.

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# wasi-sdk 32 uses --target=wasm32-wasi but the compiler prefers wasm32-wasip1.
add_compile_options(-Wno-deprecated)
add_link_options(-Wno-deprecated)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
set(XDATA_DIR "${REPO_ROOT}/cpp/x-data")
set(XDATA_QJS_DIR "${REPO_ROOT}/cpp/x-data-quickjs")
set(QUICKJS_DIR "${REPO_ROOT}/cpp/quickjs")
set(BOOST_INCLUDE_DIR "" CACHE PATH "Path to boost include directory")

# Keeps generated/embedded_xahau_definitions.h in step with its JSON.
# Without this a definitions refresh silently does not reach the build.
include(${XDATA_DIR}/definitions.cmake)
include(${XDATA_DIR}/xdata_wasm_sources.cmake)
include(${XDATA_QJS_DIR}/xdata_qjs_sources.cmake)

set(QUICKJS_SOURCES
    ${QUICKJS_DIR}/quickjs.c
    ${QUICKJS_DIR}/cutils.c
    ${QUICKJS_DIR}/dtoa.c
    ${QUICKJS_DIR}/libregexp.c
    ${QUICKJS_DIR}/libunicode.c
)

include(${REPO_ROOT}/cpp/provider/provider_sources.cmake)

set(DIGEST_STUB_SOURCE ${XDATA_DIR}/stubs/digest_stub.cpp)

add_executable(jshookz_codec_fixture
    ${QUICKJS_SOURCES}
    ${JSHOOKZ_PROVIDER_CPP_SOURCES}
    ${DIGEST_STUB_SOURCE}
    ${JSHOOKZ_XDATA_WASM_SOURCES}
    ${JSHOOKZ_XDATA_QJS_SOURCES}
)

if(TARGET generate_xdata_definitions)
    add_dependencies(jshookz_codec_fixture generate_xdata_definitions)
endif()

target_compile_definitions(jshookz_codec_fixture PRIVATE
    CONFIG_VERSION="2025-09-13"
    _GNU_SOURCE
    EMSCRIPTEN
    CODEC_BACKEND_XDATA
    CONFIG_PROTOCOL_XDATA
    CATL_XDATA_NO_BOOST_JSON
)

# Only C files get the QuickJS repo include dir. C++ files include
# <jshookz/quickjs.h> so QuickJS's VERSION file cannot shadow <version>.
set_source_files_properties(${QUICKJS_SOURCES}
    PROPERTIES
    INCLUDE_DIRECTORIES "${QUICKJS_DIR}"
    COMPILE_OPTIONS "-O2;-fwrapv;-funsigned-char;-Wno-sign-compare;-Wno-missing-field-initializers;-Wno-unused-parameter;-mllvm;-wasm-enable-sjlj"
)

set(PROVIDER_CXX_SOURCES
    ${JSHOOKZ_PROVIDER_CPP_SOURCES}
    ${DIGEST_STUB_SOURCE}
    ${JSHOOKZ_XDATA_WASM_SOURCES}
    ${JSHOOKZ_XDATA_QJS_SOURCES}
)

set_source_files_properties(${PROVIDER_CXX_SOURCES}
    PROPERTIES
    COMPILE_OPTIONS "-O2;-fwrapv;-fno-exceptions;-Wno-c99-designator;-mllvm;-wasm-enable-sjlj"
)

target_include_directories(jshookz_codec_fixture PRIVATE
    ${XDATA_DIR}/stubs
    ${JSHOOKZ_XDATA_QJS_INCLUDE_DIR}
    ${JSHOOKZ_QJS_CPP_INCLUDE_DIR}
    ${JSHOOKZ_XDATA_INCLUDE_DIRS}
)

target_link_options(jshookz_codec_fixture PRIVATE
    -mexec-model=reactor
    -Wl,--export=wizer.initialize
    -Wl,--export=qjs_init
    -Wl,--export=qjs_set_seed
    -Wl,--export=qjs_set_memory_limit
    -Wl,--export=qjs_set_max_stack_size
    -Wl,--export=qjs_enable_coverage
    -Wl,--export=qjs_eval
    -Wl,--export=qjs_eval_module
    -Wl,--export=qjs_compile
    -Wl,--export=qjs_get_bytecode_ptr
    -Wl,--export=qjs_get_bytecode_len
    -Wl,--export=qjs_eval_bytecode
    -Wl,--export=qjs_get_result_ptr
    -Wl,--export=qjs_get_result_len
    -Wl,--export=qjs_destroy
    -Wl,--export=malloc
    -Wl,--export=free
    -Wl,--allow-undefined
    -Wl,-z,stack-size=131072
)

set_target_properties(jshookz_codec_fixture PROPERTIES
    SUFFIX ".wasm"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
