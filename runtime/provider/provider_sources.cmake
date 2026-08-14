# One provider source inventory shared by the deployable Xahau build and the
# codec fixture. Keep API categories in bindings/; provider.cpp owns only the
# exported Wasm lifecycle.
set(JSHOOKZ_PROVIDER_CPP_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/provider.cpp
    ${CMAKE_CURRENT_LIST_DIR}/quickjs.cpp
    ${CMAKE_CURRENT_LIST_DIR}/sandbox.cpp
    ${CMAKE_CURRENT_LIST_DIR}/types_js.cpp
    ${CMAKE_CURRENT_LIST_DIR}/bindings/common.cpp
    ${CMAKE_CURRENT_LIST_DIR}/bindings/control.cpp
    ${CMAKE_CURRENT_LIST_DIR}/bindings/emission.cpp
    ${CMAKE_CURRENT_LIST_DIR}/bindings/ledger.cpp
    ${CMAKE_CURRENT_LIST_DIR}/bindings/legacy.cpp
    ${CMAKE_CURRENT_LIST_DIR}/bindings/register.cpp
    ${CMAKE_CURRENT_LIST_DIR}/bindings/state.cpp
    ${CMAKE_CURRENT_LIST_DIR}/bindings/trace.cpp
)
