#include "runtime_type.hpp"

#include "amount/amount_js.hpp"
#include "js.hpp"
#include "leaf/leaf.hpp"
#include "object/field_js.hpp"
#include "object/object.hpp"
#include "pathset/pathset_js.hpp"
#include "quickjs.hpp"
#include "result.hpp"

namespace jshookz::provider::types {
namespace {

[[nodiscard]] bool isHash(JSValueConst value) noexcept {
  return isHash256(value) || isRichLeaf(value, RichLeafKind::hash128) ||
         isRichLeaf(value, RichLeafKind::hash160) ||
         isRichLeaf(value, RichLeafKind::hash192);
}

JSValue runtimeHasInstance(JSContext *ctx, JSValueConst, int argc,
                           JSValueConst *argv, int magic) {
  if (argc == 0)
    return JS_FALSE;
  auto const first = static_cast<std::int32_t>(RuntimeTypeId::accountID);
  auto const last = static_cast<std::int32_t>(RuntimeTypeId::xflDecimal);
  if (magic < first || magic > last)
    return JS_FALSE;
  return JS_NewBool(
      ctx, runtimeTypeClassifies(static_cast<RuntimeTypeId>(magic), argv[0]));
}

} // namespace

bool runtimeTypeClassifies(RuntimeTypeId type, JSValueConst value) noexcept {
  switch (type) {
  case RuntimeTypeId::accountID:
    return isAccountID(value);
  case RuntimeTypeId::amount:
    return isAmount(value);
  case RuntimeTypeId::currency:
    return isRichLeaf(value, RichLeafKind::currency);
  case RuntimeTypeId::hash:
    return isHash(value);
  case RuntimeTypeId::hash128:
    return isRichLeaf(value, RichLeafKind::hash128);
  case RuntimeTypeId::hash160:
    return isRichLeaf(value, RichLeafKind::hash160);
  case RuntimeTypeId::hash192:
    return isRichLeaf(value, RichLeafKind::hash192);
  case RuntimeTypeId::hash256:
    return isHash256(value);
  case RuntimeTypeId::iouAmount:
    return isAmountKind(value, AmountIssueKind::iou);
  case RuntimeTypeId::issue:
    return isRichLeaf(value, RichLeafKind::issue);
  case RuntimeTypeId::mptAmount:
    return isAmountKind(value, AmountIssueKind::mpt);
  case RuntimeTypeId::nativeAmount:
    return isAmountKind(value, AmountIssueKind::native);
  case RuntimeTypeId::path:
    return isPath(value);
  case RuntimeTypeId::pathHop:
    return isPathHop(value);
  case RuntimeTypeId::pathSet:
    return isPathSet(value);
  case RuntimeTypeId::result:
    return bindings::isResult(value) && !bindings::isEffectResult(value);
  case RuntimeTypeId::stArray:
    return isSTArray(value);
  case RuntimeTypeId::stBlob:
    return isSTBlob(value);
  case RuntimeTypeId::stObject:
    return isSTObject(value);
  case RuntimeTypeId::serializedField:
    return isSerializedField(value);
  case RuntimeTypeId::uInt:
    return isUInt(value, 0);
  case RuntimeTypeId::uInt8:
    return isUInt(value, 8);
  case RuntimeTypeId::uInt16:
    return isUInt(value, 16);
  case RuntimeTypeId::uInt32:
    return isUInt(value, 32);
  case RuntimeTypeId::uInt64:
    return isUInt(value, 64);
  case RuntimeTypeId::vector256:
    return isRichLeaf(value, RichLeafKind::vector256);
  case RuntimeTypeId::voidResult:
    return bindings::isEffectResult(value);
  case RuntimeTypeId::xChainBridge:
    return isRichLeaf(value, RichLeafKind::xChainBridge);
  case RuntimeTypeId::xflDecimal:
    return isXFLDecimal(value);
  }
  return false;
}

bool installRuntimeTypeClassifier(JSContext *ctx, JSValueConst target,
                                  RuntimeTypeId type) {
  if (ctx == nullptr || !JS_IsObject(target))
    return false;
  qjs::OwnedValue classifier(ctx, JS_NewCFunctionMagic(ctx, runtimeHasInstance,
                                                       "hasInstance", 1,
                                                       JS_CFUNC_generic_magic,
                                                       static_cast<int>(type)));
  if (classifier.isException())
    return false;
  qjs::OwnedValue global(ctx, JS_GetGlobalObject(ctx));
  qjs::OwnedValue symbolConstructor(
      ctx, global.isException()
               ? JS_EXCEPTION
               : JS_GetPropertyStr(ctx, global.get(), "Symbol"));
  qjs::OwnedValue symbol(
      ctx,
      symbolConstructor.isException()
          ? JS_EXCEPTION
          : JS_GetPropertyStr(ctx, symbolConstructor.get(), "hasInstance"));
  if (global.isException() || symbolConstructor.isException() ||
      symbol.isException())
    return false;
  JSAtom const atom = JS_ValueToAtom(ctx, symbol.get());
  if (atom == JS_ATOM_NULL)
    return false;
  int const status =
      JS_DefinePropertyValue(ctx, target, atom, classifier.release(), 0);
  JS_FreeAtom(ctx, atom);
  return status >= 0;
}

bool publishRuntimeType(JSContext *ctx, JSValueConst global, char const *name,
                        RuntimeTypeId type) {
  if (ctx == nullptr || name == nullptr || !JS_IsObject(global))
    return false;
  JSAtom const nameAtom = JS_NewAtom(ctx, name);
  if (nameAtom == JS_ATOM_NULL)
    return false;
  qjs::OwnedValue noun(ctx, JS_NewObject(ctx));
  if (noun.isException() ||
      !installRuntimeTypeClassifier(ctx, noun.get(), type) ||
      JS_PreventExtensions(ctx, noun.get()) != 1) {
    JS_FreeAtom(ctx, nameAtom);
    return false;
  }
  int const status = JS_DefinePropertyValue(ctx, global, nameAtom,
                                            noun.release(), JS_PROP_C_W_E);
  JS_FreeAtom(ctx, nameAtom);
  return status >= 0;
}

} // namespace jshookz::provider::types
