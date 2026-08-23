#include "field_js.hpp"

#include "quickjs.hpp"

#include "catl/xdata/static_protocol.h"

#include <cstdint>

namespace jshookz::provider::types {
namespace {

namespace qjs = ::jshookz::qjs;
namespace xdata = catl::xdata;

JSClassID fieldClassId;

struct FieldState {
  std::uint32_t code = 0;
};

void fieldFinalizer(JSRuntime *runtime, JSValue value) {
  auto *state = static_cast<FieldState *>(JS_GetOpaque(value, fieldClassId));
  js_free_rt(runtime, state);
}

JSClassDef fieldClass = {
    .class_name = "SerializedField",
    .finalizer = fieldFinalizer,
};

[[nodiscard]] FieldState *fieldState(JSContext *ctx, JSValueConst value) {
  return static_cast<FieldState *>(JS_GetOpaque2(ctx, value, fieldClassId));
}

[[nodiscard]] JSValue fieldCode(JSContext *ctx, JSValueConst value) {
  auto const *state = fieldState(ctx, value);
  return state == nullptr ? JS_EXCEPTION : JS_NewUint32(ctx, state->code);
}

[[nodiscard]] JSValue fieldTypeCode(JSContext *ctx, JSValueConst value) {
  auto const *state = fieldState(ctx, value);
  return state == nullptr ? JS_EXCEPTION : JS_NewUint32(ctx, state->code >> 16);
}

[[nodiscard]] JSValue fieldNth(JSContext *ctx, JSValueConst value) {
  auto const *state = fieldState(ctx, value);
  return state == nullptr ? JS_EXCEPTION
                          : JS_NewUint32(ctx, state->code & 0xffffu);
}

JSCFunctionListEntry const fieldPrototype[] = {
    JS_CGETSET_DEF("code", fieldCode, nullptr),
    JS_CGETSET_DEF("typeCode", fieldTypeCode, nullptr),
    JS_CGETSET_DEF("fieldCode", fieldNth, nullptr),
};

[[nodiscard]] JSValue newField(JSContext *ctx, std::uint32_t code) {
  JSValue value = JS_NewObjectClass(ctx, fieldClassId);
  if (JS_IsException(value))
    return value;
  auto *state = static_cast<FieldState *>(js_malloc(ctx, sizeof(FieldState)));
  if (state == nullptr) {
    JS_FreeValue(ctx, value);
    return JS_ThrowOutOfMemory(ctx);
  }
  state->code = code;
  JS_SetOpaque(value, state);
  if (JS_PreventExtensions(ctx, value) < 0) {
    JS_FreeValue(ctx, value);
    return JS_EXCEPTION;
  }
  return value;
}

} // namespace

bool registerFieldDescriptors(JSContext *ctx, JSValueConst global) {
  if (!qjs::defineClass(JS_GetRuntime(ctx), &fieldClassId, &fieldClass) ||
      !qjs::installPrototype(ctx, fieldClassId, fieldPrototype))
    return false;

  qjs::OwnedValue fields(ctx, JS_NewObject(ctx));
  if (fields.isException())
    return false;
  auto const &protocol = xdata::xahau_static_protocol();
  for (std::uint32_t i = 0; i < protocol.material_field_count; ++i) {
    auto const *material = protocol.material_field(i);
    auto const *descriptor = material == nullptr
                                 ? nullptr
                                 : protocol.field_by_code(material->field_code);
    if (descriptor == nullptr)
      return JS_ThrowInternalError(ctx,
                                   "generated material field is unavailable"),
             false;
    auto const name = protocol.field_name(descriptor->name_ordinal);
    JSAtom const atom = JS_NewAtomLen(ctx, name.data, name.size);
    if (atom == JS_ATOM_NULL) {
      if (!JS_HasException(ctx))
        JS_ThrowOutOfMemory(ctx);
      return false;
    }
    qjs::OwnedValue value(ctx, newField(ctx, material->field_code));
    if (value.isException() ||
        JS_DefinePropertyValue(ctx, fields.get(), atom, value.release(),
                               JS_PROP_ENUMERABLE) < 0) {
      JS_FreeAtom(ctx, atom);
      return false;
    }
    JS_FreeAtom(ctx, atom);
  }
  if (!qjs::freezeObject(ctx, fields.get()))
    return false;
  return JS_SetPropertyStr(ctx, global, "Field", fields.release()) >= 0;
}

bool serializedFieldCode(JSValueConst value, std::uint32_t &code) noexcept {
  if (!JS_IsObject(value) || JS_GetClassID(value) != fieldClassId)
    return false;
  auto const *state =
      static_cast<FieldState const *>(JS_GetOpaque(value, fieldClassId));
  if (state == nullptr)
    return false;
  code = state->code;
  return xdata::xahau_static_protocol().field_by_code(code) != nullptr;
}

} // namespace jshookz::provider::types
