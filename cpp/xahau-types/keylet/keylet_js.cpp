#include "keylet_js.hpp"

#include "js.hpp"
#include "object/nominal_payload.hpp"
#include "quickjs.hpp"

#include "sha_impl.h"

#include <array>
#include <cstring>

namespace jshookz::provider::types {
namespace {

namespace qjs = jshookz::provider::qjs;

constexpr std::uint16_t kAccountRootType = 0x0061;
constexpr std::uint16_t kUriTokenType = 0x0055;
constexpr std::uint32_t kKeyletBytes = 34;

JSClassID keyletClassId;

struct LedgerKeyletState {
  std::array<std::uint8_t, kKeyletBytes> bytes{};
  LedgerKeyletKind kind = LedgerKeyletKind::accountRoot;
};

[[nodiscard]] LedgerKeyletState *keyletState(JSContext *ctx,
                                             JSValueConst value) {
  return qjs::opaque<LedgerKeyletState>(ctx, value, keyletClassId);
}

[[nodiscard]] JSValue newUriTokenKeylet(
    JSContext* ctx, std::uint8_t const tokenId[32])
{
  LedgerKeyletState state;
  state.kind = LedgerKeyletKind::uriToken;
  state.bytes[0] = 0x00;
  state.bytes[1] = 0x55;
  std::memcpy(state.bytes.data() + 2, tokenId, 32);
  return nativeNew<LedgerKeyletState>(ctx, keyletClassId, state);
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
  std::uint8_t preimage[22] = {0x00, 0x61};
  std::memcpy(preimage + 2, account, 20);

  std::uint8_t digest[64];
  sha_impl::Sha512State hash;
  sha_impl::sha512_init(hash);
  sha_impl::sha512_update(hash, preimage, sizeof(preimage));
  sha_impl::sha512_final(hash, digest);

  LedgerKeyletState state;
  state.bytes[0] = 0x00;
  state.bytes[1] = 0x61;
  std::memcpy(state.bytes.data() + 2, digest, 32);
  return nativeNew<LedgerKeyletState>(ctx, keyletClassId, state);
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
  return newUriTokenKeylet(ctx, payload.data);
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
  return JS_NewUint32(
      ctx,
      state->kind == LedgerKeyletKind::uriToken
          ? kUriTokenType
          : kAccountRootType);
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
};

} // namespace

bool registerLedgerKeylet(JSContext *ctx) {
  return registerHiddenClass(
      ctx, &keyletClassId, &keyletClass, keyletPrototype,
      qjs::ByteClassFamily::serializedType, jsKeyletToBytes);
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
  if (state == nullptr || state->bytes[0] != 0x00)
    return false;
  switch (state->kind) {
  case LedgerKeyletKind::accountRoot:
    return state->bytes[1] == 0x61;
  case LedgerKeyletKind::uriToken:
    return state->bytes[1] == 0x55;
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
