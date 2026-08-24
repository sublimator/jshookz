#include "field_js.hpp"

#include "object.hpp"
#include "quickjs.hpp"

#include "catl/xdata/static_protocol.h"

#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
#include "tests/object_gc_lifetime_probe_hooks.hpp"
#endif

#include <cstdint>

namespace jshookz::provider::types {
namespace {

namespace qjs = ::jshookz::qjs;
namespace xdata = catl::xdata;

JSClassID fieldClassId;
JSClassID fieldTableClassId;

struct alignas(void *) FieldTableState {
  void *cache[1];
};

#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
void fieldDescriptorFinalizer(JSRuntime *, JSValue value) {
  test::gcProbeFinalized(test::TrackedEntity::fieldDescriptor, value);
}
#endif

JSClassDef fieldClass = {
    .class_name = "SerializedField",
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
    .finalizer = fieldDescriptorFinalizer,
#endif
};

[[nodiscard]] xdata::StaticMaterialField const *
fieldState(JSContext *ctx, JSValueConst value) {
  return static_cast<xdata::StaticMaterialField const *>(
      JS_GetOpaque2(ctx, value, fieldClassId));
}

[[nodiscard]] JSValue fieldCode(JSContext *ctx, JSValueConst value) {
  auto const *state = fieldState(ctx, value);
  return state == nullptr ? JS_EXCEPTION
                          : JS_NewUint32(ctx, state->field_code);
}

[[nodiscard]] JSValue fieldTypeCode(JSContext *ctx, JSValueConst value) {
  auto const *state = fieldState(ctx, value);
  return state == nullptr ? JS_EXCEPTION
                          : JS_NewUint32(ctx, state->field_code >> 16);
}

[[nodiscard]] JSValue fieldNth(JSContext *ctx, JSValueConst value) {
  auto const *state = fieldState(ctx, value);
  return state == nullptr ? JS_EXCEPTION
                          : JS_NewUint32(ctx, state->field_code & 0xffffu);
}

JSCFunctionListEntry const fieldPrototype[] = {
    JS_CGETSET_DEF("code", fieldCode, nullptr),
    JS_CGETSET_DEF("typeCode", fieldTypeCode, nullptr),
    JS_CGETSET_DEF("fieldCode", fieldNth, nullptr),
};

[[nodiscard]] JSValue
newField(JSContext *ctx, xdata::StaticMaterialField const *material) {
  if (material == nullptr)
    return JS_ThrowInternalError(ctx, "generated material field is unavailable");
  JSValue value = JS_NewObjectClass(ctx, fieldClassId);
  if (JS_IsException(value))
    return value;
  JS_SetOpaque(value, const_cast<xdata::StaticMaterialField *>(material));
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
  test::gcProbeCreated(test::TrackedEntity::fieldDescriptor, value);
  if (!test::gcProbePlantPendingCycle(
          ctx, test::HiddenEdge::fieldTableDescriptor, value)) {
    JS_FreeValue(ctx, value);
    return JS_EXCEPTION;
  }
#endif
  if (JS_PreventExtensions(ctx, value) < 0) {
    JS_FreeValue(ctx, value);
    return JS_EXCEPTION;
  }
  return value;
}

void fieldTableFinalizer(JSRuntime *runtime, JSValue value) {
  auto *state = static_cast<FieldTableState *>(
      JS_GetOpaque(value, fieldTableClassId));
  if (state == nullptr)
    return;
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
  test::gcProbeFinalized(test::TrackedEntity::fieldTable, value);
#endif
  auto const count = xdata::xahau_static_protocol().material_field_count;
  for (std::uint32_t i = 0; i < count; ++i) {
    if (state->cache[i] != nullptr)
      JS_FreeValueRT(runtime, JS_MKPTR(JS_TAG_OBJECT, state->cache[i]));
  }
  js_free_rt(runtime, state);
}

void fieldTableMark(JSRuntime *runtime, JSValueConst value, JS_MarkFunc *mark) {
  auto const *state = static_cast<FieldTableState const *>(
      JS_GetOpaque(value, fieldTableClassId));
  if (state == nullptr)
    return;
  auto const count = xdata::xahau_static_protocol().material_field_count;
  for (std::uint32_t i = 0; i < count; ++i) {
    if (state->cache[i] != nullptr) {
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
      if (!test::gcProbeMarkEnabled(
              test::HiddenEdge::fieldTableDescriptor))
        continue;
#endif
      JS_MarkValue(runtime, JS_MKPTR(JS_TAG_OBJECT, state->cache[i]), mark);
    }
  }
}

[[nodiscard]] FieldTableState *
fieldTableState(JSContext *ctx, JSValueConst value) {
  return static_cast<FieldTableState *>(
      JS_GetOpaque2(ctx, value, fieldTableClassId));
}

[[nodiscard]] JSValue
fieldTableValue(JSContext *ctx, FieldTableState &state,
                std::uint16_t materialOrdinal) {
  auto const &protocol = xdata::xahau_static_protocol();
  if (materialOrdinal >= protocol.material_field_count)
    return JS_ThrowInternalError(ctx, "material field ordinal is unavailable");
  if (state.cache[materialOrdinal] == nullptr) {
    JSValue value = newField(ctx, protocol.material_field(materialOrdinal));
    if (JS_IsException(value))
      return value;
    state.cache[materialOrdinal] = JS_VALUE_GET_PTR(value);
  }
  return JS_DupValue(
      ctx, JS_MKPTR(JS_TAG_OBJECT, state.cache[materialOrdinal]));
}

[[nodiscard]] int
fieldTableOwnProperty(JSContext *ctx, JSPropertyDescriptor *property,
                      JSValueConst value, JSAtom atom) {
  if (property != nullptr) {
    property->flags = 0;
    property->value = JS_UNDEFINED;
    property->getter = JS_UNDEFINED;
    property->setter = JS_UNDEFINED;
  }
  auto *state = fieldTableState(ctx, value);
  if (state == nullptr)
    return -1;
  std::uint32_t code = 0;
  if (!registeredFieldCode(atom, code))
    return 0;
  auto const *descriptor = xdata::xahau_static_protocol().field_by_code(code);
  if (descriptor == nullptr ||
      descriptor->material_ordinal == xdata::ProtocolView::no_ordinal)
    return 0;
  if (property != nullptr) {
    JSValue observed =
        fieldTableValue(ctx, *state, descriptor->material_ordinal);
    if (JS_IsException(observed))
      return -1;
    property->value = observed;
    property->flags = JS_PROP_ENUMERABLE;
  }
  return 1;
}

[[nodiscard]] int
fieldTableOwnNames(JSContext *ctx, JSPropertyEnum **table,
                   std::uint32_t *length, JSValueConst value) {
  *table = nullptr;
  *length = 0;
  if (fieldTableState(ctx, value) == nullptr)
    return -1;
  auto const &protocol = xdata::xahau_static_protocol();
  auto const count = static_cast<std::uint32_t>(protocol.material_field_count);
  auto *names = static_cast<JSPropertyEnum *>(
      js_malloc(ctx, static_cast<std::size_t>(count) * sizeof(JSPropertyEnum)));
  if (names == nullptr)
    return -1;
  for (std::uint32_t i = 0; i < count; ++i) {
    auto const *material = protocol.material_field(i);
    auto const *descriptor = material == nullptr
        ? nullptr : protocol.field_by_code(material->field_code);
    if (descriptor == nullptr) {
      for (std::uint32_t j = 0; j < i; ++j)
        JS_FreeAtom(ctx, names[j].atom);
      js_free(ctx, names);
      return -1;
    }
    auto const name = protocol.field_name(descriptor->name_ordinal);
    JSAtom const atom = JS_NewAtomLen(ctx, name.data, name.size);
    if (atom == JS_ATOM_NULL) {
      for (std::uint32_t j = 0; j < i; ++j)
        JS_FreeAtom(ctx, names[j].atom);
      js_free(ctx, names);
      if (!JS_HasException(ctx))
        JS_ThrowOutOfMemory(ctx);
      return -1;
    }
    names[i] = {true, atom};
  }
  *table = names;
  *length = count;
  return 0;
}

[[nodiscard]] int
fieldTableDelete(JSContext *ctx, JSValueConst value, JSAtom atom) {
  if (fieldTableState(ctx, value) == nullptr)
    return -1;
  std::uint32_t code = 0;
  if (!registeredFieldCode(atom, code))
    return 1;
  auto const *descriptor = xdata::xahau_static_protocol().field_by_code(code);
  return descriptor == nullptr ||
      descriptor->material_ordinal == xdata::ProtocolView::no_ordinal;
}

[[nodiscard]] int
fieldTableDefineOwnProperty(JSContext *ctx, JSValueConst value, JSAtom atom,
                            JSValueConst proposed, JSValueConst,
                            JSValueConst, int flags) {
  auto *state = fieldTableState(ctx, value);
  if (state == nullptr)
    return -1;
  std::uint32_t code = 0;
  if (!registeredFieldCode(atom, code))
    return 0;
  auto const *descriptor = xdata::xahau_static_protocol().field_by_code(code);
  if (descriptor == nullptr ||
      descriptor->material_ordinal == xdata::ProtocolView::no_ordinal)
    return 0;
  if ((flags & JS_PROP_HAS_CONFIGURABLE) &&
      (flags & JS_PROP_CONFIGURABLE))
    return 0;
  if ((flags & JS_PROP_HAS_WRITABLE) && (flags & JS_PROP_WRITABLE))
    return 0;
  if ((flags & JS_PROP_HAS_ENUMERABLE) &&
      !(flags & JS_PROP_ENUMERABLE))
    return 0;
  if (flags & (JS_PROP_HAS_GET | JS_PROP_HAS_SET))
    return 0;
  if (flags & JS_PROP_HAS_VALUE) {
    qjs::OwnedValue current(
        ctx, fieldTableValue(ctx, *state, descriptor->material_ordinal));
    if (current.isException())
      return -1;
    if (!JS_SameValue(ctx, current.get(), proposed))
      return 0;
  }
  return 1;
}

JSClassExoticMethods fieldTableExotic = {
    .get_own_property = fieldTableOwnProperty,
    .get_own_property_names = fieldTableOwnNames,
    .delete_property = fieldTableDelete,
    .define_own_property = fieldTableDefineOwnProperty,
};

JSClassDef fieldTableClass = {
    .class_name = "FieldTable",
    .finalizer = fieldTableFinalizer,
    .gc_mark = fieldTableMark,
    .exotic = &fieldTableExotic,
};

[[nodiscard]] bool registrationFailure(JSContext *ctx) {
  if (!JS_HasException(ctx))
    JS_ThrowOutOfMemory(ctx);
  return false;
}

} // namespace

bool registerFieldDescriptors(JSContext *ctx, JSValueConst global) {
  if (!qjs::defineClass(JS_GetRuntime(ctx), &fieldClassId, &fieldClass) ||
      !qjs::installPrototype(ctx, fieldClassId, fieldPrototype) ||
      !qjs::defineClass(JS_GetRuntime(ctx), &fieldTableClassId,
                        &fieldTableClass))
    return registrationFailure(ctx);

  qjs::OwnedValue objectConstructor(
      ctx, JS_GetPropertyStr(ctx, global, "Object"));
  if (objectConstructor.isException())
    return registrationFailure(ctx);
  qjs::OwnedValue objectPrototype(
      ctx, JS_GetPropertyStr(ctx, objectConstructor.get(), "prototype"));
  if (objectPrototype.isException())
    return registrationFailure(ctx);
  JS_SetClassProto(ctx, fieldTableClassId, objectPrototype.release());

  qjs::OwnedValue fields(ctx, JS_NewObjectClass(ctx, fieldTableClassId));
  if (fields.isException())
    return registrationFailure(ctx);
  auto const &protocol = xdata::xahau_static_protocol();
  auto const bytes = sizeof(FieldTableState) +
      static_cast<std::size_t>(protocol.material_field_count - 1) *
          sizeof(void *);
  auto *state = static_cast<FieldTableState *>(js_malloc(ctx, bytes));
  if (state == nullptr)
    return registrationFailure(ctx);
  JS_SetOpaque(fields.get(), state);
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
  test::gcProbeCreated(test::TrackedEntity::fieldTable, fields.get());
#endif

  for (std::uint32_t i = 0; i < protocol.material_field_count; ++i)
    state->cache[i] = nullptr;
  if (JS_PreventExtensions(ctx, fields.get()) < 0)
    return registrationFailure(ctx);
  if (JS_SetPropertyStr(ctx, global, "Field", fields.release()) < 0)
    return registrationFailure(ctx);
  return true;
}

bool serializedFieldCode(JSValueConst value, std::uint32_t &code) noexcept {
  if (!JS_IsObject(value) || JS_GetClassID(value) != fieldClassId)
    return false;
  auto const *state = static_cast<xdata::StaticMaterialField const *>(
      JS_GetOpaque(value, fieldClassId));
  if (state == nullptr)
    return false;
  code = state->field_code;
  return xdata::xahau_static_protocol().field_by_code(code) != nullptr;
}

} // namespace jshookz::provider::types
