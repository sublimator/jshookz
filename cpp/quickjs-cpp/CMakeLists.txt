include(${CMAKE_CURRENT_LIST_DIR}/quickjs_cpp_sources.cmake)

set(JSHOOKZ_QUICKJS_SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../quickjs)
file(READ ${JSHOOKZ_QUICKJS_SOURCE_DIR}/VERSION _qjs_version)
string(STRIP "${_qjs_version}" JSHOOKZ_QJS_CONFIG_VERSION)

set(JSHOOKZ_QUICKJS_C_SOURCES
    ${JSHOOKZ_QUICKJS_SOURCE_DIR}/quickjs.c
    ${JSHOOKZ_QUICKJS_SOURCE_DIR}/cutils.c
    ${JSHOOKZ_QUICKJS_SOURCE_DIR}/dtoa.c
    ${JSHOOKZ_QUICKJS_SOURCE_DIR}/libregexp.c
    ${JSHOOKZ_QUICKJS_SOURCE_DIR}/libunicode.c
)

add_library(jshookz_qjs INTERFACE)
target_include_directories(jshookz_qjs INTERFACE
    ${JSHOOKZ_QJS_CPP_INCLUDE_DIR}
)
target_compile_features(jshookz_qjs INTERFACE cxx_std_23)

add_library(jshookz_quickjs STATIC ${JSHOOKZ_QUICKJS_C_SOURCES})
set_source_files_properties(${JSHOOKZ_QUICKJS_C_SOURCES} PROPERTIES
    INCLUDE_DIRECTORIES ${JSHOOKZ_QUICKJS_SOURCE_DIR}
    COMPILE_OPTIONS "-fwrapv;-funsigned-char"
)
target_compile_definitions(jshookz_quickjs PRIVATE
    CONFIG_VERSION="${JSHOOKZ_QJS_CONFIG_VERSION}"
    _GNU_SOURCE
)
if(UNIX)
    target_link_libraries(jshookz_quickjs PUBLIC m)
endif()

add_library(jshookz_qjs_src STATIC ${JSHOOKZ_QJS_CPP_SOURCES})
target_link_libraries(jshookz_qjs_src PUBLIC jshookz_qjs jshookz_quickjs)

if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    add_executable(test_qjs_hex tests/test_qjs_hex.cpp)
    target_link_libraries(test_qjs_hex PRIVATE jshookz_qjs GTest::gtest_main)
    include(GoogleTest)
    gtest_discover_tests(test_qjs_hex)
endif()
