#include "keylet_js.hpp"

#include "js.hpp"
#include "object/nominal_payload.hpp"
#include "quickjs.hpp"
#include "result.hpp"
#include "runtime_type.hpp"

#include "sha_impl.h"

#include <array>
#include <cstring>

namespace jshookz::provider::types {
namespace {

namespace qjs = jshookz::provider::qjs;

constexpr std::uint16_t kAccountRootType = 0x0061;
constexpr std::uint16_t kUriTokenType = 0x0055;
constexpr std::uint16_t kHookLedgerType = 0x0048;
constexpr std::uint16_t kHookDefinitionType = 0x0044;
constexpr std::uint16_t kFeesType = 0x0073;
constexpr std::uint16_t kFeesNamespace = 0x0065;
constexpr std::uint32_t kKeyletBytes = 34;

JSClassID keyletClassId;

struct LedgerKeyletState {
  std::array<std::uint8_t, kKeyletBytes> bytes{};
  LedgerKeyletKind kind = LedgerKeyletKind::generic;
};

[[nodiscard]] LedgerKeyletState *keyletState(JSContext *ctx,
                                             JSValueConst value) {
  return qjs::opaque<LedgerKeyletState>(ctx, value, keyletClassId);
}

[[nodiscard]] std::uint16_t keyletType(LedgerKeyletState const &state) {
  return static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(state.bytes[0]) << 8) | state.bytes[1]);
}

[[nodiscard]] JSValue newKeylet(JSContext *ctx, LedgerKeyletState const &state) {
  return nativeNew<LedgerKeyletState>(ctx, keyletClassId, state);
}

[[nodiscard]] JSValue newDirectKeylet(JSContext *ctx, LedgerKeyletKind kind,
                                      std::uint16_t type,
                                      std::uint8_t const locator[32]) {
  LedgerKeyletState state;
  state.kind = kind;
  state.bytes[0] = static_cast<std::uint8_t>(type >> 8);
  state.bytes[1] = static_cast<std::uint8_t>(type);
  std::memcpy(state.bytes.data() + 2, locator, 32);
  return newKeylet(ctx, state);
}

[[nodiscard]] JSValue newHashedKeylet(JSContext *ctx, LedgerKeyletKind kind,
                                      std::uint16_t type,
                                      std::uint16_t nameSpace,
                                      std::uint8_t const *payload,
                                      std::size_t payloadSize) {
  std::uint8_t prefix[2] = {
      static_cast<std::uint8_t>(nameSpace >> 8),
      static_cast<std::uint8_t>(nameSpace),
  };
  std::uint8_t digest[64];
  sha_impl::Sha512State hash;
  sha_impl::sha512_init(hash);
  sha_impl::sha512_update(hash, prefix, sizeof(prefix));
  if (payloadSize != 0)
    sha_impl::sha512_update(hash, payload, payloadSize);
  sha_impl::sha512_final(hash, digest);
  return newDirectKeylet(ctx, kind, type, digest);
}

void keyletFinalizer(JSRuntime *runtime, JSValue value) {
  qjs::destroyOpaque<LedgerKeyletState>(runtime, value, keyletClassId);
}

JSClassDef keyletClass = {
    .class_name = "LedgerKeylet",
    .finalizer = keyletFinalizer,
};

[[nodiscard]] JSValue newAccountKeylet(JSContext *ctx,
                                       std::uint8_t const account[20]) {
  return newHashedKeylet(ctx, LedgerKeyletKind::accountRoot,
                         kAccountRootType, kAccountRootType, account, 20);
}

// @binding provider:util.keylet.account
JSValue jsKeyletAccount(JSContext *ctx, JSValueConst, int argc,
                        JSValueConst *argv) {
  std::uint8_t account[20];
  if (argc < 1 || !readAccountIDBytes(
                      ctx, argc > 0 ? argv[0] : JS_UNDEFINED, account))
    return JS_ThrowTypeError(
        ctx, "util.keylet.account: expected a provider AccountID");
  return newAccountKeylet(ctx, account);
}

// @binding provider:util.keylet.uriToken
JSValue jsKeyletUriToken(JSContext* ctx, JSValueConst, int argc,
                         JSValueConst* argv)
{
  NominalPayloadView payload{};
  if (argc < 1 ||
      !detail::readHash256NominalPayload(
          argc > 0 ? argv[0] : JS_UNDEFINED, payload) ||
      payload.size != 32)
    return JS_ThrowTypeError(
        ctx, "util.keylet.uriToken: expected a provider Hash256");
  return newDirectKeylet(ctx, LedgerKeyletKind::uriToken, kUriTokenType,
                         payload.data);
}

