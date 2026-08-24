# Host-blind Xahau JS values. Same TUs in the sealed wasm and the Mac gtests.
# Rule: these files do not include hook_imports.hpp and do not call hook_* / host_*.
set(JSHOOKZ_XAHAU_TYPES_INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR})
set(JSHOOKZ_XAHAU_TYPES_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/js.cpp
    ${CMAKE_CURRENT_LIST_DIR}/quickjs.cpp
    ${CMAKE_CURRENT_LIST_DIR}/runtime_type.cpp
    ${CMAKE_CURRENT_LIST_DIR}/result_js.cpp
    ${CMAKE_CURRENT_LIST_DIR}/blob/blob_js.cpp
    ${CMAKE_CURRENT_LIST_DIR}/hash/hash256_js.cpp
    ${CMAKE_CURRENT_LIST_DIR}/hash/uint_js.cpp
    ${CMAKE_CURRENT_LIST_DIR}/account/account_js.cpp
    ${CMAKE_CURRENT_LIST_DIR}/account/account_json.cpp
    ${CMAKE_CURRENT_LIST_DIR}/amount/amount_js.cpp
    ${CMAKE_CURRENT_LIST_DIR}/xfl/xfl_js.cpp
    ${CMAKE_CURRENT_LIST_DIR}/leaf/leaf_js.cpp
    ${CMAKE_CURRENT_LIST_DIR}/pathset/pathset_js.cpp
    ${CMAKE_CURRENT_LIST_DIR}/object/canonical_json.cpp
    ${CMAKE_CURRENT_LIST_DIR}/object/nominal_payload.cpp
    ${CMAKE_CURRENT_LIST_DIR}/object/object_js.cpp
    ${CMAKE_CURRENT_LIST_DIR}/object/field_js.cpp
)
