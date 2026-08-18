# Vendored QuickJS C engine. No libc. Same TUs on host and in the sealed wasm.
set(JSHOOKZ_QUICKJS_SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../quickjs)
file(READ ${JSHOOKZ_QUICKJS_SOURCE_DIR}/QUICKJS_VERSION _qjs_version)
string(STRIP "${_qjs_version}" JSHOOKZ_QJS_CONFIG_VERSION)
set(JSHOOKZ_QUICKJS_C_SOURCES
    ${JSHOOKZ_QUICKJS_SOURCE_DIR}/quickjs.c
    ${JSHOOKZ_QUICKJS_SOURCE_DIR}/cutils.c
    ${JSHOOKZ_QUICKJS_SOURCE_DIR}/dtoa.c
    ${JSHOOKZ_QUICKJS_SOURCE_DIR}/libregexp.c
    ${JSHOOKZ_QUICKJS_SOURCE_DIR}/libunicode.c
)
