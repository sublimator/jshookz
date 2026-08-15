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
#include <new>
#include <utility>

using namespace hook;
namespace qjs = jshookz::provider::qjs;

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
    auto *blob = static_cast<JSBlob *>(JS_GetOpaque(val, js_blob_class_id));
    if (!blob) return;
    if (blob->data) js_free_rt(rt, blob->data);
    js_free_rt(rt, blob);
}

static JSClassDef js_blob_class = {
    "STBlob",
    .finalizer = js_blob_finalizer,
};

template <class T, class... Args>
[[nodiscard]] static JSValue
js_native_new(JSContext* ctx, JSClassID class_id, Args&&... args)
{
    void* storage = js_mallocz(ctx, sizeof(T));
    if (!storage)
        return JS_ThrowOutOfMemory(ctx);
    T* value = new (storage) T(std::forward<Args>(args)...);
    JSValue object = JS_NewObjectClass(ctx, class_id);
    if (JS_IsException(object)) {
        value->~T();
        js_free(ctx, storage);
        return object;
    }
    JS_SetOpaque(object, value);
    return object;
}

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
        ctx, argv[0], qjs::BytePolicy::bytesLike);
    if (!bytes)
        return qjs::byteInputTypeError(
            ctx, "STBlob.from()", qjs::BytePolicy::bytesLike);
    return js_blob_new(ctx, bytes.data(), bytes.size());
}

static JSValue js_blob_from_hex(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "STBlob.fromHex() expects a hex string");
    auto bytes = qjs::ByteView::get(
        ctx, argv[0], qjs::BytePolicy::hexString);
    if (!bytes)
        return qjs::byteInputTypeError(
            ctx, "STBlob.fromHex()", qjs::BytePolicy::hexString);
    return js_blob_new(ctx, bytes.data(), bytes.size());
}

static JSValue js_blob_byte_length(JSContext *ctx, JSValueConst this_val)
{
    auto *blob = qjs::opaque<JSBlob>(ctx, this_val, js_blob_class_id);
    if (!blob) return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)blob->len);
}

static JSValue js_blob_to_bytes(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    auto *blob = qjs::opaque<JSBlob>(ctx, this_val, js_blob_class_id);
    if (!blob) return JS_EXCEPTION;
    return qjs::uint8Array(
        ctx, std::span<std::uint8_t const>{blob->data, blob->len});
}

static JSValue js_blob_to_hex(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    auto *blob = qjs::opaque<JSBlob>(ctx, this_val, js_blob_class_id);
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
    auto *blob = qjs::opaque<JSBlob>(ctx, this_val, js_blob_class_id);
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
    auto *blob = qjs::opaque<JSBlob>(ctx, this_val, js_blob_class_id);
    if (!blob) return JS_EXCEPTION;
    if (argc < 1) return JS_FALSE;
    auto other = qjs::ByteView::get(
        ctx, argv[0], qjs::BytePolicy::bytesLikeOrSTBlob);
    if (!other)
        return JS_HasException(ctx) ? JS_EXCEPTION : JS_FALSE;
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
    JS_CFUNC_DEF("fromHex", 1, js_blob_from_hex),
};

// --- Hash256 ---

static void js_hash256_finalizer(JSRuntime *rt, JSValue val) {
    qjs::destroyOpaque<Hash256>(rt, val, js_hash256_class_id);
}

static JSClassDef js_hash256_class = {
    "Hash256",
    .finalizer = js_hash256_finalizer,
};

// Hash256.from(BytesLike) — shared byte inputs, with strings decoded as hex
static JSValue js_hash256_from(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Hash256.from() expects a byte value");
    auto bytes = qjs::ByteView::get(
        ctx, argv[0], qjs::BytePolicy::bytesLike);
    if (!bytes) {
        return qjs::byteInputTypeError(
            ctx, "Hash256.from()", qjs::BytePolicy::bytesLike);
    }
    if (bytes.size() != 32) {
        return JS_ThrowTypeError(
            ctx,
            "Hash256.from() needs exactly 32 bytes (got %u)",
            bytes.size());
    }

    return js_native_new<Hash256>(
        ctx, js_hash256_class_id, bytes.data(), bytes.size());
}

