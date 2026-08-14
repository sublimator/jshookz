/*
 * types_js.cpp - Expose C++ types to JavaScript via QuickJS
 *
 * This is compiled with clang++ into the WASM module alongside
 * QuickJS (which is C). The C++ types in types.hpp get QuickJS
 * class wrappers here, making them available as JS objects.
 *
 * Example JS usage:
 *   let h = Hash256.fromHex("AABB...");
 *   h.toHex()  // "AABB..."
 *   h.isZero() // false
 *
 *   let a = Amount.xrp(1000000);  // 1 XRP in drops
 *   a.isXRP()   // true
 *   a.drops()   // 1000000
 *
 *   let id = AccountID.fromHex("...");
 */

#include "types.hpp"
#include "quickjs.hpp"

#include <cstdio>
#include <cstring>

using namespace hook;
namespace qjs = jshookz::provider::qjs;

// Create a Uint8Array from raw bytes
static JSValue new_uint8array(JSContext *ctx, const uint8_t *data, size_t len) {
    JSValue ab = JS_NewArrayBufferCopy(ctx, data, len);
    if (JS_IsException(ab)) return ab;
    JSValue args[] = { ab, JS_NewInt32(ctx, 0), JS_NewInt32(ctx, (int32_t)len) };
    JSValue ta = JS_NewTypedArray(ctx, 3, args, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, args[2]);
    return ta;
}

// --- Class IDs (registered with QuickJS) ---

static JSClassID js_hash256_class_id;
static JSClassID js_accountid_class_id;
static JSClassID js_xfl_class_id;
static JSClassID js_amount_class_id;
static JSClassID js_keylet_class_id;
static JSClassID js_blob_class_id;

// --- STBlob ---

struct JSBlob {
    uint8_t *data = nullptr;
    size_t len = 0;
};

static void js_blob_finalizer(JSRuntime *rt, JSValue val) {
    auto *blob = (JSBlob *)JS_GetOpaque(val, js_blob_class_id);
    if (!blob) return;
    if (blob->data) js_free_rt(rt, blob->data);
    js_free_rt(rt, blob);
}

static JSClassDef js_blob_class = {
    "STBlob",
    .finalizer = js_blob_finalizer,
};

static JSValue js_blob_new(JSContext *ctx, const uint8_t *data, size_t len) {
    auto *blob = (JSBlob *)js_mallocz(ctx, sizeof(JSBlob));
    if (!blob) return JS_ThrowOutOfMemory(ctx);
    if (len != 0) {
        blob->data = (uint8_t *)js_malloc(ctx, len);
        if (!blob->data) {
            js_free(ctx, blob);
            return JS_ThrowOutOfMemory(ctx);
        }
        std::memcpy(blob->data, data, len);
    }
    blob->len = len;

    JSValue obj = JS_NewObjectClass(ctx, js_blob_class_id);
    if (JS_IsException(obj)) {
        if (blob->data) js_free(ctx, blob->data);
        js_free(ctx, blob);
        return obj;
    }
    JS_SetOpaque(obj, blob);
    return obj;
}

static JSValue js_blob_from(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "STBlob.from() expects a byte value");
    auto bytes = qjs::ByteView::get(
        ctx, argv[0], qjs::StringBytes::hex);
    if (!bytes)
        return JS_ThrowTypeError(
            ctx, "STBlob.from() expects Uint8Array, ArrayBuffer, or hex string");
    return js_blob_new(ctx, bytes.data(), bytes.size());
}

static JSValue js_blob_byte_length(JSContext *ctx, JSValueConst this_val)
{
    auto *blob = (JSBlob *)JS_GetOpaque2(ctx, this_val, js_blob_class_id);
    if (!blob) return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)blob->len);
}

static JSValue js_blob_to_bytes(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    auto *blob = (JSBlob *)JS_GetOpaque2(ctx, this_val, js_blob_class_id);
    if (!blob) return JS_EXCEPTION;
    return new_uint8array(ctx, blob->data, blob->len);
}

static JSValue js_blob_to_hex(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    auto *blob = (JSBlob *)JS_GetOpaque2(ctx, this_val, js_blob_class_id);
    if (!blob) return JS_EXCEPTION;
    static const char hex[] = "0123456789ABCDEF";
    char *encoded = (char *)js_malloc(ctx, blob->len * 2 + 1);
    if (!encoded) return JS_ThrowOutOfMemory(ctx);
    for (size_t i = 0; i < blob->len; ++i) {
        encoded[i * 2] = hex[blob->data[i] >> 4];
        encoded[i * 2 + 1] = hex[blob->data[i] & 0x0F];
    }
    encoded[blob->len * 2] = '\0';
    JSValue result = JS_NewStringLen(ctx, encoded, blob->len * 2);
    js_free(ctx, encoded);
    return result;
}

