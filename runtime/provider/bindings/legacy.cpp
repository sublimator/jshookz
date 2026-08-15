#include "common.hpp"

#include <cstring>

namespace jshookz::provider::bindings {

#ifndef CONFIG_XAHAU_HOOK_PROVIDER
namespace {

extern "C" {
__attribute__((import_module("env"), import_name("host_log")))
void host_log(const char *ptr, uint32_t len);

__attribute__((import_module("env"), import_name("host_get_balance")))
int64_t host_get_balance(const char *ptr, uint32_t len);

__attribute__((import_module("env"), import_name("host_get_state")))
uint32_t host_get_state(const char *key_ptr, uint32_t key_len,
                        char *out_buf, uint32_t out_buf_len);

__attribute__((import_module("env"), import_name("host_get_tx_info")))
uint32_t host_get_tx_info(char *out_buf, uint32_t out_buf_len);

__attribute__((import_module("env"), import_name("host_util_sha256")))
uint32_t host_util_sha256(const uint8_t *data_ptr, uint32_t data_len,
                          uint8_t *out_buf, uint32_t out_buf_len);

__attribute__((import_module("env"), import_name("host_util_sha512h")))
uint32_t host_util_sha512h(const uint8_t *data_ptr, uint32_t data_len,
                           uint8_t *out_buf, uint32_t out_buf_len);

__attribute__((import_module("env"), import_name("host_util_ripemd160")))
uint32_t host_util_ripemd160(const uint8_t *data_ptr, uint32_t data_len,
                             uint8_t *out_buf, uint32_t out_buf_len);
}

char state_buf[4096];
char tx_info_buf[8192];

JSValue
js_host_log(JSContext *ctx, JSValueConst this_val,
            int argc, JSValueConst *argv)
{
    const char *msg = JS_ToCString(ctx, argv[0]);
    if (!msg) return JS_EXCEPTION;
    host_log(msg, strlen(msg));
    JS_FreeCString(ctx, msg);
    return JS_UNDEFINED;
}

JSValue
js_host_get_balance(JSContext *ctx, JSValueConst this_val,
                    int argc, JSValueConst *argv)
{
    const char *addr = JS_ToCString(ctx, argv[0]);
    if (!addr) return JS_EXCEPTION;
    int64_t balance = host_get_balance(addr, strlen(addr));
    JS_FreeCString(ctx, addr);
    return JS_NewInt64(ctx, balance);
}

JSValue
js_host_get_state(JSContext *ctx, JSValueConst this_val,
                  int argc, JSValueConst *argv)
{
    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_EXCEPTION;

    uint32_t len = host_get_state(key, strlen(key), state_buf,
                                  sizeof(state_buf));
    JS_FreeCString(ctx, key);

    if (len == 0)
        return JS_NULL;
    return JS_NewStringLen(ctx, state_buf, len);
}

JSValue
js_host_get_tx_info(JSContext *ctx, JSValueConst this_val,
                    int argc, JSValueConst *argv)
{
    uint32_t len = host_get_tx_info(tx_info_buf, sizeof(tx_info_buf));
    if (len == 0)
        return JS_NULL;
    /* return raw JSON string — JS contract can JSON.parse() it */
    return JS_NewStringLen(ctx, tx_info_buf, len);
}

JSValue
js_util_sha256(JSContext *ctx, JSValueConst this_val,
               int argc, JSValueConst *argv)
{
    auto in = qjs::ByteView::get(
        ctx, argv[0], qjs::StringBytes::hex);
    if (!in)
        return qjs::byteInputTypeError(
            ctx, "util_sha256", qjs::StringBytes::hex);

    uint8_t hash[32];
    host_util_sha256(in.data(), in.size(), hash, 32);
    return qjs::uint8Array(ctx, {hash, 32});
}

JSValue
js_util_sha512h(JSContext *ctx, JSValueConst this_val,
                int argc, JSValueConst *argv)
{
    auto in = qjs::ByteView::get(
        ctx, argv[0], qjs::StringBytes::hex);
    if (!in)
        return qjs::byteInputTypeError(
            ctx, "util_sha512h", qjs::StringBytes::hex);

    uint8_t hash[32];
    host_util_sha512h(in.data(), in.size(), hash, 32);
    return qjs::uint8Array(ctx, {hash, 32});
}

JSValue
js_util_ripemd160(JSContext *ctx, JSValueConst this_val,
                  int argc, JSValueConst *argv)
{
    auto in = qjs::ByteView::get(
        ctx, argv[0], qjs::StringBytes::hex);
    if (!in)
        return qjs::byteInputTypeError(
            ctx, "util_ripemd160", qjs::StringBytes::hex);

    uint8_t hash[20];
    host_util_ripemd160(in.data(), in.size(), hash, 20);
    return qjs::uint8Array(ctx, {hash, 20});
}

}  // namespace
#endif

bool
registerLegacy(JSContext *ctx, JSValue global)
{
#ifndef CONFIG_XAHAU_HOOK_PROVIDER
    return
        JS_SetPropertyStr(ctx, global, "host_log",
            JS_NewCFunction(ctx, js_host_log, "host_log", 1)) >= 0 &&
        JS_SetPropertyStr(ctx, global, "get_balance",
            JS_NewCFunction(ctx, js_host_get_balance, "get_balance", 1)) >= 0 &&
        JS_SetPropertyStr(ctx, global, "get_state",
            JS_NewCFunction(ctx, js_host_get_state, "get_state", 1)) >= 0 &&
        JS_SetPropertyStr(ctx, global, "get_tx_info",
            JS_NewCFunction(ctx, js_host_get_tx_info, "get_tx_info", 0)) >= 0 &&
        JS_SetPropertyStr(ctx, global, "util_sha256",
            JS_NewCFunction(ctx, js_util_sha256, "util_sha256", 1)) >= 0 &&
        JS_SetPropertyStr(ctx, global, "util_sha512h",
            JS_NewCFunction(ctx, js_util_sha512h, "util_sha512h", 1)) >= 0 &&
        JS_SetPropertyStr(ctx, global, "util_ripemd160",
            JS_NewCFunction(ctx, js_util_ripemd160, "util_ripemd160", 1)) >= 0;
#else
    (void)ctx;
    (void)global;
    return true;
#endif
}

}  // namespace jshookz::provider::bindings
