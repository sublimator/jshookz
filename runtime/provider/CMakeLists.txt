cmake_minimum_required(VERSION 3.15)
project(jshookz_provider C CXX)

set(JSHOOKZ_BINARYEN_VERSION "128")
find_program(JSHOOKZ_WASM_OPT wasm-opt)
if(NOT JSHOOKZ_WASM_OPT)
    message(FATAL_ERROR
        "jshookz provider requires Binaryen ${JSHOOKZ_BINARYEN_VERSION} "
        "(wasm-opt not found)")
endif()
execute_process(
    COMMAND "${JSHOOKZ_WASM_OPT}" --version
    OUTPUT_VARIABLE JSHOOKZ_WASM_OPT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE JSHOOKZ_WASM_OPT_RESULT
)
string(REGEX MATCH
    "^wasm-opt version ${JSHOOKZ_BINARYEN_VERSION}"
    JSHOOKZ_WASM_OPT_VERSION_MATCH
    "${JSHOOKZ_WASM_OPT_VERSION}"
)
if(NOT JSHOOKZ_WASM_OPT_RESULT EQUAL 0 OR
        NOT JSHOOKZ_WASM_OPT_VERSION_MATCH)
    message(FATAL_ERROR
        "jshookz provider requires Binaryen ${JSHOOKZ_BINARYEN_VERSION}; "
        "found '${JSHOOKZ_WASM_OPT_VERSION}' at ${JSHOOKZ_WASM_OPT}")
endif()

option(
    XAHAU_HOOK_PROVIDER
    "Build the runtime with only the Xahau Hook host-import surface"
    ON
)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 20)

# wasi-sdk 32 uses --target=wasm32-wasi but the compiler prefers wasm32-wasip1
add_compile_options(-Wno-deprecated)
add_link_options(-Wno-deprecated)

# QuickJS source files (C — no quickjs-libc.c, sandboxed)
set(QUICKJS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../engine/quickjs")
set(QUICKJS_SOURCES
    ${QUICKJS_SOURCE_DIR}/quickjs.c
    ${QUICKJS_SOURCE_DIR}/cutils.c
    ${QUICKJS_SOURCE_DIR}/dtoa.c
    ${QUICKJS_SOURCE_DIR}/libregexp.c
    ${QUICKJS_SOURCE_DIR}/libunicode.c
)

# Provider-owned C++ sources compiled into the WASM module.
include(provider_sources.cmake)

add_executable(jshookz_provider
    ${QUICKJS_SOURCES}
    ${JSHOOKZ_PROVIDER_CPP_SOURCES}
)

# Only C files need the quickjs parent include (for quickjs.h etc.)
# C++ files include quickjs.h via relative path to avoid VERSION file clash
# (QuickJS repo has a VERSION file that conflicts with C++ <version> header)
set_source_files_properties(${QUICKJS_SOURCES} PROPERTIES
    INCLUDE_DIRECTORIES "${QUICKJS_SOURCE_DIR}"
)

target_compile_definitions(jshookz_provider PRIVATE
    CONFIG_VERSION="2025-09-13"
    _GNU_SOURCE
    # Standalone WASI needs switch dispatch and no thread-backed Atomics, but
    # unlike Emscripten it supports the native stack-pointer check.
    CONFIG_WASM_STANDALONE
)

if(XAHAU_HOOK_PROVIDER)
    target_compile_definitions(jshookz_provider PRIVATE
        CONFIG_XAHAU_HOOK_PROVIDER
    )
    target_sources(jshookz_provider PRIVATE
        wasi_preview1_shims.c
    )
    target_link_options(jshookz_provider PRIVATE
        -Wl,--max-memory=33554432
    )
endif()

# C flags
set_source_files_properties(${QUICKJS_SOURCES} PROPERTIES COMPILE_OPTIONS
    "-O2;-fwrapv;-funsigned-char;-Wno-sign-compare;-Wno-missing-field-initializers;-Wno-unused-parameter;-mllvm;-wasm-enable-sjlj"
)

# C++ flags
set_source_files_properties(${JSHOOKZ_PROVIDER_CPP_SOURCES} PROPERTIES COMPILE_OPTIONS
    "-O2;-fno-exceptions;-fno-rtti;-Wno-c99-designator;-mllvm;-wasm-enable-sjlj"
)

# wasi-sdk reactor mode: no main(), exports _initialize
target_link_options(jshookz_provider PRIVATE
    # wasi-sdk otherwise runs wasm-opt only when it happens to be on PATH.
    # Disable that ambient behavior; the pinned post-link step below is the
    # canonical provider build.
    --no-wasm-opt
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
    -Wl,--export=qjs_compile_module
    -Wl,--export=qjs_get_bytecode_ptr
    -Wl,--export=qjs_get_bytecode_len
    -Wl,--export=qjs_eval_bytecode
    -Wl,--export=qjs_hook
    -Wl,--export=qjs_cbak
    -Wl,--export=qjs_get_result_ptr
    -Wl,--export=qjs_get_result_len
    -Wl,--export=qjs_destroy
    -Wl,--export=malloc
    -Wl,--export=free
    -Wl,--allow-undefined
    -Wl,-z,stack-size=131072
)

set_target_properties(jshookz_provider PROPERTIES
    SUFFIX ".wasm"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_custom_command(TARGET jshookz_provider POST_BUILD
    COMMAND "${JSHOOKZ_WASM_OPT}"
        "$<TARGET_FILE:jshookz_provider>"
        -O3
        # DWARF records checkout and build-environment details. The sealed
        # provider is byte-pinned, so retaining it makes otherwise equivalent
        # macOS and Linux builds produce different artifact identities.
        --strip-debug
        -o "$<TARGET_FILE:jshookz_provider>"
    COMMENT "Optimizing provider with Binaryen ${JSHOOKZ_BINARYEN_VERSION}"
    VERBATIM
)