static JSValue js_blob_byte_at(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    auto *blob = (JSBlob *)JS_GetOpaque2(ctx, this_val, js_blob_class_id);
    if (!blob) return JS_EXCEPTION;
    int64_t index;
    if (argc < 1 || JS_ToInt64(ctx, &index, argv[0]) < 0)
        return JS_EXCEPTION;
    if (index < 0 || (uint64_t)index >= blob->len)
        return JS_ThrowRangeError(ctx, "STBlob.byteAt(): index out of range");
    return JS_NewInt32(ctx, blob->data[index]);
}

static JSValue js_blob_equals(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    auto *blob = (JSBlob *)JS_GetOpaque2(ctx, this_val, js_blob_class_id);
    if (!blob) return JS_EXCEPTION;
    if (argc < 1) return JS_FALSE;
    auto other = qjs::ByteView::get(
        ctx, argv[0], qjs::StringBytes::hex);
    if (!other) return JS_FALSE;
    bool equal = blob->len == other.size() &&
        (blob->len == 0 ||
         std::memcmp(blob->data, other.data(), blob->len) == 0);
    return JS_NewBool(ctx, equal);
}

static const JSCFunctionListEntry js_blob_proto_funcs[] = {
    JS_CGETSET_DEF("byteLength", js_blob_byte_length, NULL),
    JS_CFUNC_DEF("byteAt", 1, js_blob_byte_at),
    JS_CFUNC_DEF("toBytes", 0, js_blob_to_bytes),
    JS_CFUNC_DEF("toHex", 0, js_blob_to_hex),
    JS_CFUNC_DEF("equals", 1, js_blob_equals),
};

static const JSCFunctionListEntry js_blob_static_funcs[] = {
    JS_CFUNC_DEF("from", 1, js_blob_from),
};

// --- Hash256 ---

static void js_hash256_finalizer(JSRuntime *rt, JSValue val) {
    auto *h = (Hash256 *)JS_GetOpaque(val, js_hash256_class_id);
    if (h) js_free_rt(rt, h);
}

static JSClassDef js_hash256_class = {
    "Hash256",
    .finalizer = js_hash256_finalizer,
};

// Hash256.from(BytesLike) — accepts Uint8Array, ArrayBuffer, or hex string
static JSValue js_hash256_from(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    auto bytes = qjs::ByteView::get(
        ctx, argv[0], qjs::StringBytes::hex);
    if (!bytes) {
        return JS_ThrowTypeError(ctx, "Hash256.from() expects Uint8Array, ArrayBuffer, or hex string");
    }
    if (bytes.size() != 32) {
        return JS_ThrowTypeError(
            ctx,
            "Hash256.from() needs exactly 32 bytes (got %u)",
            bytes.size());
    }

    auto *h = (Hash256 *)js_mallocz(ctx, sizeof(Hash256));
    new (h) Hash256(bytes.data(), bytes.size());

    JSValue obj = JS_NewObjectClass(ctx, js_hash256_class_id);
    JS_SetOpaque(obj, h);
    return obj;
}

static JSValue js_hash256_to_hex(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    auto *h = (Hash256 *)JS_GetOpaque2(ctx, this_val, js_hash256_class_id);
    if (!h) return JS_EXCEPTION;

    char buf[65];
    h->to_hex(buf, sizeof(buf));
    buf[64] = '\0';
    return JS_NewString(ctx, buf);
}

static JSValue js_hash256_to_bytes(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    auto *h = (Hash256 *)JS_GetOpaque2(ctx, this_val, js_hash256_class_id);
    if (!h) return JS_EXCEPTION;
    return new_uint8array(ctx, h->data(), h->size());
}

static JSValue js_hash256_is_zero(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    auto *h = (Hash256 *)JS_GetOpaque2(ctx, this_val, js_hash256_class_id);
    if (!h) return JS_EXCEPTION;
    return JS_NewBool(ctx, h->is_zero());
}

static JSValue js_hash256_equals(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    auto *h1 = (Hash256 *)JS_GetOpaque2(ctx, this_val, js_hash256_class_id);
    auto *h2 = (Hash256 *)JS_GetOpaque2(ctx, argv[0], js_hash256_class_id);
    if (!h1 || !h2) return JS_EXCEPTION;
    return JS_NewBool(ctx, *h1 == *h2);
}

