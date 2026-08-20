include(${CMAKE_CURRENT_LIST_DIR}/xdata_wasm_sources.cmake)

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
    CODEC_BACKEND_XDATA
)
target_compile_features(jshookz_xdata PUBLIC cxx_std_23)

find_path(Boost_INCLUDE_DIR
    boost/json.hpp
    HINTS ENV BOOST_INCLUDE_DIR
    PATHS /opt/homebrew/include /usr/include
)
# Separate TUs, no CATL_XDATA_NO_BOOST_JSON. Do not link this with
# jshookz_xdata — same symbols, different class layout.
if(Boost_INCLUDE_DIR)
    add_library(jshookz_xdata_json STATIC
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
endif()

if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    include(GoogleTest)

    add_executable(test_parse_helpers tests/test_parse_helpers.cpp)
    target_link_libraries(test_parse_helpers PRIVATE jshookz_xdata GTest::gtest_main)
    gtest_discover_tests(test_parse_helpers)

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
        )
        gtest_discover_tests(test_scan)

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
        gtest_discover_tests(test_scan_alloc)

        find_package(Python3 COMPONENTS Interpreter)
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
            add_dependencies(test_protocol_tables jshookz_cleaned_protocol_json)
            gtest_discover_tests(test_protocol_tables)
        endif()
    endif()
endif()
