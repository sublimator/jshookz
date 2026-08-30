# One provider source inventory. Xahau JS values live in ../xahau-types
# (no hook_* / host_*). Bindings/ own host crossings. provider.cpp owns
# the exported Wasm lifecycle.
include(${CMAKE_CURRENT_LIST_DIR}/../quickjs-cpp/quickjs_c_sources.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../quickjs-cpp/quickjs_cpp_sources.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../x-data/xdata_wasm_sources.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../xahau-types/xahau_types_sources.cmake)
set(JSHOOKZ_PROVIDER_CPP_SOURCES
    ${JSHOOKZ_QJS_CPP_SOURCES}
    ${JSHOOKZ_XDATA_PROVIDER_STATIC_SOURCES}
    ${JSHOOKZ_XAHAU_TYPES_SOURCES}
    ${CMAKE_CURRENT_LIST_DIR}/provider.cpp
    ${CMAKE_CURRENT_LIST_DIR}/enum_namespaces.cpp
    ${CMAKE_CURRENT_LIST_DIR}/sandbox.cpp
    ${CMAKE_CURRENT_LIST_DIR}/xfl_profile.cpp
    ${CMAKE_CURRENT_LIST_DIR}/bindings/common.cpp
    ${CMAKE_CURRENT_LIST_DIR}/bindings/control.cpp
    ${CMAKE_CURRENT_LIST_DIR}/bindings/emission.cpp
    ${CMAKE_CURRENT_LIST_DIR}/bindings/entropy.cpp
    ${CMAKE_CURRENT_LIST_DIR}/bindings/ledger.cpp
    ${CMAKE_CURRENT_LIST_DIR}/bindings/legacy.cpp
    ${CMAKE_CURRENT_LIST_DIR}/bindings/register.cpp
    ${CMAKE_CURRENT_LIST_DIR}/bindings/state.cpp
    ${CMAKE_CURRENT_LIST_DIR}/bindings/trace.cpp
)
