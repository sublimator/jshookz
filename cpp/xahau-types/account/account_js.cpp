#include "account/account.hpp"
#include "js.hpp"
#include "object/nominal_payload.hpp"
#include "object/object.hpp"

#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
#include "tests/object_gc_lifetime_probe_hooks.hpp"
#endif

#include <cstring>

namespace jshookz::provider::types {
namespace qjs = jshookz::provider::qjs;
using hook::AccountID;

namespace {

JSClassID js_accountid_class_id;

struct AccountIDState {
  JSValue owner = JS_UNDEFINED;
  AccountID owned;
  std::uint8_t const *data = nullptr;
};

constexpr std::uint8_t zeroAccount[20] = {};

[[nodiscard]] AccountIDState *accountState(JSContext *ctx, JSValueConst value) {
  return qjs::opaque<AccountIDState>(ctx, value, js_accountid_class_id);
}

[[nodiscard]] JSValue newAccountID(JSContext *ctx, JSValueConst owner,
                                   std::uint8_t const *bytes,
                                   std::uint32_t encodedLength) {
  if (JS_IsUndefined(owner)) {
    if (encodedLength != 20 || bytes == nullptr)
      return JS_ThrowInternalError(ctx,
                                   "AccountID construction requires 20 bytes");
  } else if ((encodedLength != 0 && encodedLength != 20) ||
             !isCertifiedObjectRange(ctx, owner, bytes, encodedLength)) {
    return JS_HasException(ctx)
               ? JS_EXCEPTION
               : JS_ThrowTypeError(ctx,
                                   "AccountID: certified owner is required");
  }
  auto *state =
      static_cast<AccountIDState *>(js_mallocz(ctx, sizeof(AccountIDState)));
  if (!state)
    return JS_ThrowOutOfMemory(ctx);
  new (&state->owned) AccountID();
  if (JS_IsUndefined(owner)) {
    state->owned = AccountID(bytes, encodedLength);
    state->data = state->owned.data();
  } else {
    state->owner = JS_DupValue(ctx, owner);
    state->data = encodedLength == 0 ? zeroAccount : bytes;
  }
  JSValue object = JS_NewObjectClass(ctx, js_accountid_class_id);
  if (JS_IsException(object)) {
    JS_FreeValue(ctx, state->owner);
    state->owned.~AccountID();
    js_free(ctx, state);
    return object;
  }
  JS_SetOpaque(object, state);
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
  if (!JS_IsUndefined(owner)) {
    test::gcProbeCreated(test::TrackedEntity::accountID, object);
    if (!test::gcProbePlantCycle(ctx, test::HiddenEdge::accountIDOwner, owner,
                                 object, object) ||
        !test::gcProbePlantPendingOwnerCycle(
            ctx, test::HiddenEdge::issueCacheValue, owner) ||
        !test::gcProbePlantPendingOwnerCycle(
            ctx, test::HiddenEdge::xchainBridgeCacheValue, owner)) {
      JS_FreeValue(ctx, object);
      return JS_EXCEPTION;
    }
  }
#endif
  return object;
}

void js_accountid_finalizer(JSRuntime *rt, JSValue val) {
  auto *state =
      static_cast<AccountIDState *>(JS_GetOpaque(val, js_accountid_class_id));
  if (!state)
    return;
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
  if (!JS_IsUndefined(state->owner))
    test::gcProbeFinalized(test::TrackedEntity::accountID, val);
#endif
  JS_FreeValueRT(rt, state->owner);
  state->owned.~AccountID();
  js_free_rt(rt, state);
}

void js_accountid_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark) {
  auto const *state = static_cast<AccountIDState const *>(
      JS_GetOpaque(val, js_accountid_class_id));
  if (state) {
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
    if (!test::gcProbeMarkEnabled(test::HiddenEdge::accountIDOwner))
      return;
#endif
    JS_MarkValue(rt, state->owner, mark);
  }
}

JSClassDef js_accountid_class = {
    .class_name = "AccountID",
    .finalizer = js_accountid_finalizer,
    .gc_mark = js_accountid_mark,
};

// @binding provider:AccountID.from
JSValue js_accountid_from(JSContext *ctx, JSValueConst, int argc,
                          JSValueConst *argv) {
  if (argc < 1)
    return JS_ThrowTypeError(ctx, "AccountID.from() expects a byte value");
  auto bytes = qjs::ByteView::getBinding(ctx, argv[0], "AccountID.from", 0,
                                         qjs::BytePolicy::bytesLike);
  if (!bytes) {
    return qjs::byteInputTypeError(ctx, "AccountID.from()",
                                   qjs::BytePolicy::bytesLike);
  }
  if (bytes.size() != 20) {
    return JS_ThrowTypeError(
        ctx, "AccountID.from() needs exactly 20 bytes (got %u)", bytes.size());
  }
  return newAccountID(ctx, JS_UNDEFINED, bytes.data(), bytes.size());
}

// @binding provider:AccountID.fromHex
JSValue js_accountid_from_hex(JSContext *ctx, JSValueConst, int argc,
                              JSValueConst *argv) {
  if (argc < 1 || !JS_IsString(argv[0]))
    return JS_ThrowTypeError(ctx, "AccountID.fromHex() expects a hex string");
  auto bytes = qjs::ByteView::getBinding(ctx, argv[0], "AccountID.fromHex", 0,
                                         qjs::BytePolicy::hexString);
  if (!bytes)
    return qjs::byteInputTypeError(ctx, "AccountID.fromHex()",
                                   qjs::BytePolicy::hexString);
  if (bytes.size() != 20)
    return JS_ThrowTypeError(
        ctx, "AccountID.fromHex() needs exactly 20 bytes (got %u)",
        bytes.size());
  return newAccountID(ctx, JS_UNDEFINED, bytes.data(), bytes.size());
}

// @binding provider:AccountID.toHex
JSValue js_accountid_to_hex(JSContext *ctx, JSValueConst this_val, int,
                            JSValueConst *) {
  auto *state = accountState(ctx, this_val);
  if (!state)
    return JS_EXCEPTION;
  char buf[41];
  constexpr char digits[] = "0123456789ABCDEF";
  for (std::size_t i = 0; i < 20; ++i) {
    buf[i * 2] = digits[state->data[i] >> 4];
    buf[i * 2 + 1] = digits[state->data[i] & 0x0f];
  }
  buf[40] = '\0';
  return JS_NewString(ctx, buf);
}

// @binding provider:AccountID.toBytes
JSValue js_accountid_to_bytes(JSContext *ctx, JSValueConst this_val, int,
                              JSValueConst *) {
  auto *state = accountState(ctx, this_val);
  if (!state)
    return JS_EXCEPTION;
  return qjs::uint8Array(ctx, std::span<std::uint8_t const>{state->data, 20});
}

// @binding provider:AccountID.isZero
JSValue js_accountid_is_zero(JSContext *ctx, JSValueConst this_val, int,
                             JSValueConst *) {
  auto *state = accountState(ctx, this_val);
  if (!state)
    return JS_EXCEPTION;
  std::uint8_t combined = 0;
  for (std::size_t i = 0; i < 20; ++i)
    combined |= state->data[i];
  return JS_NewBool(ctx, combined == 0);
}

// @binding provider:AccountID.equals
JSValue js_accountid_equals(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
  auto *left = accountState(ctx, this_val);
  auto *right = accountState(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
  if (!left || !right)
    return JS_EXCEPTION;
  return JS_NewBool(ctx, std::memcmp(left->data, right->data, 20) == 0);
}

//@@impl STAddress
JSCFunctionListEntry const proto[] = {
    JS_CFUNC_DEF("toHex", 0, js_accountid_to_hex),
    JS_CFUNC_DEF("toBytes", 0, js_accountid_to_bytes),
    JS_CFUNC_DEF("isZero", 0, js_accountid_is_zero),
    JS_CFUNC_DEF("equals", 1, js_accountid_equals),
};

//@@impl STAddress static
JSCFunctionListEntry const statics[] = {
    JS_CFUNC_DEF("from", 1, js_accountid_from),
    JS_CFUNC_DEF("fromHex", 1, js_accountid_from_hex),
};

// @binding provider:AccountID.zero
// @binding provider:AccountID.one
bool install_account_constants(JSContext *ctx, JSValueConst factory) {
  std::uint8_t zeroBytes[20] = {};
  std::uint8_t oneBytes[20] = {};
  oneBytes[19] = 1;
  qjs::OwnedValue zero(
      ctx, newAccountID(ctx, JS_UNDEFINED, zeroBytes, sizeof(zeroBytes)));
  qjs::OwnedValue one(
      ctx, newAccountID(ctx, JS_UNDEFINED, oneBytes, sizeof(oneBytes)));
  if (zero.isException() || one.isException())
    return false;
  if (!qjs::freezeObject(ctx, zero.get()) || !qjs::freezeObject(ctx, one.get()))
    return false;
  return JS_DefinePropertyValueStr(ctx, factory, "zero", zero.release(),
                                   JS_PROP_ENUMERABLE) >= 0 &&
         JS_DefinePropertyValueStr(ctx, factory, "one", one.release(),
                                   JS_PROP_ENUMERABLE) >= 0;
}

}  // namespace

bool
registerAccountID(JSContext* ctx, JSValueConst global)
{
    return registerClass(
        ctx,
        global,
        "AccountID",
        &js_accountid_class_id,
        &js_accountid_class,
        proto,
        statics,
        RuntimeTypeId::accountID,
        qjs::ByteClassFamily::serializedType,
        js_accountid_to_bytes,
        install_account_constants);
}

bool
isAccountID(JSValueConst value) noexcept
{
    if (!JS_IsObject(value) || JS_GetClassID(value) != js_accountid_class_id)
        return false;
    auto const* state = static_cast<AccountIDState const*>(
        JS_GetOpaque(value, js_accountid_class_id));
    return state != nullptr && state->data != nullptr;
}

JSValue makeAccountIDBytes(JSContext *ctx, std::uint8_t const *bytes,
                           std::uint32_t length) {
  if (length != 20)
    return JS_ThrowInternalError(ctx,
                                 "AccountID construction requires 20 bytes");
  return newAccountID(ctx, JS_UNDEFINED, bytes, length);
}

JSValue makeAccountIDView(JSContext *ctx, JSValueConst owner,
                          std::uint8_t const *bytes,
                          std::uint32_t encodedLength) {
  return newAccountID(ctx, owner, bytes, encodedLength);
}

bool readAccountIDBytes(JSContext *, JSValueConst input,
                        std::uint8_t output[20]) noexcept {
  if (output == nullptr)
    return false;
  std::memset(output, 0, 20);
  if (!JS_IsObject(input) || JS_GetClassID(input) != js_accountid_class_id)
    return false;
  auto const *state = static_cast<AccountIDState const *>(
      JS_GetOpaque(input, js_accountid_class_id));
  if (state == nullptr || state->data == nullptr)
    return false;
  std::memcpy(output, state->data, 20);
  return true;
}

bool detail::readAccountIDNominalPayload(JSValueConst input,
                                         NominalPayloadView &output) noexcept {
  output = {};
  if (!JS_IsObject(input) || JS_GetClassID(input) != js_accountid_class_id)
    return false;
  auto const *state = static_cast<AccountIDState const *>(
      JS_GetOpaque(input, js_accountid_class_id));
  if (state == nullptr || state->data == nullptr)
    return false;
  output = {state->data, 20};
  return true;
}

}  // namespace jshookz::provider::types

namespace jshookz::provider {

JSValue
makeAccountID(JSContext* ctx, std::uint8_t const* bytes, std::uint32_t length)
{
    return types::makeAccountIDBytes(ctx, bytes, length);
}

}  // namespace jshookz::provider