static JSValue js_hash256_from_hex(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "Hash256.fromHex() expects a hex string");
    auto bytes = qjs::ByteView::get(
        ctx, argv[0], qjs::BytePolicy::hexString);
    if (!bytes)
        return qjs::byteInputTypeError(
            ctx, "Hash256.fromHex()", qjs::BytePolicy::hexString);
    if (bytes.size() != 32)
        return JS_ThrowTypeError(
            ctx, "Hash256.fromHex() needs exactly 32 bytes (got %u)",
            bytes.size());
    return js_native_new<Hash256>(
        ctx, js_hash256_class_id, bytes.data(), bytes.size());
}

static JSValue js_hash256_to_hex(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    auto *h = qjs::opaque<Hash256>(ctx, this_val, js_hash256_class_id);
    if (!h) return JS_EXCEPTION;

    char buf[65];
    h->to_hex(buf, sizeof(buf));
    buf[64] = '\0';
    return JS_NewString(ctx, buf);
}

static JSValue js_hash256_to_bytes(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    auto *h = qjs::opaque<Hash256>(ctx, this_val, js_hash256_class_id);
    if (!h) return JS_EXCEPTION;
    return qjs::uint8Array(
        ctx, std::span<std::uint8_t const>{h->data(), h->size()});
}

static JSValue js_hash256_is_zero(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    auto *h = qjs::opaque<Hash256>(ctx, this_val, js_hash256_class_id);
    if (!h) return JS_EXCEPTION;
    return JS_NewBool(ctx, h->is_zero());
}

static JSValue js_hash256_equals(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    auto *h1 = qjs::opaque<Hash256>(ctx, this_val, js_hash256_class_id);
    auto *h2 = qjs::opaque<Hash256>(ctx, argv[0], js_hash256_class_id);
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
    JS_CFUNC_DEF("fromHex", 1, js_hash256_from_hex),
};

// --- AccountID (inherits Hash160 behavior) ---

static void js_accountid_finalizer(JSRuntime *rt, JSValue val) {
    qjs::destroyOpaque<AccountID>(rt, val, js_accountid_class_id);
}

static JSClassDef js_accountid_class = {
    "AccountID",
    .finalizer = js_accountid_finalizer,
};

// AccountID.from(BytesLike)
static JSValue js_accountid_from(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "AccountID.from() expects a byte value");
    auto bytes = qjs::ByteView::get(
        ctx, argv[0], qjs::BytePolicy::bytesLike);
    if (!bytes) {
        return qjs::byteInputTypeError(
            ctx, "AccountID.from()", qjs::BytePolicy::bytesLike);
    }
    if (bytes.size() != 20) {
        return JS_ThrowTypeError(
            ctx,
            "AccountID.from() needs exactly 20 bytes (got %u)",
            bytes.size());
    }

    return js_native_new<AccountID>(
        ctx, js_accountid_class_id, bytes.data(), bytes.size());
}

static JSValue js_accountid_from_hex(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "AccountID.fromHex() expects a hex string");
    auto bytes = qjs::ByteView::get(
        ctx, argv[0], qjs::BytePolicy::hexString);
    if (!bytes)
        return qjs::byteInputTypeError(
            ctx, "AccountID.fromHex()", qjs::BytePolicy::hexString);
    if (bytes.size() != 20)
        return JS_ThrowTypeError(
            ctx, "AccountID.fromHex() needs exactly 20 bytes (got %u)",
            bytes.size());
    return js_native_new<AccountID>(
        ctx, js_accountid_class_id, bytes.data(), bytes.size());
}

static JSValue js_accountid_to_hex(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    auto *a = qjs::opaque<AccountID>(ctx, this_val, js_accountid_class_id);
    if (!a) return JS_EXCEPTION;
    char buf[41];
    a->to_hex(buf, sizeof(buf));
    buf[40] = '\0';
    return JS_NewString(ctx, buf);
}

