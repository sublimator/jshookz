#include "hash/hash.hpp"
#include "js.hpp"

namespace jshookz::provider::types {
namespace qjs = jshookz::provider::qjs;
using hook::Hash256;

namespace {

JSClassID js_hash256_class_id;

void
js_hash256_finalizer(JSRuntime* rt, JSValue val)
{
    qjs::destroyOpaque<Hash256>(rt, val, js_hash256_class_id);
}

JSClassDef js_hash256_class = {
    .class_name = "Hash256",
    .finalizer = js_hash256_finalizer,
};

// @binding provider:Hash256.from
JSValue
js_hash256_from(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Hash256.from() expects a byte value");
    auto bytes = qjs::ByteView::getBinding(
        ctx, argv[0], "Hash256.from", 0, qjs::BytePolicy::bytesLike);
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
    return nativeNew<Hash256>(
        ctx, js_hash256_class_id, bytes.data(), bytes.size());
}

// @binding provider:Hash256.fromHex
JSValue
js_hash256_from_hex(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "Hash256.fromHex() expects a hex string");
    auto bytes = qjs::ByteView::getBinding(
        ctx, argv[0], "Hash256.fromHex", 0, qjs::BytePolicy::hexString);
    if (!bytes)
        return qjs::byteInputTypeError(
            ctx, "Hash256.fromHex()", qjs::BytePolicy::hexString);
    if (bytes.size() != 32)
        return JS_ThrowTypeError(
            ctx,
            "Hash256.fromHex() needs exactly 32 bytes (got %u)",
            bytes.size());
    return nativeNew<Hash256>(
        ctx, js_hash256_class_id, bytes.data(), bytes.size());
}

// @binding provider:Hash256.toHex
JSValue
js_hash256_to_hex(JSContext* ctx, JSValueConst this_val, int, JSValueConst*)
{
    auto* h = qjs::opaque<Hash256>(ctx, this_val, js_hash256_class_id);
    if (!h)
        return JS_EXCEPTION;
    char buf[65];
    h->to_hex(buf, sizeof(buf));
    buf[64] = '\0';
    return JS_NewString(ctx, buf);
}

// @binding provider:Hash256.toBytes
JSValue
js_hash256_to_bytes(JSContext* ctx, JSValueConst this_val, int, JSValueConst*)
{
    auto* h = qjs::opaque<Hash256>(ctx, this_val, js_hash256_class_id);
    if (!h)
        return JS_EXCEPTION;
    return qjs::uint8Array(
        ctx, std::span<std::uint8_t const>{h->data(), h->size()});
}

// @binding provider:Hash256.isZero
JSValue
js_hash256_is_zero(JSContext* ctx, JSValueConst this_val, int, JSValueConst*)
{
    auto* h = qjs::opaque<Hash256>(ctx, this_val, js_hash256_class_id);
    if (!h)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, h->is_zero());
}

// @binding provider:Hash256.equals
JSValue
js_hash256_equals(
    JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    auto* h1 = qjs::opaque<Hash256>(ctx, this_val, js_hash256_class_id);
    auto* h2 = qjs::opaque<Hash256>(
        ctx, argc > 0 ? argv[0] : JS_UNDEFINED, js_hash256_class_id);
    if (!h1 || !h2)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, *h1 == *h2);
}

//@@impl STHash
JSCFunctionListEntry const proto[] = {
    JS_CFUNC_DEF("toHex", 0, js_hash256_to_hex),
    JS_CFUNC_DEF("toBytes", 0, js_hash256_to_bytes),
    JS_CFUNC_DEF("isZero", 0, js_hash256_is_zero),
    JS_CFUNC_DEF("equals", 1, js_hash256_equals),
};

//@@impl STHash static
JSCFunctionListEntry const statics[] = {
    JS_CFUNC_DEF("from", 1, js_hash256_from),
    JS_CFUNC_DEF("fromHex", 1, js_hash256_from_hex),
};

}  // namespace

bool
registerHash256(JSContext* ctx, JSValueConst global)
{
    return registerClass(
        ctx,
        global,
        "Hash256",
        &js_hash256_class_id,
        &js_hash256_class,
        proto,
        statics,
        qjs::ByteClassFamily::serializedType,
        js_hash256_to_bytes);
}

JSValue
makeHash256Bytes(
    JSContext* ctx, std::uint8_t const* bytes, std::uint32_t length)
{
    if (length != 32)
        return JS_ThrowInternalError(
            ctx, "Hash256 construction requires 32 bytes");
    return nativeNew<Hash256>(ctx, js_hash256_class_id, bytes, length);
}

}  // namespace jshookz::provider::types

namespace jshookz::provider {

JSValue
makeHash256(JSContext* ctx, std::uint8_t const* bytes, std::uint32_t length)
{
    return types::makeHash256Bytes(ctx, bytes, length);
}

}  // namespace jshookz::provider
