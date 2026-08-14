/*
 * wasm_bindings.c - Thin WASM API surface for QuickJS smart contract runtime
 *
 * This file is compiled INTO the WASM module alongside QuickJS.
 * It exports functions the host calls, and declares imports the host provides.
 */

#include "../../engine/quickjs/quickjs.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* C++ types registration (types_js.cpp) */
extern void register_cpp_types(JSContext *ctx);

#ifdef CONFIG_PROTOCOL_XDATA
/* xdata protocol registration (bridge_xdata.cpp) */
extern void register_protocol_functions(JSContext *ctx);
#endif

/* The ordinary embedding host exposes prototype-only helpers.  Xahau instead
   provides the canonical raw Hook ABI generated below, so its provider build
   must not retain imports which no Xahau Hook host can satisfy. */
#ifndef CONFIG_XAHAU_HOOK_PROVIDER

/* ---- Coverage host import ---- */

__attribute__((import_module("env"), import_name("host_coverage_hit")))
extern void host_coverage_hit(const char *filename_ptr, uint32_t filename_len,
                               uint32_t line_num);

/* ---- Module loader host import ---- */

/* Host resolves a module name to precompiled bytecode.
   Writes bytecode into out_buf, returns bytes written (or -1 on error).
   This is how JS `import` statements resolve to on-chain modules. */
__attribute__((import_module("env"), import_name("host_resolve_module")))
extern int32_t host_resolve_module(const char *name_ptr, uint32_t name_len,
                                    uint8_t *out_buf, uint32_t out_buf_len);

/* ---- Host function imports (provided by WasmEdge host) ---- */

__attribute__((import_module("env"), import_name("host_log")))
extern void host_log(const char *ptr, uint32_t len);

__attribute__((import_module("env"), import_name("host_get_balance")))
extern int64_t host_get_balance(const char *ptr, uint32_t len);

__attribute__((import_module("env"), import_name("host_get_state")))
extern uint32_t host_get_state(const char *key_ptr, uint32_t key_len,
                               char *out_buf, uint32_t out_buf_len);

__attribute__((import_module("env"), import_name("host_get_tx_info")))
extern uint32_t host_get_tx_info(char *out_buf, uint32_t out_buf_len);

#endif

/* Raw ABI declarations are projected from hookz's Tree-sitter parse of the
   pinned Xahau hook_api.macro. Rich JS lowering remains handwritten below. */
#include "generated/hook_raw_imports.inc"

/* Single-threaded, init-once, non-reentrant: static scratch buffers are safe. */
#ifndef CONFIG_XAHAU_HOOK_PROVIDER
static char state_buf[4096];
static char tx_info_buf[8192];
static uint8_t module_loader_buf[65536];

/* ---- Utility host imports (host_util_ prefix) ---- */

__attribute__((import_module("env"), import_name("host_util_sha256")))
extern uint32_t host_util_sha256(const uint8_t *data_ptr, uint32_t data_len,
                                  uint8_t *out_buf, uint32_t out_buf_len);

__attribute__((import_module("env"), import_name("host_util_sha512h")))
extern uint32_t host_util_sha512h(const uint8_t *data_ptr, uint32_t data_len,
                                   uint8_t *out_buf, uint32_t out_buf_len);

__attribute__((import_module("env"), import_name("host_util_ripemd160")))
extern uint32_t host_util_ripemd160(const uint8_t *data_ptr, uint32_t data_len,
                                     uint8_t *out_buf, uint32_t out_buf_len);

/* ---- QuickJS wrappers (JS-callable, call the host imports) ---- */

static JSValue js_host_log(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    const char *msg = JS_ToCString(ctx, argv[0]);
    if (!msg) return JS_EXCEPTION;
    host_log(msg, strlen(msg));
    JS_FreeCString(ctx, msg);
    return JS_UNDEFINED;
}

static JSValue js_host_get_balance(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    const char *addr = JS_ToCString(ctx, argv[0]);
    if (!addr) return JS_EXCEPTION;
    int64_t balance = host_get_balance(addr, strlen(addr));
    JS_FreeCString(ctx, addr);
    return JS_NewInt64(ctx, balance);
}

