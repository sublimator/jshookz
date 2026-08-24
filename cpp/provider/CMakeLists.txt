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
option(
    JSHOOKZ_RESOURCE_PROBE
    "Expose non-product QuickJS heap accounting diagnostics"
    OFF
)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# wasi-sdk 32 uses --target=wasm32-wasi but the compiler prefers wasm32-wasip1
add_compile_options(-Wno-deprecated)
add_link_options(-Wno-deprecated)

# Provider-owned C++ sources compiled into the WASM module.
include(provider_sources.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../x-data/definitions.cmake)

add_executable(jshookz_provider
    ${JSHOOKZ_QUICKJS_C_SOURCES}
    ${JSHOOKZ_PROVIDER_CPP_SOURCES}
)
add_dependencies(jshookz_provider generate_xdata_definitions)

target_include_directories(jshookz_provider PRIVATE
    ${JSHOOKZ_QJS_CPP_INCLUDE_DIR}
    ${JSHOOKZ_QUICKJS_SOURCE_DIR}
    ${JSHOOKZ_XDATA_INCLUDE_DIRS}
    ${JSHOOKZ_XAHAU_TYPES_INCLUDE_DIR}
)

target_compile_definitions(jshookz_provider PRIVATE
    CONFIG_VERSION="${JSHOOKZ_QJS_CONFIG_VERSION}"
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
        # The accepted provider ABI starts at exactly seven 64-KiB pages. Make
        # that an input to initialization rather than relying on allocator
        # growth granularity to happen to land on the profile value.
        -Wl,--initial-memory=458752
        -Wl,--max-memory=33554432
    )
endif()

if(JSHOOKZ_RESOURCE_PROBE)
    target_compile_definitions(jshookz_provider PRIVATE
        CONFIG_WASM_RESOURCE_PROBE
    )
    target_link_options(jshookz_provider PRIVATE
        -Wl,--export=qjs_resource_reset_peak
        -Wl,--export=qjs_resource_current_size
        -Wl,--export=qjs_resource_current_count
        -Wl,--export=qjs_resource_peak_size
        -Wl,--export=qjs_resource_peak_count
        -Wl,--export=qjs_resource_static_protocol_bytes
    )
endif()

# C flags
set_source_files_properties(${JSHOOKZ_QUICKJS_C_SOURCES} PROPERTIES COMPILE_OPTIONS
    "-O2;-fwrapv;-funsigned-char;-Wno-sign-compare;-Wno-missing-field-initializers;-Wno-unused-parameter;-mllvm;-wasm-enable-sjlj"
)
# C++ flags
set_source_files_properties(${JSHOOKZ_PROVIDER_CPP_SOURCES} PROPERTIES COMPILE_OPTIONS
    "-O2;-fno-exceptions;-fno-rtti;-Werror=switch;-Wno-c99-designator;-mllvm;-wasm-enable-sjlj"
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
    OUTPUT_NAME "jshookz_provider.unwizered"
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

# A separate, non-product WASM fixture exercises the exact standalone
# allocator implementation.  Native tests use injected allocators and cannot
# cover the provider's requested-size tail metadata or wasi-libc realloc path.
add_executable(quickjs_wasm_allocator_probe EXCLUDE_FROM_ALL
    ${JSHOOKZ_QUICKJS_C_SOURCES}
    ${JSHOOKZ_QUICKJS_SOURCE_DIR}/tests/wasm_allocator_probe.c
    wasi_preview1_shims.c
)
target_include_directories(quickjs_wasm_allocator_probe PRIVATE
    ${JSHOOKZ_QUICKJS_SOURCE_DIR}
)
target_compile_definitions(quickjs_wasm_allocator_probe PRIVATE
    CONFIG_VERSION="${JSHOOKZ_QJS_CONFIG_VERSION}"
    _GNU_SOURCE
    CONFIG_WASM_STANDALONE
    CONFIG_WASM_ALLOCATOR_PROBE
)
target_link_options(quickjs_wasm_allocator_probe PRIVATE
    --no-wasm-opt
    -mexec-model=reactor
    -Wl,--initial-memory=393216
    -Wl,--max-memory=33554432
    -Wl,--export=quickjs_wasm_allocator_probe
    -Wl,-z,stack-size=131072
)
set_target_properties(quickjs_wasm_allocator_probe PROPERTIES
    SUFFIX ".wasm"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
