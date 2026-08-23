include(${CMAKE_CURRENT_LIST_DIR}/xdata_wasm_sources.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/definitions.cmake)

add_library(jshookz_xdata STATIC
    ${JSHOOKZ_XDATA_WASM_SOURCES}
    ${CMAKE_CURRENT_LIST_DIR}/stubs/digest_stub.cpp
)
target_include_directories(jshookz_xdata PUBLIC
    ${JSHOOKZ_XDATA_INCLUDE_DIRS}
    ${CMAKE_CURRENT_LIST_DIR}/stubs
)
target_compile_definitions(jshookz_xdata PUBLIC
    CATL_XDATA_NO_BOOST_JSON
    CATL_XDATA_NO_THROWING_CURSOR
    CODEC_BACKEND_XDATA
)
target_compile_features(jshookz_xdata PUBLIC cxx_std_23)
add_dependencies(jshookz_xdata generate_xdata_definitions)

find_path(Boost_INCLUDE_DIR
    boost/json.hpp
    HINTS ENV BOOST_INCLUDE_DIR
    PATHS /opt/homebrew/include /usr/include
)
# Separate TUs, no CATL_XDATA_NO_BOOST_JSON. Do not link this with
# jshookz_xdata — same symbols, different class layout.
if(Boost_INCLUDE_DIR)
    add_library(jshookz_xdata_json STATIC
        ${JSHOOKZ_XDATA_PROVIDER_STATIC_SOURCES}
        ${CMAKE_CURRENT_LIST_DIR}/src/protocol.cpp
        ${CMAKE_CURRENT_LIST_DIR}/src/embedded_protocol.cpp
        ${CMAKE_CURRENT_LIST_DIR}/src/protocol_json.cpp
        ${CMAKE_CURRENT_LIST_DIR}/boost_json_src.cpp
        ${CMAKE_CURRENT_LIST_DIR}/base58/src/base58.cpp
        ${CMAKE_CURRENT_LIST_DIR}/stubs/digest_stub.cpp
    )
    target_include_directories(jshookz_xdata_json PUBLIC
        ${JSHOOKZ_XDATA_INCLUDE_DIRS}
        ${CMAKE_CURRENT_LIST_DIR}/stubs
        ${Boost_INCLUDE_DIR}
    )
    target_compile_features(jshookz_xdata_json PUBLIC cxx_std_23)
    add_dependencies(jshookz_xdata_json generate_xdata_definitions)
endif()