static JSValue js_host_get_state(JSContext *ctx, JSValueConst this_val,
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

static JSValue js_host_get_tx_info(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    uint32_t len = host_get_tx_info(tx_info_buf, sizeof(tx_info_buf));
    if (len == 0)
        return JS_NULL;
    /* return raw JSON string — JS contract can JSON.parse() it */
    return JS_NewStringLen(ctx, tx_info_buf, len);
}

#endif

/* ---- Util: BytesLike input helper (C version) ---- */

/* Extract raw bytes from a JSValue: Uint8Array, ArrayBuffer, or hex string.
   Returns pointer into WASM memory (zero-copy for typed arrays).
   Sets *out_len. Caller must call cleanup_bytes_input() after use. */
typedef struct {
    const uint8_t *ptr;
    uint32_t len;
    uint8_t *owned;    /* non-NULL if we allocated (hex parse) */
    const char *cstring; /* non-NULL for an API's UTF-8 string input */
    JSValue ab_ref;     /* prevent GC for typed array buffer */
} BytesInput;

static int get_bytes_input(JSContext *ctx, JSValueConst val, BytesInput *out)
{
    size_t offset, byte_len;
    out->ptr = NULL;
    out->len = 0;
    out->owned = NULL;
    out->cstring = NULL;
    out->ab_ref = JS_UNDEFINED;

    /* Try TypedArray */
    JSValue ab = JS_GetTypedArrayBuffer(ctx, val, &offset, &byte_len, NULL);
    if (!JS_IsException(ab)) {
        size_t buf_size;
        uint8_t *buf = JS_GetArrayBuffer(ctx, &buf_size, ab);
        if (buf) {
            out->ptr = buf + offset;
            out->len = (uint32_t)byte_len;
            out->ab_ref = ab;
            return 1;
        }
        JS_FreeValue(ctx, ab);
    } else {
        JSValue exc = JS_GetException(ctx);
        JS_FreeValue(ctx, exc);
    }

    /* Try ArrayBuffer */
    {
        size_t buf_size;
        uint8_t *buf = JS_GetArrayBuffer(ctx, &buf_size, val);
        if (buf) {
            out->ptr = buf;
            out->len = (uint32_t)buf_size;
            return 1;
        }
    }

    /* Try hex string */
    if (JS_IsString(val)) {
        size_t slen;
        const char *s = JS_ToCStringLen(ctx, &slen, val);
        if (!s || slen % 2 != 0) {
            if (s) JS_FreeCString(ctx, s);
            return 0;
        }
        uint32_t n = (uint32_t)(slen / 2);
        out->owned = (uint8_t *)js_malloc(ctx, n);
        if (!out->owned) { JS_FreeCString(ctx, s); return 0; }
        for (uint32_t i = 0; i < n; i++) {
            int hi, lo;
            char c;
            c = s[i * 2];
            hi = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            c = s[i * 2 + 1];
            lo = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (hi < 0 || lo < 0) {
                JS_FreeCString(ctx, s);
                js_free(ctx, out->owned);
                out->owned = NULL;
                return 0;
            }
            out->owned[i] = (uint8_t)((hi << 4) | lo);
        }
        JS_FreeCString(ctx, s);
        out->ptr = out->owned;
        out->len = n;
        return 1;
    }
    return 0;
}

static void free_bytes_input(JSContext *ctx, BytesInput *in)
{
    if (in->owned) { js_free(ctx, in->owned); in->owned = NULL; }
    if (in->cstring) { JS_FreeCString(ctx, in->cstring); in->cstring = NULL; }
    JS_FreeValue(ctx, in->ab_ref);
    in->ab_ref = JS_UNDEFINED;
}

/* Hook API strings are literal UTF-8 bytes (state keys such as "CNT"), while
   the older crypto BytesLike helper treats strings as hexadecimal. */
static int get_hook_input(JSContext *ctx, JSValueConst val, BytesInput *out)
{
    if (JS_IsString(val)) {
        size_t len;
        const char *text = JS_ToCStringLen(ctx, &len, val);
        if (!text)
            return 0;
        out->ptr = (const uint8_t *)text;
        out->len = (uint32_t)len;
        out->owned = NULL;
        out->cstring = text;
        out->ab_ref = JS_UNDEFINED;
        return 1;
    }

    if (get_bytes_input(ctx, val, out))
        return 1;

    /* Rich binary values cross raw host calls through their canonical bytes. */
    JSValue to_bytes = JS_GetPropertyStr(ctx, val, "toBytes");
    if (!JS_IsFunction(ctx, to_bytes)) {
        JS_FreeValue(ctx, to_bytes);
        return 0;
    }
    JSValue bytes = JS_Call(ctx, to_bytes, val, 0, NULL);
    JS_FreeValue(ctx, to_bytes);
    if (JS_IsException(bytes))
        return 0;
    int ok = get_bytes_input(ctx, bytes, out);
    JS_FreeValue(ctx, bytes);
    return ok;
}

static JSValue make_uint8array(JSContext *ctx, const uint8_t *data,
                               uint32_t len);

static JSValue host_success(JSContext *ctx, JSValue value)
{
    if (JS_IsException(value))
        return value;
    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "ok", JS_TRUE);
    JS_SetPropertyStr(ctx, result, "value", value);
    return result;
}

static JSValue host_failure(JSContext *ctx, int64_t code)
{
    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "ok", JS_FALSE);
    JS_SetPropertyStr(ctx, result, "code", JS_NewInt64(ctx, code));
    return result;
}

static JSValue rich_from_bytes(JSContext *ctx, const char *type_name,
                               const uint8_t *bytes, uint32_t length)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue type = JS_GetPropertyStr(ctx, global, type_name);
    JSValue from = JS_GetPropertyStr(ctx, type, "from");
    JSValue input = make_uint8array(ctx, bytes, length);
    JSValue value = JS_Call(ctx, from, type, 1, &input);
    JS_FreeValue(ctx, input);
    JS_FreeValue(ctx, from);
    JS_FreeValue(ctx, type);
    JS_FreeValue(ctx, global);
    return value;
}

/* ---- Hook lifecycle terminals ---- */

static JSValue js_hook_accept(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    int64_t code = 0;
    if (argc > 1 && !JS_IsUndefined(argv[1]) &&
        JS_ToInt64(ctx, &code, argv[1]) < 0)
        return JS_EXCEPTION;

    if (argc == 0 || JS_IsUndefined(argv[0]))
        return JS_NewInt64(ctx, hook_accept(0, 0, code));

    /* A lifecycle message string is UTF-8 text, not the hex-string shorthand
       accepted by BytesLike APIs. */
    if (JS_IsString(argv[0])) {
        size_t len;
        const char *message = JS_ToCStringLen(ctx, &len, argv[0]);
        if (!message)
            return JS_EXCEPTION;
        int64_t result = hook_accept(
            (uint32_t)(uintptr_t)message, (uint32_t)len, code);
        JS_FreeCString(ctx, message);
        return JS_NewInt64(ctx, result);
    }

    BytesInput message;
    if (!get_bytes_input(ctx, argv[0], &message))
        return JS_ThrowTypeError(
            ctx, "accept: expected string, Uint8Array, or ArrayBuffer");
    int64_t result = hook_accept(
        (uint32_t)(uintptr_t)message.ptr, message.len, code);
    free_bytes_input(ctx, &message);
    return JS_NewInt64(ctx, result);
}

static JSValue js_hook_rollback(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    int64_t code = 0;
    if (argc > 1 && !JS_IsUndefined(argv[1]) &&
        JS_ToInt64(ctx, &code, argv[1]) < 0)
        return JS_EXCEPTION;
    if (argc == 0 || JS_IsUndefined(argv[0]))
        return JS_NewInt64(ctx, hook_rollback(0, 0, code));

    BytesInput message;
    if (!get_hook_input(ctx, argv[0], &message))
        return JS_ThrowTypeError(
            ctx, "rollback: expected string, Uint8Array, or ArrayBuffer");
    int64_t result = hook_rollback(
        (uint32_t)(uintptr_t)message.ptr, message.len, code);
    free_bytes_input(ctx, &message);
    return JS_NewInt64(ctx, result);
}

static JSValue js_rollback_on_host_failure(JSContext *ctx,
                                           JSValueConst this_val,
                                           int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(
            ctx, "rollback.onHostFailure: expected HostResult");

    JSValue ok = JS_GetPropertyStr(ctx, argv[0], "ok");
    if (JS_IsException(ok))
        return ok;
    if (!JS_IsBool(ok)) {
        JS_FreeValue(ctx, ok);
        return JS_ThrowTypeError(
            ctx, "rollback.onHostFailure: expected boolean ok");
    }
    int success = JS_ToBool(ctx, ok);
    JS_FreeValue(ctx, ok);
    if (success)
        return JS_GetPropertyStr(ctx, argv[0], "value");

    JSValue code_value = JS_GetPropertyStr(ctx, argv[0], "code");
    if (JS_IsException(code_value))
        return code_value;
    int64_t code;
    if (!JS_IsNumber(code_value) || JS_ToInt64(ctx, &code, code_value) < 0) {
        JS_FreeValue(ctx, code_value);
        return JS_ThrowTypeError(
            ctx, "rollback.onHostFailure: expected numeric failure code");
    }
    JS_FreeValue(ctx, code_value);

    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        JSValue code_arg = JS_NewInt64(ctx, code);
        JSValueConst rollback_args[2] = { argv[1], code_arg };
        JSValue result = js_hook_rollback(ctx, this_val, 2, rollback_args);
        JS_FreeValue(ctx, code_arg);
        return result;
    }

    char message[64];
    int length = snprintf(
        message, sizeof(message), "host operation failed: %lld",
        (long long)code);
    if (length < 0 || (size_t)length >= sizeof(message))
        return JS_ThrowInternalError(
            ctx, "rollback.onHostFailure: failed to format status");
    return JS_NewInt64(ctx, hook_rollback(
        (uint32_t)(uintptr_t)message, (uint32_t)length, code));
}

