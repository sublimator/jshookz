include(${CMAKE_CURRENT_LIST_DIR}/quickjs_cpp_sources.cmake)

add_library(jshookz_qjs INTERFACE)
target_include_directories(jshookz_qjs INTERFACE
    ${JSHOOKZ_QJS_CPP_INCLUDE_DIR}
)
target_compile_features(jshookz_qjs INTERFACE cxx_std_23)

if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    add_executable(test_qjs_hex tests/test_qjs_hex.cpp)
    target_link_libraries(test_qjs_hex PRIVATE jshookz_qjs GTest::gtest_main)
    include(GoogleTest)
    gtest_discover_tests(test_qjs_hex)
endif()
