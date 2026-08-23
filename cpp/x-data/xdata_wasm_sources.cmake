# Wasm-pinned x-data TUs. Not the whole vendor tree: no protocol_json, no CLI.
set(JSHOOKZ_XDATA_INCLUDE_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/includes
    ${CMAKE_CURRENT_LIST_DIR}/core/includes
    ${CMAKE_CURRENT_LIST_DIR}/base58/includes
    ${CMAKE_CURRENT_LIST_DIR}/generated
)

# Allocation-free, no-throw x-data inventory selected by the sealed provider.
# Keep dynamic Protocol and embedded_protocol.cpp out of this list.
set(JSHOOKZ_XDATA_PROVIDER_STATIC_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/src/canonical_serializer.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/static_protocol.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/recursive_index.cpp
)

set(JSHOOKZ_XDATA_WASM_SOURCES
    ${JSHOOKZ_XDATA_PROVIDER_STATIC_SOURCES}
    ${CMAKE_CURRENT_LIST_DIR}/src/protocol.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/embedded_protocol.cpp
    ${CMAKE_CURRENT_LIST_DIR}/base58/src/base58.cpp
    ${CMAKE_CURRENT_LIST_DIR}/core/src/types.cpp
)