/* ---- Canonical rich adapters for the first raw ABI slice ---- */

static JSValue js_ledger_sequence(JSContext *ctx, JSValueConst this_val)
{
    return JS_NewInt64(ctx, hook_ledger_seq());
}

static JSValue js_ledger_last_time(JSContext *ctx, JSValueConst this_val)
{
    return JS_NewInt64(ctx, hook_ledger_last_time());
}

static JSValue js_ledger_last_hash(JSContext *ctx, JSValueConst this_val)
{
    uint8_t bytes[32];
    int64_t result = hook_ledger_last_hash(
        (uint32_t)(uintptr_t)bytes, sizeof(bytes));
    if (result != (int64_t)sizeof(bytes))
        return JS_ThrowInternalError(
            ctx, "ledger.lastHash: host returned %lld, expected 32",
            (long long)result);
    return rich_from_bytes(ctx, "Hash256", bytes, sizeof(bytes));
}

static JSValue js_otxn_type(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    int64_t result = hook_otxn_type();
    return result < 0
        ? host_failure(ctx, result)
        : host_success(ctx, JS_NewInt64(ctx, result));
}

static JSValue js_lifecycle_account(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    uint8_t bytes[20];
    int64_t result = hook_hook_account(
        (uint32_t)(uintptr_t)bytes, sizeof(bytes));
    if (result < 0)
        return host_failure(ctx, result);
    if (result != (int64_t)sizeof(bytes))
        return JS_ThrowInternalError(
            ctx, "lifecycle.account: host returned %lld, expected 20",
            (long long)result);
    return host_success(ctx,
        rich_from_bytes(ctx, "AccountID", bytes, sizeof(bytes)));
}

static JSValue js_trace(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "trace: expected a string label");

    size_t label_len;
    const char *label = JS_ToCStringLen(ctx, &label_len, argv[0]);
    if (!label)
        return JS_EXCEPTION;

    JSValue rendered = JS_UNDEFINED;
    const char *data = "";
    size_t data_len = 0;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        rendered = JS_ToString(ctx, argv[1]);
        if (JS_IsException(rendered)) {
            JS_FreeCString(ctx, label);
            return rendered;
        }
        data = JS_ToCStringLen(ctx, &data_len, rendered);
        if (!data) {
            JS_FreeValue(ctx, rendered);
            JS_FreeCString(ctx, label);
            return JS_EXCEPTION;
        }
    }

    if (label_len > UINT32_MAX || data_len > UINT32_MAX) {
        if (data_len != 0) JS_FreeCString(ctx, data);
        JS_FreeValue(ctx, rendered);
        JS_FreeCString(ctx, label);
        return JS_ThrowRangeError(ctx, "trace: label or value is too large");
    }

    int64_t result = hook_trace(
        (uint32_t)(uintptr_t)label, (uint32_t)label_len,
        (uint32_t)(uintptr_t)data, (uint32_t)data_len, 0);
    if (!JS_IsUndefined(rendered)) JS_FreeCString(ctx, data);
    JS_FreeValue(ctx, rendered);
    JS_FreeCString(ctx, label);
    if (result < 0)
        return JS_ThrowInternalError(
            ctx, "trace: host returned %lld", (long long)result);
    return JS_UNDEFINED;
}

static JSValue js_state_get(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "state.get: expected a key");
    BytesInput key;
    if (!get_hook_input(ctx, argv[0], &key))
        return JS_ThrowTypeError(ctx, "state.get: invalid key");

    /* Extended Hook state tops out at 16 * 256 bytes. A maximum-sized buffer
       preserves the fixed-buffer host contract: TOO_SMALL never means a
       truncated success. */
    uint8_t value[4096];
    int64_t result = hook_state(
        (uint32_t)(uintptr_t)value, sizeof(value),
        (uint32_t)(uintptr_t)key.ptr, key.len);
    free_bytes_input(ctx, &key);

    if (result == -5) /* DOESNT_EXIST is typed absence, not host failure. */
        return host_success(ctx, JS_UNDEFINED);
    if (result < 0)
        return host_failure(ctx, result);
    if ((uint64_t)result > sizeof(value))
        return JS_ThrowInternalError(
            ctx, "state.get: host returned oversized length %lld",
            (long long)result);
    return host_success(
        ctx, rich_from_bytes(ctx, "STBlob", value, (uint32_t)result));
}

static JSValue js_state_set(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "state.set: expected key and value");
    BytesInput key;
    if (!get_hook_input(ctx, argv[0], &key))
        return JS_ThrowTypeError(ctx, "state.set: invalid key");
    BytesInput value;
    if (!get_hook_input(ctx, argv[1], &value)) {
        free_bytes_input(ctx, &key);
        return JS_ThrowTypeError(ctx, "state.set: invalid value");
    }

    int64_t result = hook_state_set(
        (uint32_t)(uintptr_t)value.ptr, value.len,
        (uint32_t)(uintptr_t)key.ptr, key.len);
    free_bytes_input(ctx, &value);
    free_bytes_input(ctx, &key);
    return result < 0
        ? host_failure(ctx, result)
        : host_success(ctx, JS_UNDEFINED);
}

