#include "js.hpp"
#include "object/nominal_payload.hpp"
#include "object/object.hpp"

#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
#include "tests/object_gc_lifetime_probe_hooks.hpp"
#endif

#include <cstring>
#include <limits>

namespace jshookz::provider::types {
namespace qjs = jshookz::provider::qjs;

namespace {

JSClassID js_blob_class_id;

struct JSBlob {
  JSValue owner = JS_UNDEFINED;
  uint8_t *owned = nullptr;
  uint8_t const *data = nullptr;
  size_t len = 0;
};

void js_blob_finalizer(JSRuntime *rt, JSValue val) {
  auto *blob = static_cast<JSBlob *>(JS_GetOpaque(val, js_blob_class_id));
  if (!blob)
    return;
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
  if (!JS_IsUndefined(blob->owner))
    test::gcProbeFinalized(test::TrackedEntity::blob, val);
#endif
  if (blob->owned)
    js_free_rt(rt, blob->owned);
  JS_FreeValueRT(rt, blob->owner);
  js_free_rt(rt, blob);
}

void js_blob_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark) {
  auto const *blob =
      static_cast<JSBlob const *>(JS_GetOpaque(val, js_blob_class_id));
  if (blob) {
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
    if (!test::gcProbeMarkEnabled(test::HiddenEdge::blobOwner))
      return;
#endif
    JS_MarkValue(rt, blob->owner, mark);
  }
}

JSClassDef js_blob_class = {
    .class_name = "STBlob",
    .finalizer = js_blob_finalizer,
    .gc_mark = js_blob_mark,
};

JSValue newBlobUninitialized(JSContext *ctx, size_t len, std::uint8_t **data) {
  *data = nullptr;
  auto *blob = (JSBlob *)js_mallocz(ctx, sizeof(JSBlob));
  if (!blob)
    return JS_ThrowOutOfMemory(ctx);
  if (len != 0) {
    blob->owned = (uint8_t *)js_malloc(ctx, len);
    if (!blob->owned) {
      js_free(ctx, blob);
      return JS_ThrowOutOfMemory(ctx);
    }
  }
  blob->data = blob->owned;
  blob->len = len;

  JSValue obj = JS_NewObjectClass(ctx, js_blob_class_id);
  if (JS_IsException(obj)) {
    if (blob->owned)
      js_free(ctx, blob->owned);
    js_free(ctx, blob);
    return obj;
  }
  JS_SetOpaque(obj, blob);
  *data = blob->owned;
  return obj;
}

JSValue newBlobView(JSContext *ctx, JSValueConst owner, uint8_t const *data,
                    size_t len) {
  if (!isCertifiedObjectRange(ctx, owner, data,
                              static_cast<std::uint32_t>(len)))
    return JS_HasException(ctx)
               ? JS_EXCEPTION
               : JS_ThrowTypeError(ctx, "STBlob: certified owner is required");
  auto *blob = static_cast<JSBlob *>(js_mallocz(ctx, sizeof(JSBlob)));
  if (!blob)
    return JS_ThrowOutOfMemory(ctx);
  blob->owner = JS_DupValue(ctx, owner);
  blob->data = data;
  blob->len = len;
  JSValue obj = JS_NewObjectClass(ctx, js_blob_class_id);
  if (JS_IsException(obj)) {
    JS_FreeValue(ctx, blob->owner);
    js_free(ctx, blob);
    return obj;
  }
  JS_SetOpaque(obj, blob);
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
  test::gcProbeCreated(test::TrackedEntity::blob, obj);
  if (!test::gcProbePlantCycle(ctx, test::HiddenEdge::blobOwner, owner, obj,
                               obj)) {
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
  }
#endif
  return obj;
}

JSValue
newBlob(JSContext* ctx, const uint8_t* data, size_t len)
{
    std::uint8_t* output = nullptr;
    JSValue value = newBlobUninitialized(ctx, len, &output);
    if (!JS_IsException(value) && len != 0)
        std::memcpy(output, data, len);
    return value;
}

// @binding provider:STBlob.from
JSValue
js_blob_from(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "STBlob.from() expects a byte value");
    if (JS_IsObject(argv[0]) && JS_GetClassID(argv[0]) == js_blob_class_id) {
        auto const* blob = static_cast<JSBlob const*>(
            JS_GetOpaque(argv[0], js_blob_class_id));
        if (blob == nullptr || (blob->len != 0 && blob->data == nullptr))
            return qjs::byteInputTypeError(
                ctx, "STBlob.from()", qjs::BytePolicy::bytesLikeOrSTBlob);
        return JS_DupValue(ctx, argv[0]);
    }
    auto bytes = qjs::ByteView::getBinding(
        ctx, argv[0], "STBlob.from", 0,
        qjs::BytePolicy::bytesLikeOrSTBlob);
    if (!bytes)
        return qjs::byteInputTypeError(
            ctx, "STBlob.from()", qjs::BytePolicy::bytesLikeOrSTBlob);
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

} // namespace