// @binding provider:util.keylet.hook
JSValue jsKeyletHook(JSContext *ctx, JSValueConst, int argc,
                     JSValueConst *argv) {
  std::uint8_t account[20];
  if (argc < 1 || !readAccountIDBytes(
                      ctx, argc > 0 ? argv[0] : JS_UNDEFINED, account))
    return JS_ThrowTypeError(
        ctx, "util.keylet.hook: expected a provider AccountID");
  return newHashedKeylet(ctx, LedgerKeyletKind::hookLedger, kHookLedgerType,
                         kHookLedgerType, account, 20);
}

// @binding provider:util.keylet.hookDefinition
JSValue jsKeyletHookDefinition(JSContext *ctx, JSValueConst, int argc,
                               JSValueConst *argv) {
  NominalPayloadView payload{};
  if (argc < 1 ||
      !detail::readHash256NominalPayload(
          argc > 0 ? argv[0] : JS_UNDEFINED, payload) ||
      payload.size != 32)
    return JS_ThrowTypeError(
        ctx, "util.keylet.hookDefinition: expected a provider Hash256");
  return newHashedKeylet(
      ctx, LedgerKeyletKind::hookDefinition, kHookDefinitionType,
      kHookDefinitionType, payload.data, payload.size);
}

// @binding provider:util.keylet.fees
JSValue jsKeyletFees(JSContext *ctx, JSValueConst, int, JSValueConst *) {
  return newHashedKeylet(ctx, LedgerKeyletKind::fees, kFeesType,
                         kFeesNamespace, nullptr, 0);
}

[[nodiscard]] JSValue wrongKeyletLength(JSContext *ctx,
                                        std::uint32_t actual) {
  qjs::OwnedValue error(ctx, bindings::result_error(ctx, "parse"));
  if (error.isException())
    return error.release();
  if (JS_DefinePropertyValueStr(ctx, error.get(), "issue",
                                JS_NewString(ctx, "wrong-length"),
                                JS_PROP_ENUMERABLE) < 0 ||
      JS_DefinePropertyValueStr(ctx, error.get(), "expectedLength",
                                JS_NewUint32(ctx, kKeyletBytes),
                                JS_PROP_ENUMERABLE) < 0 ||
      JS_DefinePropertyValueStr(ctx, error.get(), "actualLength",
                                JS_NewUint32(ctx, actual),
                                JS_PROP_ENUMERABLE) < 0 ||
      !bindings::result_finish(ctx, error.get()))
    return JS_EXCEPTION;
  return bindings::result_failure(ctx, error.release());
}

// @binding provider:LedgerKeylet.fromRaw
JSValue jsKeyletFromRaw(JSContext *ctx, JSValueConst, int argc,
                        JSValueConst *argv) {
  auto bytes = qjs::ByteView::getBinding(
      ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "LedgerKeylet.fromRaw", 0,
      qjs::BytePolicy::bytesLikeOrSTBlob);
  if (!bytes)
    return qjs::pendingOrTypeError(
        ctx, "LedgerKeylet.fromRaw: expected byte input");
  if (bytes.size() != kKeyletBytes)
    return wrongKeyletLength(ctx, bytes.size());
  LedgerKeyletState state;
  std::memcpy(state.bytes.data(), bytes.data(), state.bytes.size());
  return bindings::result_success(ctx, newKeylet(ctx, state));
}

// @binding provider:LedgerKeylet.byteLength
JSValue jsKeyletByteLength(JSContext *ctx, JSValueConst thisValue) {
  return keyletState(ctx, thisValue) == nullptr
             ? JS_EXCEPTION
             : JS_NewUint32(ctx, kKeyletBytes);
}

// @binding provider:LedgerKeylet.type
JSValue jsKeyletType(JSContext *ctx, JSValueConst thisValue) {
  auto const* state = keyletState(ctx, thisValue);
  if (state == nullptr)
    return JS_EXCEPTION;
  return JS_NewUint32(ctx, keyletType(*state));
}

// @binding provider:LedgerKeylet.toBytes
JSValue jsKeyletToBytes(JSContext *ctx, JSValueConst thisValue, int,
                        JSValueConst *) {
  auto *state = keyletState(ctx, thisValue);
  if (state == nullptr)
    return JS_EXCEPTION;
  return qjs::uint8Array(ctx, state->bytes);
}