static JSValue js_emit_prepare(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "emit.prepare: expected transaction bytes");
    BytesInput transaction;
    if (!get_hook_input(ctx, argv[0], &transaction))
        return JS_ThrowTypeError(ctx, "emit.prepare: invalid transaction bytes");

    /* Preparation injects bounded protocol fields into the partial STObject.
       Keep the declared buffer proportional to its input so host-work
       charging reflects this call, while the checked host adapter returns
       TOO_SMALL without writing if this allowance is ever insufficient. */
    static const uint32_t max_transaction_bytes = 1024U * 1024U;
    static const uint32_t preparation_allowance = 4U * 1024U;
    uint64_t requested_capacity =
        (uint64_t)transaction.len + preparation_allowance;
    uint32_t output_capacity = requested_capacity < max_transaction_bytes
        ? (uint32_t)requested_capacity
        : max_transaction_bytes;
    uint8_t *output = (uint8_t *)js_malloc(ctx, output_capacity);
    if (!output) {
        free_bytes_input(ctx, &transaction);
        return JS_EXCEPTION;
    }
    int64_t result = hook_prepare(
        (uint32_t)(uintptr_t)output, output_capacity,
        (uint32_t)(uintptr_t)transaction.ptr, transaction.len);
    free_bytes_input(ctx, &transaction);
    if (result < 0) {
        js_free(ctx, output);
        return host_failure(ctx, result);
    }
    if ((uint64_t)result > output_capacity) {
        js_free(ctx, output);
        return JS_ThrowInternalError(
            ctx, "emit.prepare: host returned oversized length %lld",
            (long long)result);
    }
    JSValue value = rich_from_bytes(ctx, "STBlob", output, (uint32_t)result);
    js_free(ctx, output);
    return host_success(ctx, value);
}

static JSValue js_emit_reserve(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    uint32_t count;
    if (argc < 1 || JS_ToUint32(ctx, &count, argv[0]))
        return JS_ThrowTypeError(ctx, "emit.reserve: expected count");
    int64_t result = hook_etxn_reserve(count);
    return result < 0
        ? host_failure(ctx, result)
        : host_success(ctx, JS_UNDEFINED);
}

static JSValue js_emit_tx(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "emit.tx: expected transaction bytes");
    BytesInput transaction;
    if (!get_hook_input(ctx, argv[0], &transaction))
        return JS_ThrowTypeError(ctx, "emit.tx: invalid transaction bytes");

    uint8_t hash[32];
    int64_t result = hook_emit(
        (uint32_t)(uintptr_t)hash, sizeof(hash),
        (uint32_t)(uintptr_t)transaction.ptr, transaction.len);
    free_bytes_input(ctx, &transaction);
    if (result < 0)
        return host_failure(ctx, result);
    if (result != (int64_t)sizeof(hash))
        return JS_ThrowInternalError(
            ctx, "emit.tx: host returned length %lld", (long long)result);
    return host_success(ctx, rich_from_bytes(ctx, "Hash256", hash, sizeof(hash)));
}

static const JSCFunctionListEntry js_ledger_properties[] = {
    JS_CGETSET_DEF("sequence", js_ledger_sequence, NULL),
    JS_CGETSET_DEF("lastTime", js_ledger_last_time, NULL),
    JS_CGETSET_DEF("lastHash", js_ledger_last_hash, NULL),
};

/* Create Uint8Array from raw bytes */
static JSValue make_uint8array(JSContext *ctx, const uint8_t *data, uint32_t len)
{
    JSValue ab = JS_NewArrayBufferCopy(ctx, data, len);
    if (JS_IsException(ab)) return ab;
    JSValue args[3] = { ab, JS_NewInt32(ctx, 0), JS_NewInt32(ctx, (int32_t)len) };
    JSValue ta = JS_NewTypedArray(ctx, 3, args, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, args[2]);
    return ta;
}

/* ---- Crypto util wrappers ---- */

#ifndef CONFIG_XAHAU_HOOK_PROVIDER

static JSValue js_util_sha256(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    BytesInput in;
    if (!get_bytes_input(ctx, argv[0], &in))
        return JS_ThrowTypeError(ctx, "util_sha256: expected Uint8Array, ArrayBuffer, or hex string");

    uint8_t hash[32];
    host_util_sha256(in.ptr, in.len, hash, 32);
    free_bytes_input(ctx, &in);
    return make_uint8array(ctx, hash, 32);
}

static JSValue js_util_sha512h(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    BytesInput in;
    if (!get_bytes_input(ctx, argv[0], &in))
        return JS_ThrowTypeError(ctx, "util_sha512h: expected Uint8Array, ArrayBuffer, or hex string");

    uint8_t hash[32];
    host_util_sha512h(in.ptr, in.len, hash, 32);
    free_bytes_input(ctx, &in);
    return make_uint8array(ctx, hash, 32);
}

static JSValue js_util_ripemd160(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    BytesInput in;
    if (!get_bytes_input(ctx, argv[0], &in))
        return JS_ThrowTypeError(ctx, "util_ripemd160: expected Uint8Array, ArrayBuffer, or hex string");

    uint8_t hash[20];
    host_util_ripemd160(in.ptr, in.len, hash, 20);
    free_bytes_input(ctx, &in);
    return make_uint8array(ctx, hash, 20);
}

#endif

/* ---- Deterministic sandbox: replace non-deterministic APIs ---- */

/* ---- Deterministic PRNG: native seed cell (issue 0030) ----
 *
 * The seed lives in C, not in a JS closure and not on globalThis, for two
 * reasons: qjs_set_seed no longer has to re-evaluate the sandbox patch (which
 * re-wrapped Date on every call, nesting one FrozenDate layer deeper each
 * time), and a contract cannot reach in and steer its own randomness.
 *
 * The arithmetic below deliberately mirrors ECMAScript double semantics rather
 * than being "cleaned up" into integer math. `seed * 1103515245` exceeds 2^53,
 * so the original JS lost precision *before* the mask — an integer port would
 * produce a different, equally deterministic, but INCOMPATIBLE sequence. The
 * seeded-sequence pins in packages/jshookz/tests guard this; if you touch it, they
 * are the oracle, not your reading of the code.
 */
static uint32_t prng_seed = 42;

/* ECMAScript ToUint32 on a double: truncate toward zero, modulo 2^32. */
static uint32_t js_to_uint32(double d)
{
    if (!isfinite(d))
        return 0;
    double m = fmod(trunc(d), 4294967296.0);
    if (m < 0)
        m += 4294967296.0;
    return (uint32_t)m;
}

static JSValue js_prng_random(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    /* Mirrors: seed = (seed * 1103515245 + 12345) & 0x7fffffff */
    double product = (double)prng_seed * 1103515245.0 + 12345.0;
    prng_seed = js_to_uint32(product) & 0x7fffffffu;
    return JS_NewFloat64(ctx, (double)prng_seed / 2147483647.0);
}