static JSValue js_accountid_to_bytes(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    auto *a = qjs::opaque<AccountID>(ctx, this_val, js_accountid_class_id);
    if (!a) return JS_EXCEPTION;
    return qjs::uint8Array(
        ctx, std::span<std::uint8_t const>{a->data(), a->size()});
}

//@@impl STAddress
static const JSCFunctionListEntry js_accountid_proto_funcs[] = {
    JS_CFUNC_DEF("toHex", 0, js_accountid_to_hex),
    JS_CFUNC_DEF("toBytes", 0, js_accountid_to_bytes),
};

//@@impl STAddress static
static const JSCFunctionListEntry js_accountid_static_funcs[] = {
    JS_CFUNC_DEF("from", 1, js_accountid_from),
    JS_CFUNC_DEF("fromHex", 1, js_accountid_from_hex),
};

// --- XFL ---

static void js_xfl_finalizer(JSRuntime *rt, JSValue val) {
    qjs::destroyOpaque<XFL>(rt, val, js_xfl_class_id);
}

static JSClassDef js_xfl_class = {
    "XFL",
    .finalizer = js_xfl_finalizer,
};

static JSValue js_xfl_new(JSContext *ctx, const XFL &xfl) {
    return js_native_new<XFL>(ctx, js_xfl_class_id, xfl);
}

static JSValue js_xfl_from_raw(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "XFL.fromRaw() expects a value");
    int64_t raw;
    if (JS_IsBigInt(ctx, argv[0])) {
        if (JS_ToBigInt64(ctx, &raw, argv[0])) return JS_EXCEPTION;
    } else {
        if (JS_ToInt64(ctx, &raw, argv[0])) return JS_EXCEPTION;
    }
    return js_xfl_new(ctx, XFL(raw));
}

namespace jshookz::provider {

JSValue
makeSTBlob(
    JSContext* ctx,
    std::uint8_t const* bytes,
    std::uint32_t length)
{
    return js_blob_new(ctx, bytes, length);
}

JSValue
makeHash256(
    JSContext* ctx,
    std::uint8_t const* bytes,
    std::uint32_t length)
{
    if (length != 32)
        return JS_ThrowInternalError(
            ctx, "Hash256 construction requires 32 bytes");
    return js_native_new<Hash256>(
        ctx, js_hash256_class_id, bytes, length);
}

JSValue
makeAccountID(
    JSContext* ctx,
    std::uint8_t const* bytes,
    std::uint32_t length)
{
    if (length != 20)
        return JS_ThrowInternalError(
            ctx, "AccountID construction requires 20 bytes");
    return js_native_new<AccountID>(
        ctx, js_accountid_class_id, bytes, length);
}

}  // namespace jshookz::provider

static JSValue js_xfl_raw(JSContext *ctx, JSValueConst this_val)
{
    auto *x = qjs::opaque<XFL>(ctx, this_val, js_xfl_class_id);
    if (!x) return JS_EXCEPTION;
    return JS_NewBigInt64(ctx, x->raw());
}

static JSValue js_xfl_mantissa(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    auto *x = qjs::opaque<XFL>(ctx, this_val, js_xfl_class_id);
    if (!x) return JS_EXCEPTION;
    return JS_NewInt64(ctx, x->mantissa());
}

static JSValue js_xfl_exponent(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    auto *x = qjs::opaque<XFL>(ctx, this_val, js_xfl_class_id);
    if (!x) return JS_EXCEPTION;
    return JS_NewInt32(ctx, x->exponent());
}

static JSValue js_xfl_is_negative(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    auto *x = qjs::opaque<XFL>(ctx, this_val, js_xfl_class_id);
    if (!x) return JS_EXCEPTION;
    return JS_NewBool(ctx, x->is_negative());
}