if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    include(GoogleTest)

    add_executable(test_parse_helpers tests/test_parse_helpers.cpp)
    target_link_libraries(test_parse_helpers PRIVATE jshookz_xdata GTest::gtest_main)
    gtest_discover_tests(test_parse_helpers)

    # Amount checkpoint: not gated on Boost JSON or optional Python discovery.
    add_executable(test_amount_view tests/test_amount_view.cpp)
    target_link_libraries(test_amount_view PRIVATE jshookz_xdata GTest::gtest_main)
    target_compile_definitions(test_amount_view PRIVATE
        CATL_XDATA_TEST_PARTS_HOOK)
    gtest_discover_tests(test_amount_view DISCOVERY_TIMEOUT 30 DISCOVERY_MODE PRE_TEST)

    add_executable(test_uint32_view tests/test_uint32_view.cpp)
    target_link_libraries(test_uint32_view PRIVATE jshookz_xdata GTest::gtest_main)
    gtest_discover_tests(test_uint32_view DISCOVERY_TIMEOUT 30 DISCOVERY_MODE PRE_TEST)

    add_executable(test_account_id_view tests/test_account_id_view.cpp)
    target_link_libraries(test_account_id_view PRIVATE jshookz_xdata GTest::gtest_main)
    target_compile_definitions(test_account_id_view PRIVATE
        CATL_XDATA_TEST_ACCOUNT_BASE58_HOOK)
    gtest_discover_tests(test_account_id_view DISCOVERY_TIMEOUT 30 DISCOVERY_MODE PRE_TEST)

    add_executable(test_pathset_view tests/test_pathset_view.cpp)
    target_link_libraries(test_pathset_view PRIVATE jshookz_xdata GTest::gtest_main)
    target_compile_definitions(test_pathset_view PRIVATE
        CATL_XDATA_TEST_PATHSET_HOOK)
    gtest_discover_tests(test_pathset_view DISCOVERY_TIMEOUT 30 DISCOVERY_MODE PRE_TEST)

    add_executable(test_pathset_noexcept tests/test_pathset_noexcept.cpp)
    target_link_libraries(test_pathset_noexcept PRIVATE jshookz_xdata)
    target_compile_options(test_pathset_noexcept PRIVATE
        -fno-exceptions -fno-rtti)
    add_test(NAME test_pathset_noexcept COMMAND test_pathset_noexcept)

    add_executable(test_static_protocol_noexcept
        tests/test_static_protocol_noexcept.cpp
        ${JSHOOKZ_XDATA_PROVIDER_STATIC_SOURCES}
    )
    target_include_directories(test_static_protocol_noexcept PRIVATE
        ${JSHOOKZ_XDATA_INCLUDE_DIRS}
    )
    target_compile_options(test_static_protocol_noexcept PRIVATE
        -fno-exceptions -fno-rtti)
    add_dependencies(test_static_protocol_noexcept generate_xdata_definitions)
    add_test(NAME test_static_protocol_noexcept
        COMMAND test_static_protocol_noexcept)
    add_test(NAME provider_static_source_policy
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_LIST_DIR}/tests/check_provider_static_policy.py
            --inventory ${CMAKE_CURRENT_LIST_DIR}/xdata_wasm_sources.cmake
            --binary $<TARGET_FILE:test_static_protocol_noexcept>
            --nm ${CMAKE_NM})

    add_executable(test_provider_static_startup_alloc
        tests/provider_static_startup_alloc_probe.cpp
        ${JSHOOKZ_XDATA_PROVIDER_STATIC_SOURCES}
    )
    target_include_directories(test_provider_static_startup_alloc PRIVATE
        ${JSHOOKZ_XDATA_INCLUDE_DIRS}
    )
    target_compile_options(test_provider_static_startup_alloc PRIVATE
        -fno-exceptions -fno-rtti)
    add_dependencies(test_provider_static_startup_alloc
        generate_xdata_definitions)
    add_test(NAME provider_static_startup_no_cpp_allocation
        COMMAND test_provider_static_startup_alloc)

    function(_xdata_provider_static_poison name definition expected)
        add_executable(provider_static_poison_${name}
            tests/provider_static_compiled_poison.cpp
            ${JSHOOKZ_XDATA_PROVIDER_STATIC_SOURCES}
            ${ARGN}
        )
        target_include_directories(provider_static_poison_${name} PRIVATE
            ${JSHOOKZ_XDATA_INCLUDE_DIRS}
        )
        target_compile_definitions(provider_static_poison_${name} PRIVATE
            CATL_XDATA_NO_BOOST_JSON
            CATL_XDATA_NO_THROWING_CURSOR
            ${definition}
        )
        target_compile_options(provider_static_poison_${name} PRIVATE
            -fno-exceptions -fno-rtti -fno-builtin -O0)
        add_dependencies(provider_static_poison_${name}
            generate_xdata_definitions)
        add_test(NAME provider_static_compiled_poison_${name}
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_LIST_DIR}/tests/run_provider_static_compiled_poison.py
                --python ${Python3_EXECUTABLE}
                --checker ${CMAKE_CURRENT_LIST_DIR}/tests/check_provider_static_policy.py
                --inventory ${CMAKE_CURRENT_LIST_DIR}/xdata_wasm_sources.cmake
                --binary $<TARGET_FILE:provider_static_poison_${name}>
                --nm ${CMAKE_NM}
                --expect "${expected}")
    endfunction()

    _xdata_provider_static_poison(
        dynamic_protocol JSHOOKZ_POISON_DYNAMIC_PROTOCOL "dynamic Protocol"
        ${CMAKE_CURRENT_LIST_DIR}/src/protocol.cpp)
    _xdata_provider_static_poison(
        field_types JSHOOKZ_POISON_FIELD_TYPES "allocating FieldTypes")
    _xdata_provider_static_poison(
        ordinary_new JSHOOKZ_POISON_NEW "operator new")
    _xdata_provider_static_poison(
        aligned_new JSHOOKZ_POISON_ALIGNED_NEW "operator new")
    _xdata_provider_static_poison(
        vector JSHOOKZ_POISON_VECTOR "STL vector ownership")
    _xdata_provider_static_poison(
        string JSHOOKZ_POISON_STRING "STL string ownership")
    _xdata_provider_static_poison(
        bad_alloc JSHOOKZ_POISON_BAD_ALLOC "bad_alloc route")
    _xdata_provider_static_poison(
        pre_main_malloc JSHOOKZ_POISON_MALLOC "C allocation")

    add_executable(test_pathset_quickjs
        tests/pathset_quickjs_fixture.cpp
        tests/test_pathset_quickjs.cpp
    )
    target_link_libraries(test_pathset_quickjs PRIVATE
        jshookz_xdata jshookz_qjs_src GTest::gtest_main
    )
    target_compile_options(test_pathset_quickjs PRIVATE
        -fno-exceptions -fno-rtti)
    gtest_discover_tests(test_pathset_quickjs
        DISCOVERY_TIMEOUT 30 DISCOVERY_MODE PRE_TEST)

    add_executable(pathset_gc_probe
        tests/pathset_quickjs_fixture.cpp
        tests/pathset_gc_probe.cpp
    )
    target_link_libraries(pathset_gc_probe PRIVATE
        jshookz_xdata jshookz_qjs_src
    )
    target_compile_options(pathset_gc_probe PRIVATE
        -fno-exceptions -fno-rtti)

    add_executable(test_view_sizes tests/view_sizes_probe.cpp)
    target_link_libraries(test_view_sizes PRIVATE jshookz_xdata)
    add_test(NAME test_view_sizes COMMAND test_view_sizes)

    set(_wasi_sdk "$ENV{WASI_SDK_PATH}")
    if(NOT _wasi_sdk)
        set(_wasi_sdk "$ENV{HOME}/.local/share/mise/installs/wasi-sdk/32/wasi-sdk")
    endif()
    if(EXISTS "${_wasi_sdk}/bin/clang++")
        add_test(NAME view_sizes_wasm32
            COMMAND "${_wasi_sdk}/bin/clang++"
                --sysroot "${_wasi_sdk}/share/wasi-sysroot"
                -std=c++23 -c -fno-exceptions
                -DCATL_XDATA_NO_BOOST_JSON
                -I${CMAKE_CURRENT_LIST_DIR}/includes
                -I${CMAKE_CURRENT_LIST_DIR}/core/includes
                -I${CMAKE_CURRENT_LIST_DIR}/base58/includes
                -I${CMAKE_CURRENT_LIST_DIR}/generated
                -I${CMAKE_CURRENT_LIST_DIR}/stubs
                ${CMAKE_CURRENT_LIST_DIR}/tests/view_sizes_probe.cpp
                -o ${CMAKE_CURRENT_BINARY_DIR}/view_sizes_wasm32.o
        )
    endif()

    find_package(Python3 COMPONENTS Interpreter)
    set(_fuel_python "")
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../python/jshookz/.venv/bin/python")
        set(_fuel_python
            "${CMAKE_CURRENT_LIST_DIR}/../../python/jshookz/.venv/bin/python")
    elseif(Python3_Interpreter_FOUND)
        set(_fuel_python ${Python3_EXECUTABLE})
    else()
        set(_fuel_python python3)
    endif()

    add_test(NAME pathset_gc_red_controls
        COMMAND ${_fuel_python}
            ${CMAKE_CURRENT_LIST_DIR}/tests/run_pathset_gc_red.py
            --probe $<TARGET_FILE:pathset_gc_probe>
    )
    set_tests_properties(pathset_gc_red_controls PROPERTIES TIMEOUT 30)

    add_test(NAME amount_fuel_lanes
        COMMAND ${_fuel_python}
            ${CMAKE_CURRENT_LIST_DIR}/tests/run_amount_fuel.py
            --src ${CMAKE_CURRENT_LIST_DIR}
    )
    set_tests_properties(amount_fuel_lanes PROPERTIES TIMEOUT 240)

    add_test(NAME uint32_fuel_lanes
        COMMAND ${_fuel_python}
            ${CMAKE_CURRENT_LIST_DIR}/tests/run_uint32_fuel.py
            --src ${CMAKE_CURRENT_LIST_DIR}
    )
    set_tests_properties(uint32_fuel_lanes PROPERTIES TIMEOUT 240)

    add_test(NAME account_id_fuel_lanes
        COMMAND ${_fuel_python}
            ${CMAKE_CURRENT_LIST_DIR}/tests/run_account_id_fuel.py
            --src ${CMAKE_CURRENT_LIST_DIR}
    )
    set_tests_properties(account_id_fuel_lanes PROPERTIES TIMEOUT 240)

    add_test(NAME pathset_fuel_lanes
        COMMAND ${_fuel_python}
            ${CMAKE_CURRENT_LIST_DIR}/tests/run_pathset_fuel.py
            --src ${CMAKE_CURRENT_LIST_DIR}
    )
    set_tests_properties(pathset_fuel_lanes PROPERTIES TIMEOUT 240)

    if(Boost_INCLUDE_DIR)
        # Also the only target that compiles the codec headers at all: the
        # wasm library builds them out via CATL_XDATA_NO_BOOST_JSON, and no
        # other TU here includes codecs.h.
        add_executable(test_codecs tests/test_codecs.cpp)
        target_link_libraries(test_codecs PRIVATE
            jshookz_xdata_json GTest::gtest_main
        )
        gtest_discover_tests(test_codecs)

        add_executable(test_scan tests/test_scan.cpp)
        target_link_libraries(test_scan PRIVATE
            jshookz_xdata_json GTest::gtest_main
        )
        target_include_directories(test_scan PRIVATE ${CMAKE_CURRENT_LIST_DIR}/tests)
        target_compile_definitions(test_scan PRIVATE
            JSHOOKZ_ORACLE_CORPUS_JSON="${CMAKE_CURRENT_LIST_DIR}/tests/oracle_corpus.json"
            CATL_XDATA_TEST_PATHSET_HOOK
        )
        gtest_discover_tests(test_scan DISCOVERY_TIMEOUT 30)

        add_executable(oracle_corpus_runner tests/oracle_corpus_runner.cpp)
        target_link_libraries(oracle_corpus_runner PRIVATE jshookz_xdata_json)
        target_include_directories(oracle_corpus_runner PRIVATE
            ${CMAKE_CURRENT_LIST_DIR}/tests
        )
        target_compile_definitions(oracle_corpus_runner PRIVATE
            JSHOOKZ_ORACLE_CORPUS_JSON="${CMAKE_CURRENT_LIST_DIR}/tests/oracle_corpus.json"
        )
        add_test(NAME oracle_corpus_runner COMMAND oracle_corpus_runner)

        add_executable(test_scan_noexcept tests/test_scan_noexcept.cpp)
        target_link_libraries(test_scan_noexcept PRIVATE jshookz_xdata)
        target_compile_options(test_scan_noexcept PRIVATE -fno-exceptions)
        add_test(NAME test_scan_noexcept COMMAND test_scan_noexcept)

        add_executable(test_scan_alloc tests/test_scan_alloc.cpp)
        target_link_libraries(test_scan_alloc PRIVATE jshookz_xdata GTest::gtest_main)
        gtest_discover_tests(test_scan_alloc DISCOVERY_TIMEOUT 30)

        add_executable(test_parser_context tests/test_parser_context.cpp)
        target_link_libraries(test_parser_context PRIVATE jshookz_xdata GTest::gtest_main)
        gtest_discover_tests(test_parser_context DISCOVERY_TIMEOUT 30)

        add_executable(test_scan_recover tests/scan_recover_wasm.cpp)
        target_link_libraries(test_scan_recover PRIVATE jshookz_xdata)
        add_test(NAME test_scan_recover
            COMMAND ${CMAKE_COMMAND}
                -DCMD=$<TARGET_FILE:test_scan_recover>
                -P ${CMAKE_CURRENT_LIST_DIR}/tests/check_scan_recover.cmake
        )

        set(_poison_sysroot "${CMAKE_OSX_SYSROOT}")
        if(_poison_sysroot AND NOT EXISTS "${_poison_sysroot}")
            execute_process(
                COMMAND xcrun --sdk ${_poison_sysroot} --show-sdk-path
                OUTPUT_VARIABLE _poison_sysroot
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
            )
        endif()
        add_test(NAME throwing_cursor_poison
            COMMAND ${CMAKE_COMMAND}
                -DCOMPILER=${CMAKE_CXX_COMPILER}
                "-DFLAGS=${CMAKE_CXX_FLAGS}"
                "-DSYSROOT=${_poison_sysroot}"
                -DPROBE=${CMAKE_CURRENT_LIST_DIR}/tests/throwing_cursor_poison_probe.cpp
                -DINC0=${CMAKE_CURRENT_LIST_DIR}/includes
                -DINC1=${CMAKE_CURRENT_LIST_DIR}/core/includes
                -DOUT=${CMAKE_CURRENT_BINARY_DIR}/throwing_cursor_poison_probe.o
                -P ${CMAKE_CURRENT_LIST_DIR}/tests/check_throwing_cursor_poison.cmake
        )

        if(Python3_Interpreter_FOUND)
            set(_cleaned_dir ${CMAKE_CURRENT_BINARY_DIR}/cleaned)
            set(_xahau_cleaned ${_cleaned_dir}/xahau.cleaned.json)
            set(_xrpl_cleaned ${_cleaned_dir}/xrpl.cleaned.json)
            set(_gen ${CMAKE_CURRENT_LIST_DIR}/scripts/generate_definitions.py)
            add_custom_command(
                OUTPUT ${_xahau_cleaned} ${_xrpl_cleaned}
                COMMAND ${CMAKE_COMMAND} -E make_directory ${_cleaned_dir}
                COMMAND ${Python3_EXECUTABLE} ${_gen}
                    --input ${CMAKE_CURRENT_LIST_DIR}/definitions/xahau_definitions.json
                    --output ${_xahau_cleaned}
                    --emit-json
                COMMAND ${Python3_EXECUTABLE} ${_gen}
                    --input ${CMAKE_CURRENT_LIST_DIR}/definitions/xrpl_definitions.json
                    --output ${_xrpl_cleaned}
                    --emit-json
                DEPENDS
                    ${_gen}
                    ${CMAKE_CURRENT_LIST_DIR}/definitions/xahau_definitions.json
                    ${CMAKE_CURRENT_LIST_DIR}/definitions/xrpl_definitions.json
                COMMENT "Cleaning protocol JSON for table equality"
            )
            add_custom_target(jshookz_cleaned_protocol_json
                DEPENDS ${_xahau_cleaned} ${_xrpl_cleaned}
            )
            add_executable(test_protocol_tables tests/test_protocol_tables.cpp)
            target_link_libraries(test_protocol_tables PRIVATE
                jshookz_xdata_json GTest::gtest_main
            )
            target_compile_definitions(test_protocol_tables PRIVATE
                JSHOOKZ_XAHAU_CLEANED_JSON="${_xahau_cleaned}"
                JSHOOKZ_XRPL_CLEANED_JSON="${_xrpl_cleaned}"
            )
            file(SHA256
                ${CMAKE_CURRENT_LIST_DIR}/definitions/xahau_definitions.json
                _xahau_definitions_sha256)
            file(SHA256
                ${CMAKE_CURRENT_LIST_DIR}/definitions/provider_static_policy.json
                _xahau_provider_policy_sha256)
            target_compile_definitions(test_protocol_tables PRIVATE
                JSHOOKZ_XAHAU_DEFINITIONS_SHA256="${_xahau_definitions_sha256}"
                JSHOOKZ_XAHAU_PROVIDER_POLICY_SHA256="${_xahau_provider_policy_sha256}"
            )
            add_dependencies(test_protocol_tables
                jshookz_cleaned_protocol_json generate_xdata_definitions)
            gtest_discover_tests(test_protocol_tables)
        endif()
    endif()
endif()
