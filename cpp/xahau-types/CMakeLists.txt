# cpp/xahau-types — one folder per type family (hash+uint, account, blob, …).
#
# These TUs do not include hook_imports.hpp and do not call hook_* or host_*.
# Host crossings and host_* Result factories stay in cpp/provider/bindings/.
# cpp/provider compiles the same TUs via provider_sources.cmake.

include(${CMAKE_CURRENT_LIST_DIR}/xahau_types_sources.cmake)

add_library(jshookz_xahau_types STATIC
    ${JSHOOKZ_XAHAU_TYPES_SOURCES}
)
target_include_directories(jshookz_xahau_types PUBLIC
    ${JSHOOKZ_XAHAU_TYPES_INCLUDE_DIR}
)
target_link_libraries(jshookz_xahau_types PUBLIC
    jshookz_qjs_src
    jshookz_xdata
)
target_compile_features(jshookz_xahau_types PUBLIC cxx_std_23)

if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    include(GoogleTest)
    add_executable(test_xahau_types
        tests/test_xahau_types.cpp
        tests/test_account_json.cpp
        tests/test_canonical_json.cpp
    )
    target_link_libraries(test_xahau_types PRIVATE jshookz_xahau_types GTest::gtest_main)
    gtest_discover_tests(test_xahau_types)

    add_executable(pathset_js_probe pathset/pathset_js_probe.cpp)
    target_link_libraries(pathset_js_probe PRIVATE jshookz_xahau_types)
    add_test(NAME PathSetJSProbe COMMAND pathset_js_probe)
endif()