// @binding provider:LedgerKeylet.toHex
JSValue jsKeyletToHex(JSContext *ctx, JSValueConst thisValue, int,
                      JSValueConst *) {
  auto *state = keyletState(ctx, thisValue);
  if (state == nullptr)
    return JS_EXCEPTION;
  char encoded[kKeyletBytes * 2 + 1];
  constexpr char digits[] = "0123456789ABCDEF";
  for (std::size_t i = 0; i < state->bytes.size(); ++i) {
    encoded[i * 2] = digits[state->bytes[i] >> 4];
    encoded[i * 2 + 1] = digits[state->bytes[i] & 0x0f];
  }
  encoded[kKeyletBytes * 2] = '\0';
  return JS_NewStringLen(ctx, encoded, kKeyletBytes * 2);
}

JSCFunctionListEntry const keyletPrototype[] = {
    JS_CGETSET_DEF("byteLength", jsKeyletByteLength, nullptr),
    JS_CGETSET_DEF("type", jsKeyletType, nullptr),
    JS_CFUNC_DEF("toBytes", 0, jsKeyletToBytes),
    JS_CFUNC_DEF("toHex", 0, jsKeyletToHex),
};

JSCFunctionListEntry const keyletNamespace[] = {
    JS_CFUNC_DEF("account", 1, jsKeyletAccount),
    JS_CFUNC_DEF("uriToken", 1, jsKeyletUriToken),
    JS_CFUNC_DEF("hook", 1, jsKeyletHook),
    JS_CFUNC_DEF("hookDefinition", 1, jsKeyletHookDefinition),
    JS_CFUNC_DEF("fees", 0, jsKeyletFees),
};

JSCFunctionListEntry const keyletStatics[] = {
    JS_CFUNC_DEF("fromRaw", 1, jsKeyletFromRaw),
};

} // namespace

bool registerLedgerKeylet(JSContext *ctx) {
  return registerHiddenClass(
      ctx, &keyletClassId, &keyletClass, keyletPrototype,
      qjs::ByteClassFamily::serializedType, jsKeyletToBytes);
}

bool publishLedgerKeyletType(JSContext *ctx, JSValueConst global) {
  qjs::OwnedValue noun(ctx, JS_NewObject(ctx));
  if (noun.isException() ||
      !qjs::installFunctions(ctx, noun.get(), keyletStatics) ||
      !installRuntimeTypeClassifier(
          ctx, noun.get(), RuntimeTypeId::ledgerKeylet) ||
      !qjs::freezeObject(ctx, noun.get()))
    return false;
  return JS_SetPropertyStr(ctx, global, "LedgerKeylet", noun.release()) >= 0;
}

bool installLedgerKeyletNamespace(JSContext *ctx, JSValueConst util) {
  // @binding provider:util.keylet
  qjs::OwnedValue keylet(ctx, JS_NewObject(ctx));
  if (keylet.isException() ||
      !qjs::installFunctions(ctx, keylet.get(), keyletNamespace) ||
      !qjs::freezeObject(ctx, keylet.get()))
    return false;
  return JS_DefinePropertyValueStr(ctx, util, "keylet", keylet.release(),
                                   JS_PROP_C_W_E) >= 0;
}

bool isLedgerKeylet(JSValueConst value) noexcept {
  if (!JS_IsObject(value) || JS_GetClassID(value) != keyletClassId)
    return false;
  auto const *state = static_cast<LedgerKeyletState const *>(
      JS_GetOpaque(value, keyletClassId));
  if (state == nullptr)
    return false;
  switch (state->kind) {
  case LedgerKeyletKind::generic:
    return true;
  case LedgerKeyletKind::accountRoot:
    return keyletType(*state) == kAccountRootType;
  case LedgerKeyletKind::uriToken:
    return keyletType(*state) == kUriTokenType;
  case LedgerKeyletKind::hookLedger:
    return keyletType(*state) == kHookLedgerType;
  case LedgerKeyletKind::hookDefinition:
    return keyletType(*state) == kHookDefinitionType;
  case LedgerKeyletKind::fees:
    return keyletType(*state) == kFeesType;
  }
  return false;
}

bool readLedgerKeylet(JSValueConst value, std::uint8_t bytes[34],
                      LedgerKeyletKind *kind) noexcept {
  if (bytes == nullptr || kind == nullptr || !isLedgerKeylet(value))
    return false;
  auto const *state = static_cast<LedgerKeyletState const *>(
      JS_GetOpaque(value, keyletClassId));
  std::memcpy(bytes, state->bytes.data(), state->bytes.size());
  *kind = state->kind;
  return true;
}

} // namespace jshookz::provider::types