bool registerSTBlob(JSContext *ctx, JSValueConst global) {
  return registerClass(ctx, global, "STBlob", &js_blob_class_id, &js_blob_class,
                       proto, statics, RuntimeTypeId::stBlob,
                       qjs::ByteClassFamily::stBlob, js_blob_to_bytes);
}

bool
isSTBlob(JSValueConst value) noexcept
{
    if (!JS_IsObject(value) || JS_GetClassID(value) != js_blob_class_id)
        return false;
    auto const* blob =
        static_cast<JSBlob const*>(JS_GetOpaque(value, js_blob_class_id));
    return blob != nullptr && (blob->len == 0 || blob->data != nullptr);
}

JSValue makeSTBlobBytes(JSContext *ctx, std::uint8_t const *bytes,
                        std::uint32_t length) {
  return newBlob(ctx, bytes, length);
}

JSValue makeSTBlobView(JSContext *ctx, JSValueConst owner,
                       std::uint8_t const *bytes, std::uint32_t length) {
  return newBlobView(ctx, owner, bytes, length);
}

JSValue makeSTBlobUninitialized(JSContext *ctx, std::uint32_t length,
                                std::uint8_t **data) {
  return newBlobUninitialized(ctx, length, data);
}

JSObjectByteSpanStatus getSTBlobByteSpanNoThrow(JSContext *ctx,
                                                JSValueConst input,
                                                JSValue *owned_backing,
                                                std::uint8_t const **data,
                                                std::size_t *size) noexcept {
  *owned_backing = JS_UNDEFINED;
  *data = nullptr;
  *size = 0;
  if (!JS_IsObject(input) || JS_GetClassID(input) != js_blob_class_id)
    return JS_OBJECT_BYTES_WRONG_KIND;
  auto const *blob =
      static_cast<JSBlob const *>(JS_GetOpaque(input, js_blob_class_id));
  if (blob == nullptr || (blob->len != 0 && blob->data == nullptr))
    return JS_OBJECT_BYTES_UNUSABLE;
  *owned_backing = JS_DupValue(ctx, input);
  *data = blob->data;
  *size = blob->len;
  return JS_OBJECT_BYTES_OK;
}

bool detail::readSTBlobNominalPayload(JSValueConst input,
                                      NominalPayloadView &output) noexcept {
  output = {};
  if (!JS_IsObject(input) || JS_GetClassID(input) != js_blob_class_id)
    return false;
  auto const *blob =
      static_cast<JSBlob const *>(JS_GetOpaque(input, js_blob_class_id));
  if (blob == nullptr ||
      blob->len > std::numeric_limits<std::uint32_t>::max() ||
      (blob->len != 0 && blob->data == nullptr))
    return false;
  output = {blob->data, static_cast<std::uint32_t>(blob->len)};
  return true;
}

} // namespace jshookz::provider::types

namespace jshookz::provider {

JSValue
makeSTBlob(JSContext* ctx, std::uint8_t const* bytes, std::uint32_t length)
{
    return types::makeSTBlobBytes(ctx, bytes, length);
}

}  // namespace jshookz::provider