static void sandbox_make_deterministic(JSContext *ctx)
{
#ifdef CONFIG_XAHAU_HOOK_PROVIDER
    /* The underlying Date intrinsic reads the previous ledger close time and
       is UTC.  Do not wrap it: wrapping leaves constructor escapes and breaks
       ordinary explicit Date arguments. */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue math = JS_GetPropertyStr(ctx, global, "Math");
    JSAtom random = JS_NewAtom(ctx, "random");
    JS_DeleteProperty(ctx, math, random, 0);
    JS_FreeAtom(ctx, random);
    JS_FreeValue(ctx, math);

    /* The v1 Hook profile has no shared memory, Atomics, weak references, or
       finalizer scheduling.  WeakRef intrinsics are never installed; remove
       the shared-memory globals created with the TypedArray intrinsic.
       Promise remains available because ES-module evaluation requires it. */
    static const char *const disabled[] = {
        "SharedArrayBuffer", "Atomics"};
    for (size_t i = 0; i < sizeof(disabled) / sizeof(disabled[0]); ++i) {
        JSAtom atom = JS_NewAtom(ctx, disabled[i]);
        JS_DeleteProperty(ctx, global, atom, 0);
        JS_FreeAtom(ctx, atom);
    }
    JS_FreeValue(ctx, global);
#else
    /* Installed exactly once, from qjs_init. Anything non-deterministic that
       can be replaced in JS goes in this patch; Math.random is installed
       natively below because its seed is host state, not contract state.
       For smart contracts, the host should seed per-transaction via
       qjs_set_seed; the default seed keeps things deterministic if it does
       not. */
    const char *patch =
        "(function() {"
        "  /* Date.now and new Date() return fixed epoch */"
        "  Date.now = function() { return 0; };"
        "  Date = (function(OrigDate) {"
        "    function FrozenDate() { return new OrigDate(0); }"
        "    FrozenDate.now = function() { return 0; };"
        "    FrozenDate.parse = OrigDate.parse;"
        "    FrozenDate.UTC = OrigDate.UTC;"
        "    FrozenDate.prototype = OrigDate.prototype;"
        "    return FrozenDate;"
        "  })(Date);"
        "})();";

    JSValue r = JS_Eval(ctx, patch, strlen(patch), "<sandbox>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(ctx, r);

    /* Math.random is native and reads the seed cell — installed once. */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue math = JS_GetPropertyStr(ctx, global, "Math");
    JS_SetPropertyStr(ctx, math, "random",
        JS_NewCFunction(ctx, js_prng_random, "random", 0));
    JS_FreeValue(ctx, math);
    JS_FreeValue(ctx, global);
#endif
}

/* ---- Coverage: per-instruction interrupt handler ---- */

#ifndef CONFIG_XAHAU_HOOK_PROVIDER
static int coverage_enabled = 0;

static int coverage_interrupt_handler(JSRuntime *rt, void *opaque)
{
    JSContext *ctx = (JSContext *)opaque;
    const char *filename = NULL;
    int line = JS_GetCurrentLineNumber(ctx, &filename);
    if (line > 0 && filename) {
        host_coverage_hit(filename, strlen(filename), (uint32_t)line);
        JS_FreeCString(ctx, filename);
    }
    return 0; /* 0 = continue execution */
}
#endif

/* ---- Register all host functions on the JS global object ---- */

static void register_host_functions(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);

    JSValue lifecycle = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, lifecycle, "account",
        JS_NewCFunction(ctx, js_lifecycle_account, "account", 0));
    JS_SetPropertyStr(ctx, global, "lifecycle", lifecycle);
    JS_SetPropertyStr(ctx, global, "accept",
        JS_NewCFunction(ctx, js_hook_accept, "accept", 2));
    JSValue rollback = JS_NewCFunction(ctx, js_hook_rollback, "rollback", 2);
    JS_SetPropertyStr(ctx, rollback, "onHostFailure",
        JS_NewCFunction(
            ctx, js_rollback_on_host_failure, "onHostFailure", 2));
    JS_SetPropertyStr(ctx, global, "rollback", rollback);

    JSValue ledger = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, ledger, js_ledger_properties,
        sizeof(js_ledger_properties) / sizeof(js_ledger_properties[0]));
    JS_SetPropertyStr(ctx, global, "ledger", ledger);

    JSValue otxn = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, otxn, "type",
        JS_NewCFunction(ctx, js_otxn_type, "type", 0));
    JS_SetPropertyStr(ctx, global, "otxn", otxn);

    JSValue state = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, state, "get",
        JS_NewCFunction(ctx, js_state_get, "get", 1));
    JS_SetPropertyStr(ctx, state, "set",
        JS_NewCFunction(ctx, js_state_set, "set", 2));
    JS_SetPropertyStr(ctx, global, "state", state);

    JSValue emit = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, emit, "reserve",
        JS_NewCFunction(ctx, js_emit_reserve, "reserve", 1));
    JS_SetPropertyStr(ctx, emit, "prepare",
        JS_NewCFunction(ctx, js_emit_prepare, "prepare", 1));
    JS_SetPropertyStr(ctx, emit, "tx",
        JS_NewCFunction(ctx, js_emit_tx, "tx", 1));
    JS_SetPropertyStr(ctx, global, "emit", emit);

    JS_SetPropertyStr(ctx, global, "trace",
        JS_NewCFunction(ctx, js_trace, "trace", 2));

#ifndef CONFIG_XAHAU_HOOK_PROVIDER
    JS_SetPropertyStr(ctx, global, "host_log",
        JS_NewCFunction(ctx, js_host_log, "host_log", 1));
    JS_SetPropertyStr(ctx, global, "get_balance",
        JS_NewCFunction(ctx, js_host_get_balance, "get_balance", 1));
    JS_SetPropertyStr(ctx, global, "get_state",
        JS_NewCFunction(ctx, js_host_get_state, "get_state", 1));
    JS_SetPropertyStr(ctx, global, "get_tx_info",
        JS_NewCFunction(ctx, js_host_get_tx_info, "get_tx_info", 0));

    /* Crypto utils */
    JS_SetPropertyStr(ctx, global, "util_sha256",
        JS_NewCFunction(ctx, js_util_sha256, "util_sha256", 1));
    JS_SetPropertyStr(ctx, global, "util_sha512h",
        JS_NewCFunction(ctx, js_util_sha512h, "util_sha512h", 1));
    JS_SetPropertyStr(ctx, global, "util_ripemd160",
        JS_NewCFunction(ctx, js_util_ripemd160, "util_ripemd160", 1));
#endif

    JS_FreeValue(ctx, global);
}

/* ---- Exported WASM API ---- */

static JSRuntime *rt = NULL;
static JSContext *ctx = NULL;

