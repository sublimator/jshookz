# cpp/xahau-types — STBlob, Hash256, AccountID, XFL, UInt, Result, ByteView.
#
# These TUs do not include hook_imports.hpp and do not call hook_* or host_*.
# Host crossings and host_* Result factories stay in cpp/provider/bindings/.
# cpp/provider compiles the same TUs via provider_sources.cmake.

include(${CMAKE_CURRENT_LIST_DIR}/xahau_types_sources.cmake)

add_library(jshookz_xahau_types STATIC
    ${JSHOOKZ_XAHAU_TYPES_SOURCES}
)
target_include_directories(jshookz_xahau_types PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
)
target_link_libraries(jshookz_xahau_types PUBLIC jshookz_qjs_src)
target_compile_features(jshookz_xahau_types PUBLIC cxx_std_23)
# QuickJS JS_CFUNC_DEF mixes designated and positional initializers.
# Conan sets -std=c++23 with extensions OFF; ISO C++ rejects that mix.
set_target_properties(jshookz_xahau_types PROPERTIES CXX_EXTENSIONS ON)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(jshookz_xahau_types PRIVATE -std=gnu++23)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(jshookz_xahau_types PRIVATE -Wno-c99-designator)
endif()

if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    include(GoogleTest)
    add_executable(test_xahau_types tests/test_xahau_types.cpp)
    target_link_libraries(test_xahau_types PRIVATE jshookz_xahau_types GTest::gtest_main)
    gtest_discover_tests(test_xahau_types)
endif()