static JSValue js_xfl_is_zero(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    auto *x = qjs::opaque<XFL>(ctx, this_val, js_xfl_class_id);
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

static bool
install_account_constants(JSContext* ctx, JSValueConst factory)
{
    std::uint8_t zeroBytes[20] = {};
    std::uint8_t oneBytes[20] = {};
    oneBytes[19] = 1;
    qjs::OwnedValue zero(
        ctx,
        js_native_new<AccountID>(
            ctx, js_accountid_class_id, zeroBytes, sizeof(zeroBytes)));
    qjs::OwnedValue one(
        ctx,
        js_native_new<AccountID>(
            ctx, js_accountid_class_id, oneBytes, sizeof(oneBytes)));
    if (zero.isException() || one.isException())
        return false;
    if (!qjs::freezeObject(ctx, zero.get()) ||
        !qjs::freezeObject(ctx, one.get()))
        return false;
    if (JS_DefinePropertyValueStr(
            ctx,
            factory,
            "zero",
            zero.release(),
            JS_PROP_ENUMERABLE) < 0 ||
        JS_DefinePropertyValueStr(
            ctx,
            factory,
            "one",
            one.release(),
            JS_PROP_ENUMERABLE) < 0)
        return false;
    return true;
}

// --- Registration function (called from provider.cpp) ---

using FactoryInitializer = bool (*)(JSContext *, JSValueConst);

[[nodiscard]] static bool
register_cpp_type(
    JSContext* ctx,
    JSValueConst global,
    char const* name,
    JSClassID* class_id,
    JSClassDef const* class_def,
    std::span<JSCFunctionListEntry const> prototypeFunctions,
    std::span<JSCFunctionListEntry const> staticFunctions,
    qjs::ByteClassFamily byteFamily = qjs::ByteClassFamily::none,
    JSCFunction *toBytes = nullptr,
    FactoryInitializer initializeFactory = nullptr)
{
    JS_NewClassID(class_id);
    if (JS_NewClass(JS_GetRuntime(ctx), *class_id, class_def) < 0 ||
        !qjs::registerByteClass(*class_id, byteFamily, toBytes))
        return false;

    qjs::OwnedValue prototype(ctx, JS_NewObject(ctx));
    if (prototype.isException() ||
        !qjs::installFunctions(ctx, prototype.get(), prototypeFunctions) ||
        !qjs::freezeObject(ctx, prototype.get()))
        return false;
    JS_SetClassProto(ctx, *class_id, prototype.release());

    qjs::OwnedValue factory(ctx, JS_NewObject(ctx));
    if (factory.isException() ||
        !qjs::installFunctions(ctx, factory.get(), staticFunctions) ||
        (initializeFactory != nullptr &&
         !initializeFactory(ctx, factory.get())) ||
        !qjs::freezeObject(ctx, factory.get()))
        return false;
    return JS_SetPropertyStr(
        ctx, global, name, factory.release()) >= 0;
}

extern "C" bool
register_cpp_types(JSContext *ctx)
{
    qjs::resetByteClassRegistry();
    qjs::OwnedValue global(ctx, JS_GetGlobalObject(ctx));
    if (global.isException())
        return false;

    if (!register_cpp_type(
               ctx,
               global.get(),
               "STBlob",
               &js_blob_class_id,
               &js_blob_class,
               js_blob_proto_funcs,
               js_blob_static_funcs,
               qjs::ByteClassFamily::stBlob,
               js_blob_to_bytes) ||
        !register_cpp_type(
               ctx,
               global.get(),
               "Hash256",
               &js_hash256_class_id,
               &js_hash256_class,
               js_hash256_proto_funcs,
               js_hash256_static_funcs,
               qjs::ByteClassFamily::serializedType,
               js_hash256_to_bytes) ||
        !register_cpp_type(
               ctx,
               global.get(),
               "AccountID",
               &js_accountid_class_id,
               &js_accountid_class,
               js_accountid_proto_funcs,
               js_accountid_static_funcs,
               qjs::ByteClassFamily::serializedType,
               js_accountid_to_bytes,
               install_account_constants))
        return false;
    return register_cpp_type(
               ctx,
               global.get(),
               "XFL",
               &js_xfl_class_id,
               &js_xfl_class,
               js_xfl_proto_funcs,
               js_xfl_static_funcs);
}
