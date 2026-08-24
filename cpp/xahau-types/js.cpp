#include "js.hpp"

#include "amount/amount_js.hpp"
#include "leaf/leaf.hpp"
#include "object/field_js.hpp"
#include "object/object.hpp"
#include "pathset/pathset_js.hpp"

#include <cstdint>
#include <cstring>

namespace jshookz::provider::types {
namespace qjs = jshookz::provider::qjs;

bool
registerHiddenClass(
    JSContext* ctx,
    JSClassID* class_id,
    JSClassDef const* class_def,
    std::span<JSCFunctionListEntry const> prototypeFunctions,
    qjs::ByteClassFamily byteFamily,
    JSCFunction* toBytes)
{
    return jshookz::qjs::defineClass(
               JS_GetRuntime(ctx), class_id, class_def) &&
        qjs::registerByteClass(*class_id, byteFamily, toBytes) &&
        jshookz::qjs::installPrototype(
               ctx, *class_id, prototypeFunctions) &&
        !JS_HasException(ctx);
}

bool
registerClass(
    JSContext* ctx,
    JSValueConst global,
    char const* name,
    JSClassID* class_id,
    JSClassDef const* class_def,
    std::span<JSCFunctionListEntry const> prototypeFunctions,
    std::span<JSCFunctionListEntry const> staticFunctions,
    qjs::ByteClassFamily byteFamily,
    JSCFunction* toBytes,
    FactoryInitializer initializeFactory)
{
    if (!registerHiddenClass(
            ctx,
            class_id,
            class_def,
            prototypeFunctions,
            byteFamily,
            toBytes))
        return false;

    qjs::OwnedValue factory(ctx, JS_NewObject(ctx));
    if (factory.isException() ||
        !qjs::installFunctions(ctx, factory.get(), staticFunctions) ||
        (initializeFactory != nullptr &&
         !initializeFactory(ctx, factory.get())) ||
        !qjs::freezeObject(ctx, factory.get()))
        return false;
    return JS_SetPropertyStr(ctx, global, name, factory.release()) >= 0;
}

}  // namespace jshookz::provider::types

namespace {

namespace qjs = jshookz::provider::qjs;
namespace types = jshookz::provider::types;

[[nodiscard]] JSValue makeIssueForAmount(JSContext *ctx, JSValueConst owner,
                                         types::AmountIssueKind kind,
                                         std::uint8_t const *identity,
                                         std::uint32_t length) {
  if (kind == types::AmountIssueKind::native) {
    std::uint8_t native[20] = {};
    return JS_IsUndefined(owner)
               ? types::makeIssueBytes(ctx, native, sizeof(native))
               : types::makeIssueDerivedBytes(ctx, owner, native,
                                              sizeof(native));
  }
  if (kind == types::AmountIssueKind::iou)
    return JS_IsUndefined(owner)
               ? types::makeIssueBytes(ctx, identity, length)
               : types::makeIssueView(ctx, owner, identity, length);
  if (identity == nullptr || length != 24)
    return JS_ThrowInternalError(ctx, "invalid certified MPT issue identity");
  std::uint8_t issue[44] = {};
  std::memcpy(issue, identity + 4, 20);
  issue[39] = 1;
  issue[40] = identity[3];
  issue[41] = identity[2];
  issue[42] = identity[1];
  issue[43] = identity[0];
  return JS_IsUndefined(owner)
             ? types::makeIssueBytes(ctx, issue, sizeof(issue))
             : types::makeIssueDerivedBytes(ctx, owner, issue, sizeof(issue));
}

[[nodiscard]] JSValue makeAccountIDForOwner(JSContext *ctx, JSValueConst owner,
                                            std::uint8_t const *bytes,
                                            std::uint32_t length) {
  return JS_IsUndefined(owner)
             ? types::makeAccountIDBytes(ctx, bytes, length)
             : types::makeAccountIDView(ctx, owner, bytes, length);
}

[[nodiscard]] JSValue makeCurrencyForOwner(JSContext *ctx, JSValueConst owner,
                                           std::uint8_t const *bytes,
                                           std::uint32_t length) {
  return JS_IsUndefined(owner)
             ? types::makeCurrencyBytes(ctx, bytes, length)
             : types::makeCurrencyView(ctx, owner, bytes, length);
}

[[nodiscard]] JSValue makeHash192ForOwner(JSContext *ctx, JSValueConst owner,
                                          std::uint8_t const *bytes,
                                          std::uint32_t length) {
  return JS_IsUndefined(owner)
             ? types::makeHash192Bytes(ctx, bytes, length)
             : types::makeHash192View(ctx, owner, bytes, length);
}

[[nodiscard]] JSValueConst
firstArgument(int argc, JSValueConst* argv) noexcept
{
    return argc > 0 ? argv[0] : JS_UNDEFINED;
}

[[nodiscard]] JSValue
validateObject(
    JSContext* ctx,
    JSValueConst,
    int argc,
    JSValueConst* argv)
{
    return types::validateObjectBytes(ctx, firstArgument(argc, argv));
}

[[nodiscard]] JSValue
safeDecodeObject(
    JSContext* ctx,
    JSValueConst,
    int argc,
    JSValueConst* argv)
{
    return types::safeDecodeObjectBytes(ctx, firstArgument(argc, argv));
}

[[nodiscard]] JSValue
decodeObject(
    JSContext* ctx,
    JSValueConst,
    int argc,
    JSValueConst* argv)
{
    return types::decodeObjectBytes(ctx, firstArgument(argc, argv));
}

JSCFunctionListEntry const utilFunctions[] = {
    JS_CFUNC_DEF("validateObject", 1, validateObject),
    JS_CFUNC_DEF("safeDecodeObject", 1, safeDecodeObject),
    JS_CFUNC_DEF("decodeObject", 1, decodeObject),
};

} // namespace

extern "C" bool register_cpp_types(JSContext *ctx) {
  qjs::resetByteClassRegistry();
  qjs::OwnedValue global(ctx, JS_GetGlobalObject(ctx));
  if (global.isException())
    return false;
  types::AmountLeafMaterializers const amountLeaves{
      makeAccountIDForOwner,      makeCurrencyForOwner, makeHash192ForOwner,
      types::makeXFLDecimalParts, makeIssueForAmount,
  };
  types::PathSetLeafMaterializers const pathLeaves{
      makeAccountIDForOwner,
      makeCurrencyForOwner,
      types::isCertifiedObjectRange,
  };
  return types::registerSTBlob(ctx, global.get()) &&
         types::registerHash256(ctx, global.get()) &&
         types::registerAccountID(ctx, global.get()) &&
         types::registerXFL(ctx) && types::registerRichLeafTypes(ctx) &&
         types::registerAmount(ctx, amountLeaves) &&
         types::registerObjectTypes(ctx) &&
         types::registerPathSet(ctx, pathLeaves) &&
         types::registerFieldDescriptors(ctx, global.get()) &&
         jshookz::qjs::installFactory(ctx, global.get(), "util", utilFunctions);
}

extern "C" void
unregister_cpp_types(JSRuntime* runtime)
{
    types::unregisterObjectTypes(runtime);
    types::unregisterRichLeafTypes(runtime);
    qjs::resetByteClassRegistry();
}