/* Buffer for returning result strings to the host */
/* RESULT_MAX — the consensus result cap (issue 0009, decided 2026-07-26).
 *
 * Host-visible ABI: qjs_get_result_ptr/len never expose more than this, and a
 * larger result fails loudly with a RangeError rather than being clamped. The
 * pre-M7 static slot clamped silently at 16KB, which handed the host
 * syntactically partial output with a success code; that is the bug this
 * replaced.
 *
 * Why 1 MiB, and why a cap at all:
 *   - This is NOT the primary memory bound. JS heap growth is already bounded
 *     by qjs_set_memory_limit, and isolation comes from one fresh Store per
 *     contract. The cap's job is narrower: never hand the host an unbounded
 *     read.
 *   - The result slot is the VALUE/STATUS channel, not the bulk channel. Bytes
 *     have a proper path (encode_object returns a Uint8Array the host reads
 *     from wasm memory directly). 1 MiB is far above any legitimate status,
 *     hash, error, or decoded-object JSON.
 *   - Deliberate consequence: hex of a maximum VL field (918744 bytes ->
 *     1,837,488 chars) exceeds this. That is the intended signal to use the
 *     bytes channel, not a bug to raise the cap for.
 *
 * FIXED, never host-configurable: a per-node knob would let two nodes disagree
 * about whether the same contract succeeded. Pinned on both sides of the
 * boundary by codec/xrpl/tests/test_provider_result_abi.py.
 */
enum { RESULT_MAX = 1048576 };
static char *result_buf = NULL;
static const char *result_static = NULL;
static uint32_t result_len = 0;

/* ---- On-demand module loader (resolves via host) ---- */