//@@impl STHash
static const JSCFunctionListEntry js_hash256_proto_funcs[] = {
    JS_CFUNC_DEF("toHex", 0, js_hash256_to_hex),
    JS_CFUNC_DEF("toBytes", 0, js_hash256_to_bytes),
    JS_CFUNC_DEF("isZero", 0, js_hash256_is_zero),
    JS_CFUNC_DEF("equals", 1, js_hash256_equals),
};

//@@impl STHash static
static const JSCFunctionListEntry js_hash256_static_funcs[] = {
    JS_CFUNC_DEF("from", 1, js_hash256_from),
};

// --- AccountID (inherits Hash160 behavior) ---

static void js_accountid_finalizer(JSRuntime *rt, JSValue val) {
    auto *a = (AccountID *)JS_GetOpaque(val, js_accountid_class_id);
    if (a) js_free_rt(rt, a);
}

static JSClassDef js_accountid_class = {
    "AccountID",
    .finalizer = js_accountid_finalizer,
};

// AccountID.from(BytesLike)
static JSValue js_accountid_from(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    auto bytes = qjs::ByteView::get(
        ctx, argv[0], qjs::StringBytes::hex);
    if (!bytes) {
        return JS_ThrowTypeError(ctx, "AccountID.from() expects Uint8Array, ArrayBuffer, or hex string");
    }
    if (bytes.size() != 20) {
        return JS_ThrowTypeError(
            ctx,
            "AccountID.from() needs exactly 20 bytes (got %u)",
            bytes.size());
    }

    auto *a = (AccountID *)js_mallocz(ctx, sizeof(AccountID));
    new (a) AccountID(bytes.data(), bytes.size());

    JSValue obj = JS_NewObjectClass(ctx, js_accountid_class_id);
    JS_SetOpaque(obj, a);
    return obj;
}

static JSValue js_accountid_to_hex(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    auto *a = (AccountID *)JS_GetOpaque2(ctx, this_val, js_accountid_class_id);
    if (!a) return JS_EXCEPTION;
    char buf[41];
    a->to_hex(buf, sizeof(buf));
    buf[40] = '\0';
    return JS_NewString(ctx, buf);
}

static JSValue js_accountid_to_bytes(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    auto *a = (AccountID *)JS_GetOpaque2(ctx, this_val, js_accountid_class_id);
    if (!a) return JS_EXCEPTION;
    return new_uint8array(ctx, a->data(), a->size());
}

//@@impl STAddress
static const JSCFunctionListEntry js_accountid_proto_funcs[] = {
    JS_CFUNC_DEF("toHex", 0, js_accountid_to_hex),
    JS_CFUNC_DEF("toBytes", 0, js_accountid_to_bytes),
};

//@@impl STAddress static
static const JSCFunctionListEntry js_accountid_static_funcs[] = {
    JS_CFUNC_DEF("from", 1, js_accountid_from),
};

// --- XFL ---

static void js_xfl_finalizer(JSRuntime *rt, JSValue val) {
    auto *x = (XFL *)JS_GetOpaque(val, js_xfl_class_id);
    if (x) js_free_rt(rt, x);
}

static JSClassDef js_xfl_class = {
    "XFL",
    .finalizer = js_xfl_finalizer,
};

static JSValue js_xfl_new(JSContext *ctx, const XFL &xfl) {
    auto *x = (XFL *)js_mallocz(ctx, sizeof(XFL));
    new (x) XFL(xfl);
    JSValue obj = JS_NewObjectClass(ctx, js_xfl_class_id);
    JS_SetOpaque(obj, x);
    return obj;
}

static JSValue js_xfl_from_raw(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    int64_t raw;
    if (JS_IsBigInt(ctx, argv[0])) {
        if (JS_ToBigInt64(ctx, &raw, argv[0])) return JS_EXCEPTION;
    } else {
        if (JS_ToInt64(ctx, &raw, argv[0])) return JS_EXCEPTION;
    }
    return js_xfl_new(ctx, XFL(raw));
}

static JSValue js_xfl_raw(JSContext *ctx, JSValueConst this_val)
{
    auto *x = (XFL *)JS_GetOpaque2(ctx, this_val, js_xfl_class_id);
    if (!x) return JS_EXCEPTION;
    return JS_NewBigInt64(ctx, x->raw());
}

