include(${CMAKE_CURRENT_LIST_DIR}/xdata_qjs_sources.cmake)

add_library(jshookz_xdata_qjs STATIC ${JSHOOKZ_XDATA_QJS_SOURCES})
target_include_directories(jshookz_xdata_qjs PUBLIC
    ${JSHOOKZ_XDATA_QJS_INCLUDE_DIR}
)
target_link_libraries(jshookz_xdata_qjs PUBLIC
    jshookz_xdata
    jshookz_qjs_src
)
target_compile_definitions(jshookz_xdata_qjs PRIVATE
    CATL_XDATA_NO_BOOST_JSON
    CODEC_BACKEND_XDATA
)

if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    include(GoogleTest)

    add_executable(qjs_runner tests/qjs_runner.cpp)
    target_link_libraries(qjs_runner PRIVATE jshookz_xdata_qjs)

    add_executable(test_no_boost_headers tests/no_boost_json_probe.cpp)
    target_link_libraries(test_no_boost_headers PRIVATE jshookz_xdata_qjs)
    add_test(NAME no_boost_headers COMMAND test_no_boost_headers)

    add_executable(test_xdata_qjs tests/test_xdata_qjs.cpp)
    target_link_libraries(test_xdata_qjs PRIVATE jshookz_xdata_qjs GTest::gtest_main)
    target_compile_definitions(test_xdata_qjs PRIVATE
        JSHOOKZ_XDATA_QJS_FIXTURE_DIR="${CMAKE_CURRENT_LIST_DIR}/tests/fixtures"
    )
    gtest_discover_tests(test_xdata_qjs)
endif()
