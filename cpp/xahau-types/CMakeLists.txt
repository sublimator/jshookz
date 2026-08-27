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
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    include(GoogleTest)
    add_executable(test_xahau_types
        ../provider/enum_namespaces.cpp
        tests/test_xahau_types.cpp
        tests/test_account_json.cpp
        tests/test_canonical_json.cpp
        tests/test_nominal_payload.cpp
    )
    target_link_libraries(test_xahau_types PRIVATE jshookz_xahau_types GTest::gtest_main)
    gtest_discover_tests(test_xahau_types)

    add_test(
        NAME XFLArithmeticOracleReader
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_LIST_DIR}/tests/test_xfl_arithmetic_oracle.py
    )

    add_executable(xfl_arithmetic_probe
        tests/xfl_arithmetic_probe.cpp
    )
    target_link_libraries(xfl_arithmetic_probe PRIVATE jshookz_xahau_types)
    add_test(
        NAME XFLArithmeticNativeQuickJSOracle
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_LIST_DIR}/tests/run_xfl_arithmetic_oracle.py
            ${CMAKE_CURRENT_LIST_DIR}/tests/xahau_float_v1_add_subtract_oracle.json
            $<TARGET_FILE:xfl_arithmetic_probe>
    )

    add_executable(test_object_replacement_oom
        tests/test_object_replacement_oom.cpp
    )
    target_link_libraries(test_object_replacement_oom PRIVATE jshookz_xahau_types GTest::gtest_main)
    gtest_discover_tests(test_object_replacement_oom)

    add_executable(test_object_reflection_oom
        tests/test_object_reflection_oom.cpp
    )
    target_link_libraries(test_object_reflection_oom PRIVATE jshookz_xahau_types GTest::gtest_main)
    gtest_discover_tests(test_object_reflection_oom)

    add_executable(pathset_js_probe pathset/pathset_js_probe.cpp)
    target_link_libraries(pathset_js_probe PRIVATE jshookz_xahau_types)
    add_test(NAME PathSetJSProbe COMMAND pathset_js_probe)

    # Compile the production type source TUs a second time for the GC poison
    # probe. The mark-omission switch and finalizer counters therefore do not
    # exist in jshookz_xahau_types or the sealed provider.
    add_executable(object_gc_lifetime_probe
        ${JSHOOKZ_XAHAU_TYPES_SOURCES}
        tests/object_gc_lifetime_probe.cpp
        tests/object_gc_lifetime_probe_hooks.cpp
    )
    target_include_directories(object_gc_lifetime_probe PRIVATE
        ${JSHOOKZ_XAHAU_TYPES_INCLUDE_DIR}
    )
    target_link_libraries(object_gc_lifetime_probe PRIVATE
        jshookz_qjs_src
        jshookz_xdata
    )
    target_compile_features(object_gc_lifetime_probe PRIVATE cxx_std_23)
    target_compile_definitions(object_gc_lifetime_probe PRIVATE
        JSHOOKZ_XAHAU_TYPES_GC_PROBE=1
    )
    add_test(
        NAME ObjectGCLifetimeProbe
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_LIST_DIR}/tests/run_object_gc_lifetime_probe.py
            --probe $<TARGET_FILE:object_gc_lifetime_probe>
    )
endif()