static JSValue js_xfl_mantissa(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    auto *x = (XFL *)JS_GetOpaque2(ctx, this_val, js_xfl_class_id);
    if (!x) return JS_EXCEPTION;
    return JS_NewInt64(ctx, x->mantissa());
}

static JSValue js_xfl_exponent(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    auto *x = (XFL *)JS_GetOpaque2(ctx, this_val, js_xfl_class_id);
    if (!x) return JS_EXCEPTION;
    return JS_NewInt32(ctx, x->exponent());
}

static JSValue js_xfl_is_negative(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    auto *x = (XFL *)JS_GetOpaque2(ctx, this_val, js_xfl_class_id);
    if (!x) return JS_EXCEPTION;
    return JS_NewBool(ctx, x->is_negative());
}

static JSValue js_xfl_is_zero(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    auto *x = (XFL *)JS_GetOpaque2(ctx, this_val, js_xfl_class_id);
    if (!x) return JS_EXCEPTION;
    return JS_NewBool(ctx, x->is_zero());
}

//@@impl XFL
static const JSCFunctionListEntry js_xfl_proto_funcs[] = {
    JS_CGETSET_DEF("raw", js_xfl_raw, NULL),
    JS_CFUNC_DEF("mantissa", 0, js_xfl_mantissa),
    JS_CFUNC_DEF("exponent", 0, js_xfl_exponent),
    JS_CFUNC_DEF("isNegative", 0, js_xfl_is_negative),
    JS_CFUNC_DEF("isZero", 0, js_xfl_is_zero),
};

//@@impl XFL static
static const JSCFunctionListEntry js_xfl_static_funcs[] = {
    JS_CFUNC_DEF("fromRaw", 1, js_xfl_from_raw),
};

// --- Registration function (called from provider.cpp) ---

extern "C" void register_cpp_types(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue proto, ctor;

    // STBlob
    JS_NewClassID(&js_blob_class_id);
    JS_NewClass(JS_GetRuntime(ctx), js_blob_class_id, &js_blob_class);
    proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, js_blob_proto_funcs,
                               sizeof(js_blob_proto_funcs) / sizeof(js_blob_proto_funcs[0]));
    JS_SetClassProto(ctx, js_blob_class_id, proto);
    ctor = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, ctor, js_blob_static_funcs,
                               sizeof(js_blob_static_funcs) / sizeof(js_blob_static_funcs[0]));
    JS_SetPropertyStr(ctx, global, "STBlob", ctor);

    // Hash256
    JS_NewClassID(&js_hash256_class_id);
    JS_NewClass(JS_GetRuntime(ctx), js_hash256_class_id, &js_hash256_class);
    proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, js_hash256_proto_funcs,
                               sizeof(js_hash256_proto_funcs) / sizeof(js_hash256_proto_funcs[0]));
    JS_SetClassProto(ctx, js_hash256_class_id, proto);
    ctor = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, ctor, js_hash256_static_funcs,
                               sizeof(js_hash256_static_funcs) / sizeof(js_hash256_static_funcs[0]));
    JS_SetPropertyStr(ctx, global, "Hash256", ctor);

    // AccountID
    JS_NewClassID(&js_accountid_class_id);
    JS_NewClass(JS_GetRuntime(ctx), js_accountid_class_id, &js_accountid_class);
    proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, js_accountid_proto_funcs,
                               sizeof(js_accountid_proto_funcs) / sizeof(js_accountid_proto_funcs[0]));
    JS_SetClassProto(ctx, js_accountid_class_id, proto);
    ctor = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, ctor, js_accountid_static_funcs,
                               sizeof(js_accountid_static_funcs) / sizeof(js_accountid_static_funcs[0]));
    JS_SetPropertyStr(ctx, global, "AccountID", ctor);

    // XFL
    JS_NewClassID(&js_xfl_class_id);
    JS_NewClass(JS_GetRuntime(ctx), js_xfl_class_id, &js_xfl_class);
    proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, js_xfl_proto_funcs,
                               sizeof(js_xfl_proto_funcs) / sizeof(js_xfl_proto_funcs[0]));
    JS_SetClassProto(ctx, js_xfl_class_id, proto);
    ctor = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, ctor, js_xfl_static_funcs,
                               sizeof(js_xfl_static_funcs) / sizeof(js_xfl_static_funcs[0]));
    JS_SetPropertyStr(ctx, global, "XFL", ctor);

    JS_FreeValue(ctx, global);
}
