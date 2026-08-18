#include "js.hpp"

#include <cstring>

namespace jshookz::provider::types {
namespace qjs = jshookz::provider::qjs;

namespace {

JSClassID js_blob_class_id;

struct JSBlob
{
    uint8_t* data = nullptr;
    size_t len = 0;
};

void
js_blob_finalizer(JSRuntime* rt, JSValue val)
{
    auto* blob = static_cast<JSBlob*>(JS_GetOpaque(val, js_blob_class_id));
    if (!blob)
        return;
    if (blob->data)
        js_free_rt(rt, blob->data);
    js_free_rt(rt, blob);
}

JSClassDef js_blob_class = {
    .class_name = "STBlob",
    .finalizer = js_blob_finalizer,
};

JSValue
newBlob(JSContext* ctx, const uint8_t* data, size_t len)
{
    auto* blob = (JSBlob*)js_mallocz(ctx, sizeof(JSBlob));
    if (!blob)
        return JS_ThrowOutOfMemory(ctx);
    if (len != 0) {
        blob->data = (uint8_t*)js_malloc(ctx, len);
        if (!blob->data) {
            js_free(ctx, blob);
            return JS_ThrowOutOfMemory(ctx);
        }
        std::memcpy(blob->data, data, len);
    }
    blob->len = len;

    JSValue obj = JS_NewObjectClass(ctx, js_blob_class_id);
    if (JS_IsException(obj)) {
        if (blob->data)
            js_free(ctx, blob->data);
        js_free(ctx, blob);
        return obj;
    }
    JS_SetOpaque(obj, blob);
    return obj;
}

// @binding provider:STBlob.from
JSValue
js_blob_from(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "STBlob.from() expects a byte value");
    auto bytes = qjs::ByteView::getBinding(
        ctx, argv[0], "STBlob.from", 0, qjs::BytePolicy::bytesLike);
    if (!bytes)
        return qjs::byteInputTypeError(
            ctx, "STBlob.from()", qjs::BytePolicy::bytesLike);
    return newBlob(ctx, bytes.data(), bytes.size());
}

// @binding provider:STBlob.fromHex
JSValue
js_blob_from_hex(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "STBlob.fromHex() expects a hex string");
    auto bytes = qjs::ByteView::getBinding(
        ctx, argv[0], "STBlob.fromHex", 0, qjs::BytePolicy::hexString);
    if (!bytes)
        return qjs::byteInputTypeError(
            ctx, "STBlob.fromHex()", qjs::BytePolicy::hexString);
    return newBlob(ctx, bytes.data(), bytes.size());
}

// @binding provider:STBlob.byteLength
JSValue
js_blob_byte_length(JSContext* ctx, JSValueConst this_val)
{
    auto* blob = qjs::opaque<JSBlob>(ctx, this_val, js_blob_class_id);
    if (!blob)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)blob->len);
}

// @binding provider:STBlob.toBytes
JSValue
js_blob_to_bytes(JSContext* ctx, JSValueConst this_val, int, JSValueConst*)
{
    auto* blob = qjs::opaque<JSBlob>(ctx, this_val, js_blob_class_id);
    if (!blob)
        return JS_EXCEPTION;
    return qjs::uint8Array(
        ctx, std::span<std::uint8_t const>{blob->data, blob->len});
}

// @binding provider:STBlob.toHex
JSValue
js_blob_to_hex(JSContext* ctx, JSValueConst this_val, int, JSValueConst*)
{
    auto* blob = qjs::opaque<JSBlob>(ctx, this_val, js_blob_class_id);
    if (!blob)
        return JS_EXCEPTION;
    static const char hex[] = "0123456789ABCDEF";
    char* encoded = (char*)js_malloc(ctx, blob->len * 2 + 1);
    if (!encoded)
        return JS_ThrowOutOfMemory(ctx);
    for (size_t i = 0; i < blob->len; ++i) {
        encoded[i * 2] = hex[blob->data[i] >> 4];
        encoded[i * 2 + 1] = hex[blob->data[i] & 0x0F];
    }
    encoded[blob->len * 2] = '\0';
    JSValue result = JS_NewStringLen(ctx, encoded, blob->len * 2);
    js_free(ctx, encoded);
    return result;
}

// @binding provider:STBlob.byteAt
JSValue
js_blob_byte_at(
    JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    auto* blob = qjs::opaque<JSBlob>(ctx, this_val, js_blob_class_id);
    if (!blob)
        return JS_EXCEPTION;
    int64_t index;
    if (argc < 1 || JS_ToInt64(ctx, &index, argv[0]) < 0)
        return JS_EXCEPTION;
    if (index < 0 || (uint64_t)index >= blob->len)
        return JS_ThrowRangeError(ctx, "STBlob.byteAt(): index out of range");
    return JS_NewInt32(ctx, blob->data[index]);
}

// @binding provider:STBlob.equals
JSValue
js_blob_equals(
    JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    auto* blob = qjs::opaque<JSBlob>(ctx, this_val, js_blob_class_id);
    if (!blob)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_FALSE;
    auto other = qjs::ByteView::getBinding(
        ctx, argv[0], "STBlob.equals", 0, qjs::BytePolicy::bytesLikeOrSTBlob);
    if (!other)
        return JS_HasException(ctx) ? JS_EXCEPTION : JS_FALSE;
    bool equal = blob->len == other.size() &&
        (blob->len == 0 ||
         std::memcmp(blob->data, other.data(), blob->len) == 0);
    return JS_NewBool(ctx, equal);
}

JSCFunctionListEntry const proto[] = {
    JS_CGETSET_DEF("byteLength", js_blob_byte_length, NULL),
    JS_CFUNC_DEF("byteAt", 1, js_blob_byte_at),
    JS_CFUNC_DEF("toBytes", 0, js_blob_to_bytes),
    JS_CFUNC_DEF("toHex", 0, js_blob_to_hex),
    JS_CFUNC_DEF("equals", 1, js_blob_equals),
};

JSCFunctionListEntry const statics[] = {
    JS_CFUNC_DEF("from", 1, js_blob_from),
    JS_CFUNC_DEF("fromHex", 1, js_blob_from_hex),
};

}  // namespace

bool
registerSTBlob(JSContext* ctx, JSValueConst global)
{
    return registerClass(
        ctx,
        global,
        "STBlob",
        &js_blob_class_id,
        &js_blob_class,
        proto,
        statics,
        qjs::ByteClassFamily::stBlob,
        js_blob_to_bytes);
}

JSValue
makeSTBlobBytes(JSContext* ctx, std::uint8_t const* bytes, std::uint32_t length)
{
    return newBlob(ctx, bytes, length);
}

}  // namespace jshookz::provider::types

namespace jshookz::provider {

JSValue
makeSTBlob(JSContext* ctx, std::uint8_t const* bytes, std::uint32_t length)
{
    return types::makeSTBlobBytes(ctx, bytes, length);
}

}  // namespace jshookz::provider
