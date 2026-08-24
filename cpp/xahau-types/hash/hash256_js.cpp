#include "hash/hash.hpp"
#include "js.hpp"
#include "object/nominal_payload.hpp"
#include "object/object.hpp"

#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
#include "tests/object_gc_lifetime_probe_hooks.hpp"
#endif

#include <cstring>

namespace jshookz::provider::types {
namespace qjs = jshookz::provider::qjs;
using hook::Hash256;

namespace {

JSClassID js_hash256_class_id;

struct Hash256State {
  JSValue owner = JS_UNDEFINED;
  Hash256 owned;
  std::uint8_t const *data = nullptr;
};

[[nodiscard]] Hash256State *hashState(JSContext *ctx, JSValueConst value) {
  return qjs::opaque<Hash256State>(ctx, value, js_hash256_class_id);
}

[[nodiscard]] JSValue newHash256(JSContext *ctx, JSValueConst owner,
                                 std::uint8_t const *bytes,
                                 std::uint32_t length) {
  if (length != 32 || bytes == nullptr)
    return JS_ThrowInternalError(ctx, "Hash256 construction requires 32 bytes");
  if (!JS_IsUndefined(owner) &&
      !isCertifiedObjectRange(ctx, owner, bytes, length))
    return JS_HasException(ctx)
               ? JS_EXCEPTION
               : JS_ThrowTypeError(ctx, "Hash256: certified owner is required");
  auto *state =
      static_cast<Hash256State *>(js_mallocz(ctx, sizeof(Hash256State)));
  if (!state)
    return JS_ThrowOutOfMemory(ctx);
  new (&state->owned) Hash256();
  if (JS_IsUndefined(owner)) {
    state->owned = Hash256(bytes, length);
    state->data = state->owned.data();
  } else {
    state->owner = JS_DupValue(ctx, owner);
    state->data = bytes;
  }
  JSValue object = JS_NewObjectClass(ctx, js_hash256_class_id);
  if (JS_IsException(object)) {
    JS_FreeValue(ctx, state->owner);
    state->owned.~Hash256();
    js_free(ctx, state);
    return object;
  }
  JS_SetOpaque(object, state);
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
  if (!JS_IsUndefined(owner)) {
    test::gcProbeCreated(test::TrackedEntity::hash256, object);
    if (!test::gcProbePlantCycle(ctx, test::HiddenEdge::hash256Owner, owner,
                                 object, object) ||
        !test::gcProbePlantPendingOwnerCycle(
            ctx, test::HiddenEdge::vector256CacheValue, owner)) {
      JS_FreeValue(ctx, object);
      return JS_EXCEPTION;
    }
  }
#endif
  return object;
}

void js_hash256_finalizer(JSRuntime *rt, JSValue val) {
  auto *state =
      static_cast<Hash256State *>(JS_GetOpaque(val, js_hash256_class_id));
  if (!state)
    return;
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
  if (!JS_IsUndefined(state->owner))
    test::gcProbeFinalized(test::TrackedEntity::hash256, val);
#endif
  JS_FreeValueRT(rt, state->owner);
  state->owned.~Hash256();
  js_free_rt(rt, state);
}

void js_hash256_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark) {
  auto const *state =
      static_cast<Hash256State const *>(JS_GetOpaque(val, js_hash256_class_id));
  if (state) {
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
    if (!test::gcProbeMarkEnabled(test::HiddenEdge::hash256Owner))
      return;
#endif
    JS_MarkValue(rt, state->owner, mark);
  }
}

JSClassDef js_hash256_class = {
    .class_name = "Hash256",
    .finalizer = js_hash256_finalizer,
    .gc_mark = js_hash256_mark,
};

// @binding provider:Hash256.from
JSValue js_hash256_from(JSContext *ctx, JSValueConst, int argc,
                        JSValueConst *argv) {
  if (argc < 1)
    return JS_ThrowTypeError(ctx, "Hash256.from() expects a byte value");
  auto bytes = qjs::ByteView::getBinding(ctx, argv[0], "Hash256.from", 0,
                                         qjs::BytePolicy::bytesLike);
  if (!bytes) {
    return qjs::byteInputTypeError(ctx, "Hash256.from()",
                                   qjs::BytePolicy::bytesLike);
  }
  if (bytes.size() != 32) {
    return JS_ThrowTypeError(
        ctx, "Hash256.from() needs exactly 32 bytes (got %u)", bytes.size());
  }
  return newHash256(ctx, JS_UNDEFINED, bytes.data(), bytes.size());
}

// @binding provider:Hash256.fromHex
JSValue js_hash256_from_hex(JSContext *ctx, JSValueConst, int argc,
                            JSValueConst *argv) {
  if (argc < 1 || !JS_IsString(argv[0]))
    return JS_ThrowTypeError(ctx, "Hash256.fromHex() expects a hex string");
  auto bytes = qjs::ByteView::getBinding(ctx, argv[0], "Hash256.fromHex", 0,
                                         qjs::BytePolicy::hexString);
  if (!bytes)
    return qjs::byteInputTypeError(ctx, "Hash256.fromHex()",
                                   qjs::BytePolicy::hexString);
  if (bytes.size() != 32)
    return JS_ThrowTypeError(
        ctx, "Hash256.fromHex() needs exactly 32 bytes (got %u)", bytes.size());
  return newHash256(ctx, JS_UNDEFINED, bytes.data(), bytes.size());
}

// @binding provider:Hash256.toHex
JSValue js_hash256_to_hex(JSContext *ctx, JSValueConst this_val, int,
                          JSValueConst *) {
  auto *state = hashState(ctx, this_val);
  if (!state)
    return JS_EXCEPTION;
  char buf[65];
  constexpr char digits[] = "0123456789ABCDEF";
  for (std::size_t i = 0; i < 32; ++i) {
    buf[i * 2] = digits[state->data[i] >> 4];
    buf[i * 2 + 1] = digits[state->data[i] & 0x0f];
  }
  buf[64] = '\0';
  return JS_NewString(ctx, buf);
}

// @binding provider:Hash256.toBytes
JSValue js_hash256_to_bytes(JSContext *ctx, JSValueConst this_val, int,
                            JSValueConst *) {
  auto *state = hashState(ctx, this_val);
  if (!state)
    return JS_EXCEPTION;
  return qjs::uint8Array(ctx, std::span<std::uint8_t const>{state->data, 32});
}

// @binding provider:Hash256.isZero
JSValue js_hash256_is_zero(JSContext *ctx, JSValueConst this_val, int,
                           JSValueConst *) {
  auto *state = hashState(ctx, this_val);
  if (!state)
    return JS_EXCEPTION;
  std::uint8_t combined = 0;
  for (std::size_t i = 0; i < 32; ++i)
    combined |= state->data[i];
  return JS_NewBool(ctx, combined == 0);
}

// @binding provider:Hash256.equals
JSValue js_hash256_equals(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv) {
  auto *left = hashState(ctx, this_val);
  auto *right = hashState(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
  if (!left || !right)
    return JS_EXCEPTION;
  return JS_NewBool(ctx, std::memcmp(left->data, right->data, 32) == 0);
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

// @binding provider:Hash256.zero
bool install_hash256_constants(JSContext *ctx, JSValueConst factory) {
  std::uint8_t bytes[32] = {};
  qjs::OwnedValue zero(ctx,
                       newHash256(ctx, JS_UNDEFINED, bytes, sizeof(bytes)));
  if (zero.isException() || !qjs::freezeObject(ctx, zero.get()))
    return false;
  return JS_DefinePropertyValueStr(ctx, factory, "zero", zero.release(),
                                   JS_PROP_ENUMERABLE) >= 0;
}

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
        RuntimeTypeId::hash256,
        qjs::ByteClassFamily::serializedType,
        js_hash256_to_bytes,
        install_hash256_constants);
}

bool
isHash256(JSValueConst value) noexcept
{
    if (!JS_IsObject(value) || JS_GetClassID(value) != js_hash256_class_id)
        return false;
    auto const* state = static_cast<Hash256State const*>(
        JS_GetOpaque(value, js_hash256_class_id));
    return state != nullptr && state->data != nullptr;
}

JSValue makeHash256Bytes(JSContext *ctx, std::uint8_t const *bytes,
                         std::uint32_t length) {
  if (length != 32)
    return JS_ThrowInternalError(ctx, "Hash256 construction requires 32 bytes");
  return newHash256(ctx, JS_UNDEFINED, bytes, length);
}

JSValue makeHash256View(JSContext *ctx, JSValueConst owner,
                        std::uint8_t const *bytes, std::uint32_t length) {
  return newHash256(ctx, owner, bytes, length);
}

bool detail::readHash256NominalPayload(JSValueConst input,
                                       NominalPayloadView &output) noexcept {
  output = {};
  if (!JS_IsObject(input) || JS_GetClassID(input) != js_hash256_class_id)
    return false;
  auto const *state = static_cast<Hash256State const *>(
      JS_GetOpaque(input, js_hash256_class_id));
  if (state == nullptr || state->data == nullptr)
    return false;
  output = {state->data, 32};
  return true;
}

}  // namespace jshookz::provider::types

namespace jshookz::provider {

JSValue
makeHash256(JSContext* ctx, std::uint8_t const* bytes, std::uint32_t length)
{
    return types::makeHash256Bytes(ctx, bytes, length);
}

}  // namespace jshookz::provider
