# Wasm-pinned x-data TUs. Not the whole vendor tree: no protocol_json, no CLI.
set(JSHOOKZ_XDATA_INCLUDE_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/includes
    ${CMAKE_CURRENT_LIST_DIR}/core/includes
    ${CMAKE_CURRENT_LIST_DIR}/base58/includes
    ${CMAKE_CURRENT_LIST_DIR}/generated
)
set(JSHOOKZ_XDATA_WASM_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/src/protocol.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/embedded_protocol.cpp
    ${CMAKE_CURRENT_LIST_DIR}/base58/src/base58.cpp
    ${CMAKE_CURRENT_LIST_DIR}/core/src/types.cpp
)