#ifndef CONFIG_XAHAU_HOOK_PROVIDER
static int is_hex_char(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* Normalize: validate "index:<64 hex>" format, strip prefix.
   Returns just the 64-char hex string as canonical module name. */
static char *module_normalize(JSContext *ctx, const char *base_name,
                               const char *name, void *opaque)
{
    /* Must start with "index:" */
    if (strncmp(name, "index:", 6) != 0) {
        JS_ThrowTypeError(ctx,
            "module specifier must be \"index:<64 hex chars>\", got \"%s\"", name);
        return NULL;
    }
    const char *hex = name + 6;
    if (strlen(hex) != 64) {
        JS_ThrowTypeError(ctx,
            "module index must be exactly 64 hex chars (32 bytes), got %zu", strlen(hex));
        return NULL;
    }
    for (int i = 0; i < 64; i++) {
        if (!is_hex_char(hex[i])) {
            JS_ThrowTypeError(ctx,
                "invalid hex char '%c' at position %d in module index", hex[i], i);
            return NULL;
        }
    }
    /* Return just the hex part — this is the cache key */
    return js_strdup(ctx, hex);
}

static JSModuleDef *wasm_module_loader(JSContext *ctx, const char *name,
                                        void *opaque)
{
    /* Ask the host for the module content.
       The host returns either:
       - JS source code (compiled here as module)
       - Precompiled bytecode (loaded directly)
       Detected by first byte: bytecode starts with 0x02 (QJS bytecode magic). */
    int32_t len = host_resolve_module(name, strlen(name), module_loader_buf,
                                      sizeof(module_loader_buf));
    if (len <= 0) {
        JS_ThrowReferenceError(ctx, "could not resolve module '%s'", name);
        return NULL;
    }

    JSValue func;
    if (len >= 1 && module_loader_buf[0] == 0x02) {
        /* Precompiled bytecode (starts with QJS bytecode header) */
        func = JS_ReadObject(ctx, module_loader_buf, (size_t)len,
                             JS_READ_OBJ_BYTECODE);
    } else {
        /* JS source — compile as module */
        func = JS_Eval(ctx, (const char *)module_loader_buf, (size_t)len, name,
                        JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    }

    if (JS_IsException(func))
        return NULL;

    JSModuleDef *m = JS_VALUE_GET_PTR(func);
    return m;
}
#endif

__attribute__((export_name("qjs_init")))
void qjs_init(void)
{
    if (rt) return; /* already initialized (e.g. by Wizer snapshot) */
    rt = JS_NewRuntime();
#ifdef CONFIG_XAHAU_HOOK_PROVIDER
    ctx = JS_NewContextRaw(rt);
    if (!ctx ||
        JS_AddIntrinsicBaseObjects(ctx) ||
        JS_AddIntrinsicDate(ctx) ||
        /* The host compiler and ES-module evaluator use the Eval intrinsic.
           Dynamic code is deterministic and remains metered in profile v1;
           removing it is a separate language-policy choice. */
        JS_AddIntrinsicEval(ctx) ||
        JS_AddIntrinsicStringNormalize(ctx) ||
        JS_AddIntrinsicRegExp(ctx) ||
        JS_AddIntrinsicJSON(ctx) ||
        JS_AddIntrinsicProxy(ctx) ||
        JS_AddIntrinsicMapSet(ctx) ||
        JS_AddIntrinsicTypedArrays(ctx) ||
        /* QuickJS's ES-module evaluator uses the Promise machinery even for
           synchronous modules.  Hook entry points remain synchronous; the
           profile does not drain contract-scheduled jobs after termination. */
        JS_AddIntrinsicPromise(ctx)) {
        if (ctx) JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        ctx = NULL;
        rt = NULL;
        return;
    }
#else
    ctx = JS_NewContext(rt);
#endif
#ifndef CONFIG_XAHAU_HOOK_PROVIDER
    JS_SetModuleLoaderFunc(rt, module_normalize, wasm_module_loader, NULL);
#endif
    register_host_functions(ctx);
    register_cpp_types(ctx);
#ifdef CONFIG_PROTOCOL_XDATA
    register_protocol_functions(ctx);
#endif
    sandbox_make_deterministic(ctx);
}

#ifdef CONFIG_XAHAU_HOOK_PROVIDER
/* JavaScript Date is Unix milliseconds.  ledger_last_time is the prior ledger
   close time in Ripple-epoch seconds (2000-01-01T00:00:00Z). */
int64_t qjs_hook_date_now(void)
{
    static const int64_t ripple_epoch_unix_seconds = 946684800;
    int64_t const ledger_seconds = hook_ledger_last_time();
    if (ledger_seconds < 0 ||
        ledger_seconds > INT64_MAX / 1000 - ripple_epoch_unix_seconds)
        return 0;
    return (ledger_seconds + ripple_epoch_unix_seconds) * 1000;
}
#endif

/* Wizer pre-initialization entry point.
   Called at build time to snapshot the initialized QuickJS state. */
__attribute__((export_name("wizer.initialize")))
void wizer_initialize(void)
{
    qjs_init();
}

__attribute__((export_name("qjs_set_seed")))
void qjs_set_seed(uint32_t seed)
{
    /* Write the native seed cell. No re-eval: the sandbox patch is installed
       once at init, so seeding no longer re-wraps Date (issue 0030). */
    prng_seed = seed & 0x7fffffffu;
}

__attribute__((export_name("qjs_set_memory_limit")))
void qjs_set_memory_limit(uint32_t limit_bytes)
{
    JS_SetMemoryLimit(rt, limit_bytes);
}

__attribute__((export_name("qjs_set_max_stack_size")))
void qjs_set_max_stack_size(uint32_t stack_bytes)
{
    JS_SetMaxStackSize(rt, stack_bytes);
}

__attribute__((export_name("qjs_enable_coverage")))
void qjs_enable_coverage(int32_t enable)
{
#ifdef CONFIG_XAHAU_HOOK_PROVIDER
    (void)enable;
#else
    coverage_enabled = enable;
    if (enable) {
        /* fire interrupt handler every instruction.
           Set both the reload value AND the current counter so it
           fires immediately (not after 10000 more instructions). */
        JS_SetInterruptCounterInit(rt, 1);
        JS_SetInterruptHandler(rt, coverage_interrupt_handler, ctx);
    } else {
        /* restore default */
        JS_SetInterruptCounterInit(rt, 10000);
        JS_SetInterruptHandler(rt, NULL, NULL);
    }
#endif
}

/* ---- Helpers ---- */

static void clear_result(void)
{
    if (result_buf) {
        free(result_buf);
        result_buf = NULL;
    }
    result_static = NULL;
    result_len = 0;
}

static void store_static_result(const char *str)
{
    clear_result();
    result_static = str;
    result_len = (uint32_t)strlen(str);
}

static int store_result_str(const char *str)
{
    size_t len = strlen(str);

    if (len > RESULT_MAX) {
        store_static_result("RangeError: result exceeds 1048576 byte RESULT_MAX");
        return -1;
    }

    char *buf = (char *)malloc(len + 1);
    if (!buf) {
        store_static_result("Error: out of memory storing result");
        return -1;
    }

    clear_result();
    memcpy(buf, str, len);
    buf[len] = '\0';
    result_buf = buf;
    result_len = (uint32_t)len;
    return 0;
}

static int store_exception(void)
{
    JSValue exc = JS_GetException(ctx);
    const char *str = JS_ToCString(ctx, exc);
    int status = 0;
    if (str) {
        status = store_result_str(str);
        JS_FreeCString(ctx, str);
    }
    JS_FreeValue(ctx, exc);
    return status;
}

static int store_value(JSValue val)
{
    const char *str = JS_ToCString(ctx, val);
    if (str) {
        int status = store_result_str(str);
        JS_FreeCString(ctx, str);
        return status;
    }
    return 0;
}

/* ---- Eval / Compile / Load bytecode ---- */

__attribute__((export_name("qjs_eval")))
int32_t qjs_eval(const char *input, uint32_t input_len)
{
    clear_result();

    JSValue val = JS_Eval(ctx, input, input_len, "<contract>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(val)) {
        store_exception();
        JS_FreeValue(ctx, val);
        return -1;
    }
    int status = store_value(val);
    JS_FreeValue(ctx, val);
    return status;
}

__attribute__((export_name("qjs_eval_module")))
int32_t qjs_eval_module(const char *input, uint32_t input_len)
{
    clear_result();

    JSValue val = JS_Eval(ctx, input, input_len, "<contract>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) {
        store_exception();
        JS_FreeValue(ctx, val);
        return -1;
    }
    int status = store_value(val);
    JS_FreeValue(ctx, val);
    return status;
}

/* Compile JS source to bytecode. Returns bytecode size, or -1 on error.
   The bytecode is stored in an internal buffer — retrieve with
   qjs_get_bytecode_ptr/len, then copy it out before next call. */
static uint8_t *bytecode_buf = NULL;
static uint32_t bytecode_len = 0;

static int32_t compile_source(
    const char *input, uint32_t input_len, int eval_type)
{
    clear_result();
    if (bytecode_buf) { js_free(ctx, bytecode_buf); bytecode_buf = NULL; }
    bytecode_len = 0;

    JSValue obj = JS_Eval(
        ctx,
        input,
        input_len,
        "<contract>",
        eval_type | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(obj)) {
        store_exception();
        JS_FreeValue(ctx, obj);
        return -1;
    }

    size_t out_size;
    bytecode_buf = JS_WriteObject(ctx, &out_size, obj, JS_WRITE_OBJ_BYTECODE);
    JS_FreeValue(ctx, obj);

    if (!bytecode_buf) {
        store_exception();
        return -1;
    }

    bytecode_len = (uint32_t)out_size;
    return (int32_t)bytecode_len;
}

__attribute__((export_name("qjs_compile")))
int32_t qjs_compile(const char *input, uint32_t input_len)
{
    return compile_source(input, input_len, JS_EVAL_TYPE_GLOBAL);
}

__attribute__((export_name("qjs_compile_module")))
int32_t qjs_compile_module(const char *input, uint32_t input_len)
{
    return compile_source(input, input_len, JS_EVAL_TYPE_MODULE);
}

__attribute__((export_name("qjs_get_bytecode_ptr")))
const uint8_t *qjs_get_bytecode_ptr(void)
{
    return bytecode_buf;
}

__attribute__((export_name("qjs_get_bytecode_len")))
uint32_t qjs_get_bytecode_len(void)
{
    return bytecode_len;
}

/* Load and execute precompiled bytecode. */
__attribute__((export_name("qjs_eval_bytecode")))
int32_t qjs_eval_bytecode(const uint8_t *buf, uint32_t buf_len)
{
    clear_result();

    JSValue obj = JS_ReadObject(ctx, buf, buf_len, JS_READ_OBJ_BYTECODE);
    if (JS_IsException(obj)) {
        store_exception();
        JS_FreeValue(ctx, obj);
        return -1;
    }

    JSValue val = JS_EvalFunction(ctx, obj);
    /* JS_EvalFunction frees obj */
    if (JS_IsException(val)) {
        store_exception();
        JS_FreeValue(ctx, val);
        return -1;
    }
    int status = store_value(val);
    JS_FreeValue(ctx, val);
    return status;
}

static int finish_module_evaluation(JSValue evaluated)
{
    if (JS_IsException(evaluated)) {
        store_exception();
        JS_FreeValue(ctx, evaluated);
        return -1;
    }

    int promise_state = JS_PromiseState(ctx, evaluated);
    if (promise_state == JS_PROMISE_REJECTED) {
        JSValue reason = JS_PromiseResult(ctx, evaluated);
        JS_Throw(ctx, reason); /* transfers reason ownership */
        store_exception();
        JS_FreeValue(ctx, evaluated);
        return -1;
    }
    if (promise_state == JS_PROMISE_PENDING) {
        JS_FreeValue(ctx, evaluated);
        store_static_result(
            "TypeError: pending module initialization is not supported");
        return -1;
    }

    JS_FreeValue(ctx, evaluated);
    return 0;
}

static int32_t qjs_invoke_bytecode_export(
    const uint8_t *buf,
    uint32_t buf_len,
    const char *export_name,
    uint32_t reserved)
{
    clear_result();

    JSValue obj = JS_ReadObject(ctx, buf, buf_len, JS_READ_OBJ_BYTECODE);
    if (JS_IsException(obj)) {
        store_exception();
        JS_FreeValue(ctx, obj);
        return -1;
    }
    if (JS_VALUE_GET_TAG(obj) != JS_TAG_MODULE) {
        JS_FreeValue(ctx, obj);
        store_static_result("TypeError: Hook bytecode must contain an ES module");
        return -1;
    }

    JSModuleDef *module = JS_VALUE_GET_PTR(obj);
    if (JS_ResolveModule(ctx, obj) < 0) {
        store_exception();
        JS_FreeValue(ctx, obj);
        return -1;
    }

    JSValue evaluated = JS_EvalFunction(ctx, obj);
    /* JS_EvalFunction frees obj. The loaded module retains module. */
    if (finish_module_evaluation(evaluated) < 0)
        return -1;

    JSValue module_namespace = JS_GetModuleNamespace(ctx, module);
    if (JS_IsException(module_namespace)) {
        store_exception();
        return -1;
    }
    JSValue entry = JS_GetPropertyStr(ctx, module_namespace, export_name);
    JS_FreeValue(ctx, module_namespace);
    if (JS_IsException(entry)) {
        store_exception();
        JS_FreeValue(ctx, entry);
        return -1;
    }
    if (JS_IsUndefined(entry)) {
        JS_FreeValue(ctx, entry);
        if (export_name[0] == 'c')
            store_static_result(
                "TypeError: Hook module has no exported cbak entry point");
        else
            store_static_result(
                "TypeError: Hook module has no exported hook entry point");
        return -1;
    }
    if (!JS_IsFunction(ctx, entry)) {
        JS_FreeValue(ctx, entry);
        if (export_name[0] == 'c')
            store_static_result(
                "TypeError: exported cbak entry point is not a function");
        else
            store_static_result(
                "TypeError: exported hook entry point is not a function");
        return -1;
    }

    JSValue argument = JS_NewInt64(ctx, reserved);
    JSValue result = JS_Call(ctx, entry, JS_UNDEFINED, 1, &argument);
    JS_FreeValue(ctx, argument);
    JS_FreeValue(ctx, entry);
    if (JS_IsException(result)) {
        store_exception();
        JS_FreeValue(ctx, result);
        return -1;
    }
    int status = store_value(result);
    JS_FreeValue(ctx, result);
    return status;
}

__attribute__((export_name("qjs_hook")))
int32_t qjs_hook(const uint8_t *buf, uint32_t buf_len, uint32_t reserved)
{
    return qjs_invoke_bytecode_export(buf, buf_len, "hook", reserved);
}

__attribute__((export_name("qjs_cbak")))
int32_t qjs_cbak(const uint8_t *buf, uint32_t buf_len, uint32_t reserved)
{
    return qjs_invoke_bytecode_export(buf, buf_len, "cbak", reserved);
}

/* Evaluate module initialization in the caller's disposable validation
   instance, but do not invoke either entry point. Result bits:
   1 = callable hook export, 2 = callable cbak export. */
__attribute__((export_name("qjs_validate_hook_module")))
int32_t qjs_validate_hook_module(const uint8_t *buf, uint32_t buf_len)
{
    clear_result();

    JSValue obj = JS_ReadObject(ctx, buf, buf_len, JS_READ_OBJ_BYTECODE);
    if (JS_IsException(obj)) {
        store_exception();
        JS_FreeValue(ctx, obj);
        return -1;
    }
    if (JS_VALUE_GET_TAG(obj) != JS_TAG_MODULE) {
        JS_FreeValue(ctx, obj);
        store_static_result("TypeError: Hook bytecode must contain an ES module");
        return -1;
    }

    JSModuleDef *module = JS_VALUE_GET_PTR(obj);
    if (JS_ResolveModule(ctx, obj) < 0) {
        store_exception();
        JS_FreeValue(ctx, obj);
        return -1;
    }

    JSValue evaluated = JS_EvalFunction(ctx, obj);
    /* JS_EvalFunction frees obj. The loaded module retains module. */
    if (finish_module_evaluation(evaluated) < 0)
        return -1;

    JSValue module_namespace = JS_GetModuleNamespace(ctx, module);
    if (JS_IsException(module_namespace)) {
        store_exception();
        return -1;
    }
    JSValue hook = JS_GetPropertyStr(ctx, module_namespace, "hook");
    JSValue cbak = JS_GetPropertyStr(ctx, module_namespace, "cbak");
    JS_FreeValue(ctx, module_namespace);
    if (JS_IsException(hook) || JS_IsException(cbak)) {
        store_exception();
        JS_FreeValue(ctx, hook);
        JS_FreeValue(ctx, cbak);
        return -1;
    }
    if (JS_IsUndefined(hook)) {
        JS_FreeValue(ctx, hook);
        JS_FreeValue(ctx, cbak);
        store_static_result(
            "TypeError: Hook module has no exported hook entry point");
        return -1;
    }
    if (!JS_IsFunction(ctx, hook)) {
        JS_FreeValue(ctx, hook);
        JS_FreeValue(ctx, cbak);
        store_static_result(
            "TypeError: exported hook entry point is not a function");
        return -1;
    }
    if (!JS_IsUndefined(cbak) && !JS_IsFunction(ctx, cbak)) {
        JS_FreeValue(ctx, hook);
        JS_FreeValue(ctx, cbak);
        store_static_result(
            "TypeError: exported cbak entry point is not a function");
        return -1;
    }
    int flags = 1 | (!JS_IsUndefined(cbak) ? 2 : 0);
    JS_FreeValue(ctx, hook);
    JS_FreeValue(ctx, cbak);
    return flags;
}

__attribute__((export_name("qjs_get_result_ptr")))
const char *qjs_get_result_ptr(void)
{
    if (result_buf)
        return result_buf;
    if (result_static)
        return result_static;
    return "";
}

__attribute__((export_name("qjs_get_result_len")))
uint32_t qjs_get_result_len(void)
{
    return result_len;
}

__attribute__((export_name("qjs_destroy")))
void qjs_destroy(void)
{
    clear_result();
    if (ctx) { JS_FreeContext(ctx); ctx = NULL; }
    if (rt) { JS_FreeRuntime(rt); rt = NULL; }
}

/* wasi-sdk reactor CRT provides _initialize automatically */
