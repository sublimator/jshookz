#include "object.hpp"

#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
#include "tests/object_gc_lifetime_probe_hooks.hpp"
#endif

#include "canonical_json.hpp"
#include "field_js.hpp"
#include "nominal_payload.hpp"

#include "amount/amount_js.hpp"
#include "js.hpp"
#include "leaf/leaf.hpp"
#include "pathset/pathset_js.hpp"
#include "quickjs.hpp"
#include "result.hpp"

#include "catl/xdata/canonical_replacement.h"
#include "catl/xdata/canonical_serializer.h"
#include "catl/xdata/number-rules.h"
#include "catl/xdata/recursive_index.h"
#include "catl/xdata/static_protocol.h"
#include "static_xahau_protocol.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

namespace jshookz::provider::types {
namespace {

namespace qjs = ::jshookz::qjs;
namespace xdata = catl::xdata;
namespace static_data = catl::xdata::xahau_static_data;
namespace bindings = jshookz::provider::bindings;

using ObjectView = static_data::ObjectView;
constexpr auto kObjectViewCount =
    static_cast<std::size_t>(ObjectView::object_view_count);

constexpr std::uint32_t kProtocolTag = 1;

JSClassID ownerClassId;
JSClassID objectClassId;
JSClassID arrayClassId;
JSClassID iteratorClassId;

struct CertifiedObjectValue {
  std::uint8_t *bytes = nullptr;
  void *index = nullptr;
  std::uint32_t byteCount = 0;
  std::uint32_t protocolTag = 0;
};

struct ObjectState {
  JSValue owner = JS_UNDEFINED;
  std::uint32_t scopeKey = 0;
  std::uint16_t view = ObjectView::st_object;
  std::uint16_t cacheCount = 0;
  JSValue *cache = nullptr;
};

struct ArrayCacheLeaf {
  JSValue values[32];
};

struct ArrayCacheBranch {
  ArrayCacheLeaf *pages[32];
};

struct ArrayCacheRoot {
  std::uint32_t branchCount = 0;
  std::uint32_t pageCount = 0;
  std::uint32_t valueCount = 0;
  std::uint32_t reservedVersion = 0;
  ArrayCacheBranch *branches[32];
};

struct ArrayState {
  JSValue owner = JS_UNDEFINED;
  std::uint32_t scopeKey = 0;
  ArrayCacheRoot *cache = nullptr;
};

struct IteratorState {
  JSValue array = JS_UNDEFINED;
  std::uint32_t cursor = 0;
};

struct FieldAtomRecord {
  std::uint32_t atom = JS_ATOM_NULL;
  std::uint16_t nameOrdinal = 0;
  std::uint16_t flags = 0;
};

static_assert(sizeof(CertifiedObjectValue) == sizeof(void *) * 2 + 8);
static_assert(offsetof(ObjectState, owner) == 0);
static_assert(offsetof(ObjectState, scopeKey) == sizeof(JSValue));
static_assert(offsetof(ObjectState, view) == sizeof(JSValue) + 4);
static_assert(offsetof(ObjectState, cacheCount) == sizeof(JSValue) + 6);
static_assert(offsetof(ObjectState, cache) == sizeof(JSValue) + 8);
constexpr std::size_t kObjectStateUnpadded =
    sizeof(JSValue) + 8 + sizeof(JSValue *);
static_assert(
    sizeof(ObjectState) ==
    (kObjectStateUnpadded + alignof(ObjectState) - 1) /
        alignof(ObjectState) * alignof(ObjectState));
static_assert(sizeof(FieldAtomRecord) == 8);
static_assert(offsetof(ArrayCacheRoot, branches) == 16);
static_assert(sizeof(ArrayCacheRoot) == 16 + 32 * sizeof(void *));
static_assert(sizeof(ArrayCacheBranch) == 32 * sizeof(void *));
static_assert(sizeof(ArrayCacheLeaf) == 32 * sizeof(JSValue));

// The recursive caps fit both the scope ordinal and direct-field count in
// sixteen bits. Packing both into one uint32 word lets finalizers walk caches
// without consulting an owner that is being torn down. The view and cache-count
// metadata enlarge the Wasm ObjectState from 16 to 24 bytes, but allocate no
// separate cache storage before a lazy value is materialized.
constexpr std::uint32_t kScopeMask = 0xffffu;

[[nodiscard]] constexpr std::uint32_t
makeScopeKey(std::uint32_t scopeId, std::uint32_t directCount) noexcept {
  return scopeId | (directCount << 16);
}

template <class State>
[[nodiscard]] constexpr std::uint32_t scopeId(State const &state) noexcept {
  return state.scopeKey & kScopeMask;
}

template <class State>
[[nodiscard]] constexpr std::uint32_t directCount(State const &state) noexcept {
  return state.scopeKey >> 16;
}

FieldAtomRecord *fieldAtoms;
std::uint32_t fieldAtomCount;
JSAtom diagnosticAtoms[9];
std::array<JSValue, kObjectViewCount> objectPrototypes{};
std::array<JSValue, kObjectViewCount> objectConstructors{};
bool objectNounsReady = false;

constexpr char const *diagnosticNames[] = {
    "domain", "issue", "offset",  "fieldCode",    "expected",
    "actual", "limit", "maximum", "actualAtLeast"};
static_assert(sizeof(diagnosticNames) / sizeof(diagnosticNames[0]) == 9);

[[nodiscard]] JSValue oom(JSContext *ctx) {
  return JS_HasException(ctx) ? JS_EXCEPTION : JS_ThrowOutOfMemory(ctx);
}

[[nodiscard]] bool atomFailure(JSContext *ctx) {
  if (!JS_HasException(ctx))
    JS_ThrowOutOfMemory(ctx);
  return false;
}

void *scanRealloc(void *opaque, void *pointer, std::size_t size) noexcept {
  auto *ctx = static_cast<JSContext *>(opaque);
  if (size == 0) {
    js_free(ctx, pointer);
    return nullptr;
  }
  return js_realloc(ctx, pointer, size);
}

void scanFree(void *opaque, void *pointer) noexcept {
  js_free(static_cast<JSContext *>(opaque), pointer);
}

[[nodiscard]] Slice ownerBytes(CertifiedObjectValue const &owner) noexcept {
  return {owner.bytes, owner.byteCount};
}

[[nodiscard]] xdata::RecursiveIndexView
ownerIndex(CertifiedObjectValue const &owner) noexcept {
  auto const *header = static_cast<xdata::IndexHeader const *>(owner.index);
  std::uint32_t size = 0;
  if (header == nullptr || !xdata::recursive_index_size(
                               header->scope_count, header->field_count, size))
    return {};
  return {owner.index, size, owner.byteCount};
}

[[nodiscard]] CertifiedObjectValue *ownerFrom(JSValueConst value) noexcept {
  auto *owner =
      static_cast<CertifiedObjectValue *>(JS_GetOpaque(value, ownerClassId));
  return owner != nullptr && owner->protocolTag == kProtocolTag ? owner
                                                                : nullptr;
}

[[nodiscard]] CertifiedObjectValue *
ownerFrom(ObjectState const &state) noexcept {
  return ownerFrom(state.owner);
}

[[nodiscard]] CertifiedObjectValue *
ownerFrom(ArrayState const &state) noexcept {
  return ownerFrom(state.owner);
}

void ownerFinalizer(JSRuntime *runtime, JSValue value) {
  auto *owner =
      static_cast<CertifiedObjectValue *>(JS_GetOpaque(value, ownerClassId));
  if (owner == nullptr)
    return;
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
  test::gcProbeFinalized(test::TrackedEntity::owner, value);
#endif
  js_free_rt(runtime, owner->index);
  js_free_rt(runtime, owner->bytes);
  js_free_rt(runtime, owner);
}

void objectFinalizer(JSRuntime *runtime, JSValue value) {
  auto *state = static_cast<ObjectState *>(JS_GetOpaque(value, objectClassId));
  if (state == nullptr)
    return;
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
  test::gcProbeFinalized(test::TrackedEntity::object, value);
#endif
  if (state->cache != nullptr) {
    std::uint32_t const count = state->cacheCount;
    for (std::uint32_t i = 0; i < count; ++i)
      JS_FreeValueRT(runtime, state->cache[i]);
    js_free_rt(runtime, state->cache);
  }
  JS_FreeValueRT(runtime, state->owner);
  js_free_rt(runtime, state);
}

void objectMark(JSRuntime *runtime, JSValueConst value, JS_MarkFunc *mark) {
  auto *state = static_cast<ObjectState *>(JS_GetOpaque(value, objectClassId));
  if (state == nullptr)
    return;
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
  if (test::gcProbeMarkEnabled(test::HiddenEdge::objectOwner))
#endif
    JS_MarkValue(runtime, state->owner, mark);
  if (state->cache == nullptr)
    return;
  std::uint32_t const count = state->cacheCount;
  for (std::uint32_t i = 0; i < count; ++i) {
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
    if (!test::gcProbeMarkEnabled(test::HiddenEdge::objectCacheValue))
      continue;
#endif
    JS_MarkValue(runtime, state->cache[i], mark);
  }
}

void arrayFinalizer(JSRuntime *runtime, JSValue value) {
  auto *state = static_cast<ArrayState *>(JS_GetOpaque(value, arrayClassId));
  if (state == nullptr)
    return;
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
  test::gcProbeFinalized(test::TrackedEntity::array, value);
#endif
  if (state->cache != nullptr) {
    for (auto *branch : state->cache->branches) {
      if (branch == nullptr)
        continue;
      for (auto *leaf : branch->pages) {
        if (leaf == nullptr)
          continue;
        for (JSValue cached : leaf->values)
          JS_FreeValueRT(runtime, cached);
        js_free_rt(runtime, leaf);
      }
      js_free_rt(runtime, branch);
    }
    js_free_rt(runtime, state->cache);
  }
  JS_FreeValueRT(runtime, state->owner);
  js_free_rt(runtime, state);
}

void arrayMark(JSRuntime *runtime, JSValueConst value, JS_MarkFunc *mark) {
  auto *state = static_cast<ArrayState *>(JS_GetOpaque(value, arrayClassId));
  if (state == nullptr)
    return;
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
  if (test::gcProbeMarkEnabled(test::HiddenEdge::arrayOwner))
#endif
    JS_MarkValue(runtime, state->owner, mark);
  if (state->cache == nullptr)
    return;
  for (auto *branch : state->cache->branches) {
    if (branch == nullptr)
      continue;
    for (auto *leaf : branch->pages) {
      if (leaf == nullptr)
        continue;
      for (JSValue cached : leaf->values) {
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
        if (!test::gcProbeMarkEnabled(test::HiddenEdge::arrayCacheValue))
          continue;
#endif
        JS_MarkValue(runtime, cached, mark);
      }
    }
  }
}

void iteratorFinalizer(JSRuntime *runtime, JSValue value) {
  auto *state =
      static_cast<IteratorState *>(JS_GetOpaque(value, iteratorClassId));
  if (state == nullptr)
    return;
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
  test::gcProbeFinalized(test::TrackedEntity::iterator, value);
#endif
  JS_FreeValueRT(runtime, state->array);
  js_free_rt(runtime, state);
}

void iteratorMark(JSRuntime *runtime, JSValueConst value, JS_MarkFunc *mark) {
  auto *state =
      static_cast<IteratorState *>(JS_GetOpaque(value, iteratorClassId));
  if (state != nullptr) {
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
    if (!test::gcProbeMarkEnabled(test::HiddenEdge::iteratorArray))
      return;
#endif
    JS_MarkValue(runtime, state->array, mark);
  }
}

[[nodiscard]] xdata::StaticFieldName const *
fieldNameByAtom(JSAtom atom) noexcept {
  std::uint32_t first = 0;
  std::uint32_t count = fieldAtomCount;
  while (count != 0) {
    auto const step = count / 2;
    auto const index = first + step;
    if (fieldAtoms[index].atom < atom) {
      first = index + 1;
      count -= step + 1;
    } else {
      count = step;
    }
  }
  if (first == fieldAtomCount || fieldAtoms[first].atom != atom)
    return nullptr;
  auto const ordinal = fieldAtoms[first].nameOrdinal;
  auto const &protocol = xdata::xahau_static_protocol();
  return ordinal < protocol.field_name_count ? protocol.field_names + ordinal
                                             : nullptr;
}

void swap(FieldAtomRecord &left, FieldAtomRecord &right) noexcept {
  FieldAtomRecord const temporary = left;
  left = right;
  right = temporary;
}

void siftDown(FieldAtomRecord *records, std::uint32_t root,
              std::uint32_t count) noexcept {
  while (true) {
    auto const child = root * 2 + 1;
    if (child >= count)
      return;
    auto selected = child;
    if (child + 1 < count && records[child].atom < records[child + 1].atom)
      selected = child + 1;
    if (records[root].atom >= records[selected].atom)
      return;
    swap(records[root], records[selected]);
    root = selected;
  }
}

void sortAtoms(FieldAtomRecord *records, std::uint32_t count) noexcept {
  for (std::uint32_t start = count / 2; start != 0; --start)
    siftDown(records, start - 1, count);
  for (std::uint32_t end = count; end > 1; --end) {
    swap(records[0], records[end - 1]);
    siftDown(records, 0, end - 1);
  }
}

void clearRegistrar(JSRuntime *runtime) noexcept {
  if (runtime != nullptr) {
    for (std::uint32_t i = 0; i < fieldAtomCount; ++i)
      JS_FreeAtomRT(runtime, fieldAtoms[i].atom);
    for (JSAtom atom : diagnosticAtoms) {
      if (atom != JS_ATOM_NULL)
        JS_FreeAtomRT(runtime, atom);
    }
    js_free_rt(runtime, fieldAtoms);
  }
  fieldAtoms = nullptr;
  fieldAtomCount = 0;
  std::memset(diagnosticAtoms, 0, sizeof(diagnosticAtoms));
}

void clearObjectNouns(JSRuntime *runtime) noexcept {
  if (objectNounsReady && runtime != nullptr) {
    for (JSValue value : objectPrototypes)
      JS_FreeValueRT(runtime, value);
    for (JSValue value : objectConstructors)
      JS_FreeValueRT(runtime, value);
  }
  objectPrototypes.fill(JS_UNDEFINED);
  objectConstructors.fill(JS_UNDEFINED);
  objectNounsReady = false;
}

[[nodiscard]] bool registerAtoms(JSContext *ctx) {
  auto const &protocol = xdata::xahau_static_protocol();
  auto const bytes = static_cast<std::size_t>(protocol.field_name_count) *
                     sizeof(FieldAtomRecord);
  fieldAtoms = static_cast<FieldAtomRecord *>(js_malloc(ctx, bytes));
  if (fieldAtoms == nullptr)
    return atomFailure(ctx);
  std::memset(fieldAtoms, 0, bytes);
  for (std::uint32_t i = 0; i < protocol.field_name_count; ++i) {
    auto const name = protocol.field_name(i);
    JSAtom const atom = JS_NewAtomLen(ctx, name.data, name.size);
    if (atom == JS_ATOM_NULL) {
      clearRegistrar(JS_GetRuntime(ctx));
      return atomFailure(ctx);
    }
    fieldAtoms[fieldAtomCount++] = {atom, static_cast<std::uint16_t>(i),
                                    protocol.field_names[i].flags};
  }
  sortAtoms(fieldAtoms, fieldAtomCount);
  for (std::uint32_t i = 0; i < 9; ++i) {
    diagnosticAtoms[i] = JS_NewAtom(ctx, diagnosticNames[i]);
    if (diagnosticAtoms[i] == JS_ATOM_NULL) {
      clearRegistrar(JS_GetRuntime(ctx));
      return atomFailure(ctx);
    }
  }
  return true;
}

[[nodiscard]] JSAtom newLengthAtom(JSContext *ctx) {
  JSAtom const atom = JS_NewAtom(ctx, "length");
  if (atom == JS_ATOM_NULL)
    (void)atomFailure(ctx);
  return atom;
}

[[nodiscard]] bool isLengthAtom(JSContext *ctx, JSAtom candidate,
                                bool &result) {
  JSAtom const atom = newLengthAtom(ctx);
  if (atom == JS_ATOM_NULL)
    return false;
  result = candidate == atom;
  JS_FreeAtom(ctx, atom);
  return true;
}

[[nodiscard]] ObjectState *objectState(JSContext *ctx, JSValueConst value) {
  auto *state =
      static_cast<ObjectState *>(JS_GetOpaque2(ctx, value, objectClassId));
  if (state == nullptr || ownerFrom(*state) == nullptr) {
    if (state != nullptr)
      JS_ThrowTypeError(ctx, "STObject: invalid provider provenance");
    return nullptr;
  }
  return state;
}

[[nodiscard]] ArrayState *arrayState(JSContext *ctx, JSValueConst value) {
  auto *state =
      static_cast<ArrayState *>(JS_GetOpaque2(ctx, value, arrayClassId));
  if (state == nullptr || ownerFrom(*state) == nullptr) {
    if (state != nullptr)
      JS_ThrowTypeError(ctx, "STArray: invalid provider provenance");
    return nullptr;
  }
  return state;
}

[[nodiscard]] std::uint64_t readBigEndian(std::uint8_t const *bytes,
                                          std::uint32_t size) noexcept;

[[nodiscard]] bool sortedOrdinalContains(std::uint16_t const *values,
                                         std::uint16_t count,
                                         std::uint16_t wanted) noexcept {
  std::uint16_t first = 0;
  std::uint16_t last = count;
  while (first < last) {
    auto const middle = static_cast<std::uint16_t>(first + (last - first) / 2);
    if (values[middle] < wanted)
      first = static_cast<std::uint16_t>(middle + 1);
    else
      last = middle;
  }
  return first < count && values[first] == wanted;
}

[[nodiscard]] xdata::StaticObjectFormat const *findObjectFormat(
    xdata::StaticObjectFamily const &family, std::uint16_t typeCode) noexcept {
  std::uint16_t first = 0;
  std::uint16_t count = family.format_count;
  while (count != 0) {
    auto const step = static_cast<std::uint16_t>(count / 2);
    auto const index = static_cast<std::uint16_t>(first + step);
    auto const &format = static_data::OBJECT_FORMATS[family.format_begin + index];
    if (format.type_code < typeCode) {
      first = static_cast<std::uint16_t>(index + 1);
      count = static_cast<std::uint16_t>(count - step - 1);
    } else {
      count = step;
    }
  }
  if (first >= family.format_count)
    return nullptr;
  auto const &format = static_data::OBJECT_FORMATS[family.format_begin + first];
  return format.type_code == typeCode ? &format : nullptr;
}

[[nodiscard]] bool objectRuleHolds(CertifiedObjectValue const &owner,
                                   xdata::RecursiveIndexView index,
                                   std::uint32_t scope,
                                   xdata::StaticObjectRule const &rule) noexcept {
  auto const *field = index.find_object_field(scope, rule.field_code);
  if (field == nullptr || field->payload_begin > field->wire_end ||
      field->wire_end > owner.byteCount)
    return false;
  switch (rule.kind) {
  case xdata::StaticObjectRuleKind::native_amount:
    return field->wire_end - field->payload_begin == 8;
  case xdata::StaticObjectRuleKind::uint32_zero:
    return true;
  }
  return false;
}

[[nodiscard]] ObjectView classifyObjectScope(
    CertifiedObjectValue const &owner, std::uint32_t scopeId) noexcept {
  auto const index = ownerIndex(owner);
  auto const *scope = index.scope(scopeId);
  if (scope == nullptr || scope->kind() != xdata::ScopeKind::object)
    return ObjectView::st_object;

  xdata::StaticObjectFamily const *selectedFamily = nullptr;
  xdata::StaticObjectFormat const *selectedFormat = nullptr;
  for (auto const &family : static_data::OBJECT_FAMILIES) {
    auto const *discriminator =
        index.find_object_field(scopeId, family.discriminator_field_code);
    if (discriminator == nullptr)
      continue;
    if (selectedFamily != nullptr ||
        discriminator->payload_begin > discriminator->wire_end ||
        discriminator->wire_end > owner.byteCount ||
        discriminator->wire_end - discriminator->payload_begin != 2)
      return ObjectView::st_object;
    auto const typeCode = static_cast<std::uint16_t>(readBigEndian(
        owner.bytes + discriminator->payload_begin, 2));
    selectedFamily = &family;
    selectedFormat = findObjectFormat(family, typeCode);
    if (selectedFormat == nullptr)
      return ObjectView::st_object;
  }
  if (selectedFamily == nullptr || selectedFormat == nullptr)
    return ObjectView::st_object;

  auto const &protocol = xdata::xahau_static_protocol();
  for (std::uint16_t i = 0; i < selectedFormat->required_count; ++i) {
    auto const admission = static_data::OBJECT_REQUIRED_FIELDS[
        selectedFormat->required_begin + i];
    auto const *descriptor = protocol.field_by_ordinal(admission);
    if (descriptor == nullptr ||
        index.find_object_field(scopeId, descriptor->code) == nullptr)
      return ObjectView::st_object;
  }
  auto const *allowed = static_data::OBJECT_ALLOWED_FIELDS.data() +
                        selectedFormat->allowed_begin;
  for (std::uint32_t i = 0; i < scope->field_count(); ++i) {
    auto const *field = index.field(scope->first_field + i);
    auto const *descriptor = field == nullptr
                                 ? nullptr
                                 : protocol.field_by_code(field->field_code);
    if (descriptor == nullptr)
      return ObjectView::st_object;
    auto const admission = static_cast<std::uint16_t>(descriptor - protocol.fields);
    if (!sortedOrdinalContains(allowed, selectedFormat->allowed_count,
                               admission))
      return ObjectView::st_object;
  }
  for (std::uint16_t i = 0; i < selectedFormat->refinement_count; ++i) {
    auto const &rule = static_data::OBJECT_REFINEMENTS[
        selectedFormat->refinement_begin + i];
    if (!objectRuleHolds(owner, index, scopeId, rule))
      return ObjectView::st_object;
  }
  return static_cast<ObjectView>(selectedFormat->view);
}

[[nodiscard]] bool viewBelongsToFamily(
    ObjectView view, xdata::StaticObjectFamily const &family) noexcept {
  if (family.base_view == view)
    return true;
  for (std::uint16_t i = 0; i < family.format_count; ++i) {
    if (static_data::OBJECT_FORMATS[family.format_begin + i].view == view)
      return true;
  }
  return false;
}

[[nodiscard]] std::uint16_t objectDefaultCount(ObjectView view) noexcept {
  for (auto const &family : static_data::OBJECT_FAMILIES) {
    if (viewBelongsToFamily(view, family))
      return family.default_count;
  }
  return 0;
}

[[nodiscard]] JSValue newObjectWrapper(JSContext *ctx, JSValueConst owner,
                                       std::uint32_t scopeId) {
  auto *ownerState = ownerFrom(owner);
  auto const *scope =
      ownerState == nullptr ? nullptr : ownerIndex(*ownerState).scope(scopeId);
  if (scope == nullptr || scope->kind() != xdata::ScopeKind::object ||
      scopeId > kScopeMask || scope->field_count() > kScopeMask)
    return JS_ThrowInternalError(ctx, "invalid certified object scope");
  auto const view = classifyObjectScope(*ownerState, scopeId);
  auto const viewIndex = static_cast<std::size_t>(view);
  if (!objectNounsReady || viewIndex >= objectPrototypes.size() ||
      !JS_IsObject(objectPrototypes[viewIndex]))
    return JS_ThrowInternalError(ctx, "object prototype registrar is unavailable");
  auto const defaults = objectDefaultCount(view);
  if (defaults > kScopeMask - scope->field_count())
    return JS_ThrowInternalError(ctx, "object semantic cache exceeds limits");
  JSValue value =
      JS_NewObjectProtoClass(ctx, objectPrototypes[viewIndex], objectClassId);
  if (JS_IsException(value))
    return value;
  auto *state =
      static_cast<ObjectState *>(js_mallocz(ctx, sizeof(ObjectState)));
  if (state == nullptr) {
    JS_FreeValue(ctx, value);
    return oom(ctx);
  }
  state->owner = JS_DupValue(ctx, owner);
  state->scopeKey = makeScopeKey(scopeId, scope->field_count());
  state->view = view;
  state->cacheCount = static_cast<std::uint16_t>(scope->field_count() + defaults);
  JS_SetOpaque(value, state);
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
  test::gcProbeCreated(test::TrackedEntity::object, value);
  if (!test::gcProbePlantCycle(ctx, test::HiddenEdge::objectOwner, owner, value,
                               value) ||
      !test::gcProbePlantPendingCycle(ctx, test::HiddenEdge::objectCacheValue,
                                      value) ||
      !test::gcProbePlantPendingCycle(ctx, test::HiddenEdge::arrayCacheValue,
                                      value)) {
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

[[nodiscard]] JSValue newArrayWrapper(JSContext *ctx, JSValueConst owner,
                                      std::uint32_t scopeId) {
  auto *ownerState = ownerFrom(owner);
  auto const *scope =
      ownerState == nullptr ? nullptr : ownerIndex(*ownerState).scope(scopeId);
  if (scope == nullptr || scope->kind() != xdata::ScopeKind::array ||
      scopeId > kScopeMask || scope->field_count() > kScopeMask)
    return JS_ThrowInternalError(ctx, "invalid certified array scope");
  JSValue value = JS_NewObjectClass(ctx, arrayClassId);
  if (JS_IsException(value))
    return value;
  auto *state = static_cast<ArrayState *>(js_mallocz(ctx, sizeof(ArrayState)));
  if (state == nullptr) {
    JS_FreeValue(ctx, value);
    return oom(ctx);
  }
  state->owner = JS_DupValue(ctx, owner);
  state->scopeKey = makeScopeKey(scopeId, scope->field_count());
  JS_SetOpaque(value, state);
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
  test::gcProbeCreated(test::TrackedEntity::array, value);
  if (!test::gcProbePlantCycle(ctx, test::HiddenEdge::arrayOwner, owner, value,
                               value)) {
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

[[nodiscard]] std::uint64_t readBigEndian(std::uint8_t const *bytes,
                                          std::uint32_t size) noexcept {
  std::uint64_t value = 0;
  for (std::uint32_t i = 0; i < size; ++i)
    value = (value << 8) | bytes[i];
  return value;
}

[[nodiscard]] JSValue numberString(JSContext *ctx, Slice payload) {
  xdata::NormalizedNumber number;
  if (!xdata::NumberRules::normalize(payload, number))
    return JS_ThrowInternalError(ctx, "certified Number is invalid");
  if (number.mantissa == 0)
    return JS_NewStringLen(ctx, "0", 1);

  char digits[32];
  char *end = digits + sizeof(digits);
  char *cursor = end;
  bool const negative = number.mantissa < 0;
  auto const raw = static_cast<std::uint64_t>(number.mantissa);
  std::uint64_t magnitude = negative ? std::uint64_t{0} - raw : raw;
  do {
    *--cursor = static_cast<char>('0' + magnitude % 10);
    magnitude /= 10;
  } while (magnitude != 0);
  auto const digitCount = static_cast<std::uint32_t>(end - cursor);

  char output[64];
  std::uint32_t position = 0;
  if (negative)
    output[position++] = '-';
  auto append = [&](char const *data, std::uint32_t count) noexcept {
    std::memcpy(output + position, data, count);
    position += count;
  };

  if (number.exponent != 0 && (number.exponent < -25 || number.exponent > -5)) {
    append(cursor, digitCount);
    output[position++] = 'e';
    std::int32_t exponent = number.exponent;
    if (exponent < 0) {
      output[position++] = '-';
      exponent = -exponent;
    }
    char exponentDigits[12];
    char *exponentEnd = exponentDigits + sizeof(exponentDigits);
    char *exponentCursor = exponentEnd;
    do {
      *--exponentCursor = static_cast<char>('0' + exponent % 10);
      exponent /= 10;
    } while (exponent != 0);
    append(exponentCursor,
           static_cast<std::uint32_t>(exponentEnd - exponentCursor));
    return JS_NewStringLen(ctx, output, position);
  }

  if (number.exponent == 0) {
    append(cursor, digitCount);
    return JS_NewStringLen(ctx, output, position);
  }
  std::uint32_t croppedDigitCount = digitCount;
  while (croppedDigitCount != 0 && cursor[croppedDigitCount - 1] == '0')
    --croppedDigitCount;
  auto const decimalPosition =
      static_cast<std::int32_t>(digitCount) + number.exponent;
  if (decimalPosition <= 0) {
    output[position++] = '0';
    output[position++] = '.';
    for (std::int32_t i = 0; i < -decimalPosition; ++i)
      output[position++] = '0';
    append(cursor, croppedDigitCount);
  } else if (decimalPosition < static_cast<std::int32_t>(croppedDigitCount)) {
    append(cursor, static_cast<std::uint32_t>(decimalPosition));
    output[position++] = '.';
    append(cursor + decimalPosition,
           croppedDigitCount - static_cast<std::uint32_t>(decimalPosition));
  } else {
    append(cursor, croppedDigitCount);
    for (std::int32_t i = 0;
         i < decimalPosition - static_cast<std::int32_t>(croppedDigitCount);
         ++i)
      output[position++] = '0';
  }
  return JS_NewStringLen(ctx, output, position);
}

[[nodiscard]] JSValue hexString(JSContext *ctx, std::uint8_t const *bytes,
                                std::uint32_t size) {
  if (size > std::numeric_limits<std::uint32_t>::max() / 2)
    return JS_ThrowInternalError(ctx, "hex output size overflow");
  static constexpr char digits[] = "0123456789ABCDEF";
  auto const outputSize = static_cast<std::size_t>(size) * 2;
  char stack[64];
  char *output = outputSize <= sizeof(stack)
                     ? stack
                     : static_cast<char *>(js_malloc(ctx, outputSize));
  if (output == nullptr)
    return oom(ctx);
  for (std::uint32_t i = 0; i < size; ++i) {
    output[i * 2] = digits[bytes[i] >> 4];
    output[i * 2 + 1] = digits[bytes[i] & 15];
  }
  JSValue value = JS_NewStringLen(ctx, output, outputSize);
  if (output != stack)
    js_free(ctx, output);
  return value;
}

[[nodiscard]] JSValue uint64String(JSContext *ctx, std::uint64_t value) {
  char buffer[20];
  char *end = buffer + sizeof(buffer);
  char *cursor = end;
  do {
    *--cursor = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value != 0);
  return JS_NewStringLen(ctx, cursor, static_cast<std::size_t>(end - cursor));
}

[[nodiscard]] JSValue jsonObject(JSContext *ctx, CertifiedObjectValue &owner,
                                 xdata::RecursiveIndexView index,
                                 std::uint32_t scopeId);

[[nodiscard]] JSValue jsonArray(JSContext *ctx, CertifiedObjectValue &owner,
                                xdata::RecursiveIndexView index,
                                std::uint32_t scopeId);

[[nodiscard]] JSValue jsonField(JSContext *ctx, CertifiedObjectValue &owner,
                                xdata::RecursiveIndexView index,
                                xdata::FieldRecord const &field) {
  auto const *descriptor =
      xdata::xahau_static_protocol().field_by_code(field.field_code);
  if (descriptor == nullptr || field.payload_begin > field.wire_end ||
      field.wire_end > owner.byteCount)
    return JS_ThrowInternalError(ctx, "certified JSON field is invalid");
  auto const *payload = owner.bytes + field.payload_begin;
  auto const size = field.wire_end - field.payload_begin;
  using Kind = xdata::MaterializerKind;
  switch (descriptor->materializer) {
  case Kind::uint8:
  case Kind::uint16:
  case Kind::uint32:
  case Kind::ledger_entry_type:
  case Kind::transaction_type:
  case Kind::transaction_result:
    return JS_NewUint32(
        ctx, static_cast<std::uint32_t>(readBigEndian(payload, size)));
  case Kind::uint64:
    return uint64String(ctx, readBigEndian(payload, size));
  case Kind::hash128:
  case Kind::hash160:
  case Kind::hash192:
  case Kind::hash256:
  case Kind::blob:
    return hexString(ctx, payload, size);
  case Kind::number:
    return numberString(ctx, {payload, size});
  case Kind::st_object:
    return jsonObject(ctx, owner, index, field.child_scope);
  case Kind::st_array:
    return jsonArray(ctx, owner, index, field.child_scope);
  case Kind::account_id:
    return makeAccountIDCanonicalJSON(ctx, payload, size);
  case Kind::amount:
    return makeAmountCanonicalJSON(ctx, payload, size);
  case Kind::currency:
    return makeCurrencyCanonicalJSON(ctx, payload, size);
  case Kind::issue:
    return makeIssueCanonicalJSON(ctx, payload, size);
  case Kind::path_set:
    return makePathSetCanonicalJSON(ctx, payload, size);
  case Kind::vector256:
    return makeVector256CanonicalJSON(ctx, payload, size);
  case Kind::xchain_bridge:
    return makeXChainBridgeCanonicalJSON(ctx, payload, size);
  case Kind::invalid:
    break;
  }
  return JS_ThrowInternalError(ctx, "invalid certified JSON materializer");
}

[[nodiscard]] JSValue jsonObject(JSContext *ctx, CertifiedObjectValue &owner,
                                 xdata::RecursiveIndexView index,
                                 std::uint32_t scopeId) {
  auto const *scope = index.scope(scopeId);
  if (scope == nullptr || scope->kind() != xdata::ScopeKind::object)
    return JS_ThrowInternalError(ctx, "certified JSON object scope is invalid");
  qjs::OwnedValue result(ctx, JS_NewObject(ctx));
  if (result.isException())
    return result.release();
  for (std::uint32_t i = 0; i < scope->field_count(); ++i) {
    auto const *field = index.field(scope->first_field + i);
    auto const *descriptor =
        field == nullptr
            ? nullptr
            : xdata::xahau_static_protocol().field_by_code(field->field_code);
    if (descriptor == nullptr)
      return JS_ThrowInternalError(ctx, "certified JSON field is absent");
    auto const name =
        xdata::xahau_static_protocol().field_name(descriptor->name_ordinal);
    JSAtom const atom = JS_NewAtomLen(ctx, name.data, name.size);
    if (atom == JS_ATOM_NULL) {
      (void)atomFailure(ctx);
      return JS_EXCEPTION;
    }
    qjs::OwnedValue value(ctx, jsonField(ctx, owner, index, *field));
    if (value.isException() ||
        JS_DefinePropertyValue(ctx, result.get(), atom, value.release(),
                               JS_PROP_ENUMERABLE) < 0) {
      JS_FreeAtom(ctx, atom);
      return JS_EXCEPTION;
    }
    JS_FreeAtom(ctx, atom);
  }
  return result.release();
}

[[nodiscard]] JSValue jsonArray(JSContext *ctx, CertifiedObjectValue &owner,
                                xdata::RecursiveIndexView index,
                                std::uint32_t scopeId) {
  auto const *scope = index.scope(scopeId);
  if (scope == nullptr || scope->kind() != xdata::ScopeKind::array)
    return JS_ThrowInternalError(ctx, "certified JSON array scope is invalid");
  qjs::OwnedValue result(ctx, JS_NewArray(ctx));
  if (result.isException())
    return result.release();
  for (std::uint32_t i = 0; i < scope->field_count(); ++i) {
    auto const *field = index.array_element(scopeId, i);
    auto const *descriptor =
        field == nullptr
            ? nullptr
            : xdata::xahau_static_protocol().field_by_code(field->field_code);
    if (descriptor == nullptr)
      return JS_ThrowInternalError(ctx, "certified JSON array field is absent");
    qjs::OwnedValue element(ctx, JS_NewObject(ctx));
    if (element.isException())
      return element.release();
    auto const name =
        xdata::xahau_static_protocol().field_name(descriptor->name_ordinal);
    JSAtom const atom = JS_NewAtomLen(ctx, name.data, name.size);
    if (atom == JS_ATOM_NULL) {
      (void)atomFailure(ctx);
      return JS_EXCEPTION;
    }
    qjs::OwnedValue value(ctx,
                          jsonObject(ctx, owner, index, field->child_scope));
    if (value.isException() ||
        JS_DefinePropertyValue(ctx, element.get(), atom, value.release(),
                               JS_PROP_ENUMERABLE) < 0) {
      JS_FreeAtom(ctx, atom);
      return JS_EXCEPTION;
    }
    JS_FreeAtom(ctx, atom);
    if (JS_SetPropertyUint32(ctx, result.get(), i, element.release()) < 0)
      return JS_EXCEPTION;
  }
  return result.release();
}

[[nodiscard]] JSValue materializeField(JSContext *ctx, JSValueConst ownerValue,
                                       CertifiedObjectValue &owner,
                                       xdata::FieldRecord const &field) {
  auto const *descriptor =
      xdata::xahau_static_protocol().field_by_code(field.field_code);
  if (descriptor == nullptr || field.payload_begin > field.wire_end ||
      field.wire_end > owner.byteCount)
    return JS_ThrowInternalError(ctx, "certified field metadata is invalid");
  auto const *payload = owner.bytes + field.payload_begin;
  auto const size = field.wire_end - field.payload_begin;
  using Kind = xdata::MaterializerKind;
  switch (descriptor->materializer) {
  case Kind::uint8:
    return makeUIntValue(ctx, 8, readBigEndian(payload, size));
  case Kind::uint16:
    return makeUIntValue(ctx, 16, readBigEndian(payload, size));
  case Kind::uint32:
    return makeUIntValue(ctx, 32, readBigEndian(payload, size));
  case Kind::uint64:
    return makeUIntValue(ctx, 64, readBigEndian(payload, size));
  case Kind::ledger_entry_type:
  case Kind::transaction_type:
  case Kind::transaction_result:
    return JS_NewUint32(
        ctx, static_cast<std::uint32_t>(readBigEndian(payload, size)));
  case Kind::hash256:
    return makeHash256View(ctx, ownerValue, payload, size);
  case Kind::blob:
    return makeSTBlobView(ctx, ownerValue, payload, size);
  case Kind::account_id:
    return makeAccountIDView(ctx, ownerValue, payload, size);
  case Kind::number:
    return numberString(ctx, {payload, size});
  case Kind::hash128:
    return makeHash128View(ctx, ownerValue, payload, size);
  case Kind::hash160:
    return makeHash160View(ctx, ownerValue, payload, size);
  case Kind::hash192:
    return makeHash192View(ctx, ownerValue, payload, size);
  case Kind::amount:
    return makeAmountView(ctx, ownerValue, payload, size);
  case Kind::currency:
    return makeCurrencyView(ctx, ownerValue, payload, size);
  case Kind::issue:
    return makeIssueView(ctx, ownerValue, payload, size);
  case Kind::path_set:
    return makePathSetBytes(ctx, ownerValue, payload, size);
  case Kind::vector256:
    return makeVector256View(ctx, ownerValue, payload, size);
  case Kind::xchain_bridge:
    return makeXChainBridgeView(ctx, ownerValue, payload, size);
  case Kind::st_object:
    return newObjectWrapper(ctx, ownerValue, field.child_scope);
  case Kind::st_array:
    return newArrayWrapper(ctx, ownerValue, field.child_scope);
  case Kind::invalid:
    break;
  }
  return JS_ThrowInternalError(ctx, "invalid certified materializer");
}

struct LocatedField {
  xdata::FieldRecord const *field = nullptr;
  std::uint32_t slot = 0;
};

struct LocatedDefault {
  xdata::StaticObjectRule const *rule = nullptr;
  std::uint32_t slot = 0;
};

[[nodiscard]] LocatedField locateObjectField(ObjectState const &state,
                                             std::uint32_t code) noexcept {
  auto *owner = ownerFrom(state);
  if (owner == nullptr)
    return {};
  auto const index = ownerIndex(*owner);
  auto const *scope = index.scope(scopeId(state));
  auto const *field = index.find_object_field(scopeId(state), code);
  if (scope == nullptr || field == nullptr)
    return {};
  auto const *first = index.field(scope->first_field);
  if (first == nullptr || field < first ||
      static_cast<std::uint32_t>(field - first) >= scope->field_count())
    return {};
  return {field, static_cast<std::uint32_t>(field - first)};
}

[[nodiscard]] LocatedDefault locateObjectDefault(ObjectState const &state,
                                                 std::uint32_t code) noexcept {
  auto const view = static_cast<ObjectView>(state.view);
  for (auto const &family : static_data::OBJECT_FAMILIES) {
    if (!viewBelongsToFamily(view, family))
      continue;
    for (std::uint16_t i = 0; i < family.default_count; ++i) {
      auto const &rule = static_data::OBJECT_DEFAULTS[family.default_begin + i];
      if (rule.field_code == code)
        return {&rule, directCount(state) + i};
    }
    break;
  }
  return {};
}

[[nodiscard]] bool ensureObjectCache(JSContext *ctx, ObjectState &state) {
  if (state.cache != nullptr)
    return true;
  auto const bytes = static_cast<std::size_t>(state.cacheCount) * sizeof(JSValue);
  auto *cache = static_cast<JSValue *>(js_malloc(ctx, bytes));
  if (cache == nullptr)
    return false;
  for (std::uint32_t i = 0; i < state.cacheCount; ++i)
    cache[i] = JS_UNDEFINED;
  state.cache = cache;
  return true;
}

[[nodiscard]] JSValue objectValue(JSContext *ctx, ObjectState &state,
                                  LocatedField located) {
  if (located.field == nullptr)
    return JS_UNDEFINED;
  auto *owner = ownerFrom(state);
  auto const index =
      owner == nullptr ? xdata::RecursiveIndexView{} : ownerIndex(*owner);
  auto const *scope = index.scope(scopeId(state));
  if (owner == nullptr || scope == nullptr ||
      located.slot >= scope->field_count())
    return JS_ThrowTypeError(ctx, "STObject: invalid provider provenance");
  if (state.cache != nullptr && !JS_IsUndefined(state.cache[located.slot]))
    return JS_DupValue(ctx, state.cache[located.slot]);

  qjs::OwnedValue local(
      ctx, materializeField(ctx, state.owner, *owner, *located.field));
  if (local.isException())
    return local.release();
  if (!ensureObjectCache(ctx, state))
    return oom(ctx);
  state.cache[located.slot] = local.release();
  return JS_DupValue(ctx, state.cache[located.slot]);
}

[[nodiscard]] JSValue objectDefaultValue(JSContext *ctx, ObjectState &state,
                                         LocatedDefault located) {
  if (located.rule == nullptr)
    return JS_UNDEFINED;
  if (located.slot >= state.cacheCount)
    return JS_ThrowInternalError(ctx, "object default cache is invalid");
  if (state.cache != nullptr && !JS_IsUndefined(state.cache[located.slot]))
    return JS_DupValue(ctx, state.cache[located.slot]);

  qjs::OwnedValue value(ctx);
  switch (located.rule->kind) {
  case xdata::StaticObjectRuleKind::uint32_zero:
    value = qjs::OwnedValue(ctx, makeUIntValue(ctx, 32, 0));
    break;
  case xdata::StaticObjectRuleKind::native_amount:
    return JS_ThrowInternalError(ctx, "refinement cannot supply a default");
  }
  if (value.isException())
    return value.release();
  if (!ensureObjectCache(ctx, state))
    return oom(ctx);
  state.cache[located.slot] = value.release();
  return JS_DupValue(ctx, state.cache[located.slot]);
}

[[nodiscard]] bool resolveFieldArgument(JSContext *ctx, int argc,
                                        JSValueConst *argv, std::uint32_t &code,
                                        bool allowNumeric) {
  if (argc < 1)
    return false;
  if (serializedFieldCode(argv[0], code))
    return true;
  if (allowNumeric && JS_IsNumber(argv[0])) {
    std::int64_t value = 0;
    if (JS_ToInt64(ctx, &value, argv[0]) < 0)
      return false;
    if (value <= 0 || static_cast<std::uint64_t>(value) >
                          std::numeric_limits<std::uint32_t>::max())
      return false;
    code = static_cast<std::uint32_t>(value);
    return true;
  }
  if (!JS_IsString(argv[0]))
    return false;
  JSAtom const atom = JS_ValueToAtom(ctx, argv[0]);
  if (atom == JS_ATOM_NULL)
    return atomFailure(ctx);
  auto const *name = fieldNameByAtom(atom);
  JS_FreeAtom(ctx, atom);
  if (name == nullptr || (name->flags & xdata::field_name_serialized) == 0)
    return false;
  code = name->code;
  return true;
}

[[nodiscard]] JSValue objectHas(JSContext *ctx, JSValueConst thisValue,
                                int argc, JSValueConst *argv) {
  auto *state = objectState(ctx, thisValue);
  if (state == nullptr)
    return JS_EXCEPTION;
  std::uint32_t code = 0;
  if (!resolveFieldArgument(ctx, argc, argv, code, false))
    return JS_HasException(ctx) ? JS_EXCEPTION : JS_FALSE;
  return JS_NewBool(ctx, locateObjectField(*state, code).field != nullptr ||
                             locateObjectDefault(*state, code).rule != nullptr);
}

[[nodiscard]] JSValue objectGet(JSContext *ctx, JSValueConst thisValue,
                                int argc, JSValueConst *argv) {
  auto *state = objectState(ctx, thisValue);
  if (state == nullptr)
    return JS_EXCEPTION;
  std::uint32_t code = 0;
  if (!resolveFieldArgument(ctx, argc, argv, code, false))
    return JS_HasException(ctx) ? JS_EXCEPTION : JS_UNDEFINED;
  auto const field = locateObjectField(*state, code);
  return field.field != nullptr
             ? objectValue(ctx, *state, field)
             : objectDefaultValue(ctx, *state,
                                  locateObjectDefault(*state, code));
}

[[nodiscard]] JSValue objectFieldBytes(JSContext *ctx, JSValueConst thisValue,
                                       int argc, JSValueConst *argv) {
  auto *state = objectState(ctx, thisValue);
  if (state == nullptr)
    return JS_EXCEPTION;
  std::uint32_t code = 0;
  if (!resolveFieldArgument(ctx, argc, argv, code, true))
    return JS_HasException(ctx) ? JS_EXCEPTION : JS_UNDEFINED;
  auto const located = locateObjectField(*state, code);
  if (located.field == nullptr)
    return JS_UNDEFINED;
  auto *owner = ownerFrom(*state);
  auto const index = ownerIndex(*owner);
  auto const measured = xdata::canonical_field_value_size(
      ownerBytes(*owner), index, *located.field);
  if (!measured.ok())
    return JS_ThrowInternalError(ctx, "canonical field measure failed");
  std::uint8_t *output = nullptr;
  qjs::OwnedValue result(ctx,
                         makeSTBlobUninitialized(ctx, measured.size, &output));
  if (result.isException())
    return result.release();
  auto const written = xdata::canonical_field_value_write(
      ownerBytes(*owner), index, *located.field, output, measured.size);
  if (!written.ok())
    return JS_ThrowInternalError(ctx, "canonical field write failed");
  return result.release();
}

[[nodiscard]] JSValue objectToBytes(JSContext *ctx, JSValueConst thisValue, int,
                                    JSValueConst *) {
  auto *state = objectState(ctx, thisValue);
  if (state == nullptr)
    return JS_EXCEPTION;
  auto *owner = ownerFrom(*state);
  auto const index = ownerIndex(*owner);
  auto const measured = xdata::canonical_object_size(
      ownerBytes(*owner), index, scopeId(*state), scopeId(*state) == 0);
  if (!measured.ok())
    return JS_ThrowInternalError(ctx, "canonical object measure failed");
  std::uint8_t *output = nullptr;
  qjs::OwnedValue result(
      ctx, qjs::uint8ArrayUninitialized(ctx, measured.size, &output));
  if (result.isException())
    return result.release();
  auto const written = xdata::canonical_object_write(
      ownerBytes(*owner), index, scopeId(*state), scopeId(*state) == 0, output,
      measured.size);
  if (!written.ok())
    return JS_ThrowInternalError(ctx, "canonical object write failed");
  return result.release();
}

[[nodiscard]] JSValue objectToJSON(JSContext *ctx, JSValueConst thisValue, int,
                                   JSValueConst *) {
  auto *state = objectState(ctx, thisValue);
  if (state == nullptr)
    return JS_EXCEPTION;
  auto *owner = ownerFrom(*state);
  return jsonObject(ctx, *owner, ownerIndex(*owner), scopeId(*state));
}

[[nodiscard]] JSValue arrayValue(JSContext *ctx, ArrayState &state,
                                 std::uint32_t indexValue) {
  auto *owner = ownerFrom(state);
  auto const index =
      owner == nullptr ? xdata::RecursiveIndexView{} : ownerIndex(*owner);
  auto const *scope = index.scope(scopeId(state));
  auto const *field = index.array_element(scopeId(state), indexValue);
  if (owner == nullptr || scope == nullptr || field == nullptr)
    return JS_UNDEFINED;

  auto const rootIndex = (indexValue >> 10) & 31;
  auto const branchIndex = (indexValue >> 5) & 31;
  auto const slotIndex = indexValue & 31;
  if (state.cache != nullptr) {
    auto *branch = state.cache->branches[rootIndex];
    auto *leaf = branch == nullptr ? nullptr : branch->pages[branchIndex];
    if (leaf != nullptr && !JS_IsUndefined(leaf->values[slotIndex])) {
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
      if (test::gcProbeAllocateOnRadixHit()) {
        void *poison = js_malloc(ctx, 1);
        if (poison == nullptr)
          return oom(ctx);
        test::gcProbeRadixAllocated(test::RadixAllocationKind::hitPoison, 1);
        js_free(ctx, poison);
      }
#endif
      return JS_DupValue(ctx, leaf->values[slotIndex]);
    }
  }

  qjs::OwnedValue local(ctx,
                        newObjectWrapper(ctx, state.owner, field->child_scope));
  if (local.isException())
    return local.release();

  ArrayCacheRoot *newRoot = nullptr;
  ArrayCacheBranch *newBranch = nullptr;
  ArrayCacheLeaf *newLeaf = nullptr;
  auto *root = state.cache;
  auto *branch = root == nullptr ? nullptr : root->branches[rootIndex];
  auto *leaf = branch == nullptr ? nullptr : branch->pages[branchIndex];
  if (root == nullptr) {
    newRoot =
        static_cast<ArrayCacheRoot *>(js_mallocz(ctx, sizeof(ArrayCacheRoot)));
    if (newRoot == nullptr)
      return oom(ctx);
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
    test::gcProbeRadixAllocated(test::RadixAllocationKind::root,
                                sizeof(ArrayCacheRoot));
#endif
    root = newRoot;
  }
  if (branch == nullptr) {
    newBranch = static_cast<ArrayCacheBranch *>(
        js_mallocz(ctx, sizeof(ArrayCacheBranch)));
    if (newBranch == nullptr) {
      js_free(ctx, newRoot);
      return oom(ctx);
    }
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
    test::gcProbeRadixAllocated(test::RadixAllocationKind::branch,
                                sizeof(ArrayCacheBranch));
#endif
    branch = newBranch;
  }
  if (leaf == nullptr) {
    newLeaf =
        static_cast<ArrayCacheLeaf *>(js_malloc(ctx, sizeof(ArrayCacheLeaf)));
    if (newLeaf == nullptr) {
      js_free(ctx, newBranch);
      js_free(ctx, newRoot);
      return oom(ctx);
    }
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
    test::gcProbeRadixAllocated(test::RadixAllocationKind::leaf,
                                sizeof(ArrayCacheLeaf));
#endif
    for (JSValue &value : newLeaf->values)
      value = JS_UNDEFINED;
    leaf = newLeaf;
  }

  if (newRoot != nullptr)
    state.cache = newRoot;
  if (newBranch != nullptr) {
    root->branches[rootIndex] = newBranch;
    ++root->branchCount;
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
    if (test::gcProbeCorruptRadixCounts())
      ++root->branchCount;
#endif
  }
  if (newLeaf != nullptr) {
    branch->pages[branchIndex] = newLeaf;
    ++root->pageCount;
  }
  leaf->values[slotIndex] = local.release();
  ++root->valueCount;
  return JS_DupValue(ctx, leaf->values[slotIndex]);
}

[[nodiscard]] JSValue arrayLength(JSContext *ctx, JSValueConst thisValue) {
  auto *state = arrayState(ctx, thisValue);
  if (state == nullptr)
    return JS_EXCEPTION;
  auto *owner = ownerFrom(*state);
  auto const *scope = ownerIndex(*owner).scope(scopeId(*state));
  return scope == nullptr
             ? JS_ThrowTypeError(ctx, "STArray: invalid provider provenance")
             : JS_NewUint32(ctx, scope->field_count());
}

[[nodiscard]] bool readAtIndex(JSContext *ctx, int argc, JSValueConst *argv,
                               std::uint32_t length, std::uint32_t &index) {
  if (argc < 1)
    return false;
  double number = 0;
  if (JS_ToFloat64(ctx, &number, argv[0]) < 0)
    return false;
  if (std::isnan(number) || number == 0)
    number = 0;
  else if (std::isfinite(number))
    number = std::trunc(number);
  if (number < 0)
    number += length;
  if (!std::isfinite(number) || number < 0 || number >= length)
    return false;
  index = static_cast<std::uint32_t>(number);
  return true;
}

[[nodiscard]] JSValue arrayAt(JSContext *ctx, JSValueConst thisValue, int argc,
                              JSValueConst *argv) {
  auto *state = arrayState(ctx, thisValue);
  if (state == nullptr)
    return JS_EXCEPTION;
  auto *owner = ownerFrom(*state);
  auto const *scope = ownerIndex(*owner).scope(scopeId(*state));
  if (scope == nullptr)
    return JS_ThrowTypeError(ctx, "STArray: invalid provider provenance");
  std::uint32_t index = 0;
  if (!readAtIndex(ctx, argc, argv, scope->field_count(), index))
    return JS_HasException(ctx) ? JS_EXCEPTION : JS_UNDEFINED;
  return arrayValue(ctx, *state, index);
}

[[nodiscard]] JSValue arrayToJSON(JSContext *ctx, JSValueConst thisValue, int,
                                  JSValueConst *) {
  auto *state = arrayState(ctx, thisValue);
  if (state == nullptr)
    return JS_EXCEPTION;
  auto *owner = ownerFrom(*state);
  return jsonArray(ctx, *owner, ownerIndex(*owner), scopeId(*state));
}

[[nodiscard]] JSValue iteratorSelf(JSContext *ctx, JSValueConst thisValue, int,
                                   JSValueConst *) {
  auto *state = static_cast<IteratorState *>(
      JS_GetOpaque2(ctx, thisValue, iteratorClassId));
  return state == nullptr ? JS_EXCEPTION : JS_DupValue(ctx, thisValue);
}

[[nodiscard]] JSValue newIteratorResult(JSContext *ctx, JSValue value,
                                        bool done) {
  if (JS_IsException(value))
    return value;
  qjs::OwnedValue ownedValue(ctx, value);
  qjs::OwnedValue result(ctx, JS_NewObject(ctx));
  if (result.isException())
    return result.release();
  if (JS_DefinePropertyValueStr(ctx, result.get(), "value",
                                ownedValue.release(), JS_PROP_ENUMERABLE) < 0 ||
      JS_DefinePropertyValueStr(ctx, result.get(), "done",
                                JS_NewBool(ctx, done), JS_PROP_ENUMERABLE) < 0)
    return JS_EXCEPTION;
  return result.release();
}

[[nodiscard]] JSValue iteratorNext(JSContext *ctx, JSValueConst thisValue, int,
                                   JSValueConst *) {
  auto *iterator = static_cast<IteratorState *>(
      JS_GetOpaque2(ctx, thisValue, iteratorClassId));
  if (iterator == nullptr)
    return JS_EXCEPTION;
  auto *array = arrayState(ctx, iterator->array);
  if (array == nullptr)
    return JS_EXCEPTION;
  auto *owner = ownerFrom(*array);
  auto const *scope = ownerIndex(*owner).scope(scopeId(*array));
  if (scope == nullptr)
    return JS_ThrowTypeError(ctx, "STArray: invalid provider provenance");
  if (iterator->cursor >= scope->field_count())
    return newIteratorResult(ctx, JS_UNDEFINED, true);
  qjs::OwnedValue value(ctx, arrayValue(ctx, *array, iterator->cursor));
  if (value.isException())
    return value.release();
  JSValue result = newIteratorResult(ctx, value.release(), false);
  if (!JS_IsException(result))
    ++iterator->cursor;
  return result;
}

[[nodiscard]] JSValue arrayIterator(JSContext *ctx, JSValueConst thisValue, int,
                                    JSValueConst *) {
  if (arrayState(ctx, thisValue) == nullptr)
    return JS_EXCEPTION;
  JSValue value = JS_NewObjectClass(ctx, iteratorClassId);
  if (JS_IsException(value))
    return value;
  auto *state =
      static_cast<IteratorState *>(js_mallocz(ctx, sizeof(IteratorState)));
  if (state == nullptr) {
    JS_FreeValue(ctx, value);
    return oom(ctx);
  }
  state->array = JS_DupValue(ctx, thisValue);
  JS_SetOpaque(value, state);
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
  test::gcProbeCreated(test::TrackedEntity::iterator, value);
  auto *array = arrayState(ctx, thisValue);
  if (array == nullptr ||
      !test::gcProbePlantCycle(ctx, test::HiddenEdge::iteratorArray,
                               array->owner, value, value)) {
    JS_FreeValue(ctx, value);
    return JS_EXCEPTION;
  }
#endif
  return value;
}

[[nodiscard]] bool arrayPropertyIndex(JSAtom prop, std::uint32_t length,
                                      std::uint32_t &index) noexcept {
  if (!JS_AtomIsTaggedInt(prop))
    return false;
  index = JS_AtomToTaggedInt(prop);
  return index < length;
}

[[nodiscard]] int objectOwnProperty(JSContext *ctx,
                                    JSPropertyDescriptor *descriptor,
                                    JSValueConst value, JSAtom prop) {
  if (descriptor != nullptr) {
    descriptor->flags = 0;
    descriptor->value = JS_UNDEFINED;
    descriptor->getter = JS_UNDEFINED;
    descriptor->setter = JS_UNDEFINED;
  }
  auto *state = objectState(ctx, value);
  if (state == nullptr)
    return -1;
  auto const *name = fieldNameByAtom(prop);
  if (name == nullptr || (name->flags & xdata::field_name_serialized) == 0)
    return 0;
  auto const located = locateObjectField(*state, name->code);
  auto const defaulted = located.field == nullptr
                             ? locateObjectDefault(*state, name->code)
                             : LocatedDefault{};
  if (located.field == nullptr && defaulted.rule == nullptr)
    return 0;
  if (descriptor != nullptr) {
    JSValue materialized = located.field != nullptr
                               ? objectValue(ctx, *state, located)
                               : objectDefaultValue(ctx, *state, defaulted);
    if (JS_IsException(materialized))
      return -1;
    descriptor->value = materialized;
    descriptor->flags = JS_PROP_ENUMERABLE;
  }
  return 1;
}

[[nodiscard]] int arrayOwnProperty(JSContext *ctx,
                                   JSPropertyDescriptor *descriptor,
                                   JSValueConst value, JSAtom prop) {
  if (descriptor != nullptr) {
    descriptor->flags = 0;
    descriptor->value = JS_UNDEFINED;
    descriptor->getter = JS_UNDEFINED;
    descriptor->setter = JS_UNDEFINED;
  }
  auto *state = arrayState(ctx, value);
  if (state == nullptr)
    return -1;
  auto *owner = ownerFrom(*state);
  auto const *scope = ownerIndex(*owner).scope(scopeId(*state));
  if (scope == nullptr)
    return -1;
  bool isLength = false;
  if (!isLengthAtom(ctx, prop, isLength))
    return -1;
  // @binding provider:STArray.length
  if (isLength) {
    if (descriptor != nullptr)
      descriptor->value = JS_NewUint32(ctx, scope->field_count());
    return 1;
  }
  std::uint32_t index = 0;
  if (!arrayPropertyIndex(prop, scope->field_count(), index))
    return 0;
  if (descriptor != nullptr) {
    JSValue materialized = arrayValue(ctx, *state, index);
    if (JS_IsException(materialized))
      return -1;
    descriptor->value = materialized;
    descriptor->flags = JS_PROP_ENUMERABLE;
  }
  return 1;
}

[[nodiscard]] int objectOwnNames(JSContext *ctx, JSPropertyEnum **table,
                                 std::uint32_t *length, JSValueConst value) {
  *table = nullptr;
  *length = 0;
  auto *state = objectState(ctx, value);
  if (state == nullptr)
    return -1;
  auto *owner = ownerFrom(*state);
  auto const index = ownerIndex(*owner);
  auto const *scope = index.scope(scopeId(*state));
  if (scope == nullptr)
    return -1;
  std::uint32_t defaultCount = 0;
  auto const view = static_cast<ObjectView>(state->view);
  xdata::StaticObjectFamily const *familyDefaults = nullptr;
  for (auto const &family : static_data::OBJECT_FAMILIES) {
    if (viewBelongsToFamily(view, family)) {
      familyDefaults = &family;
      for (std::uint16_t i = 0; i < family.default_count; ++i) {
        auto const &rule = static_data::OBJECT_DEFAULTS[family.default_begin + i];
        if (index.find_object_field(scopeId(*state), rule.field_code) == nullptr)
          ++defaultCount;
      }
      break;
    }
  }
  auto const count = scope->field_count() + defaultCount;
  if (count == 0)
    return 0;
  auto *names = static_cast<JSPropertyEnum *>(
      js_malloc(ctx, static_cast<std::size_t>(count) * sizeof(JSPropertyEnum)));
  if (names == nullptr)
    return -1;
  std::uint32_t written = 0;
  for (std::uint32_t i = 0; i < scope->field_count(); ++i) {
    auto const *field = index.field(scope->first_field + i);
    auto const *descriptor =
        field == nullptr
            ? nullptr
            : xdata::xahau_static_protocol().field_by_code(field->field_code);
    if (descriptor == nullptr) {
      for (std::uint32_t j = 0; j < written; ++j)
        JS_FreeAtom(ctx, names[j].atom);
      js_free(ctx, names);
      return -1;
    }
    auto const nameOrdinal = descriptor->name_ordinal;
    auto const name = xdata::xahau_static_protocol().field_name(nameOrdinal);
    JSAtom const atom = JS_NewAtomLen(ctx, name.data, name.size);
    if (atom == JS_ATOM_NULL) {
      for (std::uint32_t j = 0; j < written; ++j)
        JS_FreeAtom(ctx, names[j].atom);
      js_free(ctx, names);
      (void)atomFailure(ctx);
      return -1;
    }
    names[written++] = {true, atom};
  }
  if (familyDefaults != nullptr) {
    for (std::uint16_t i = 0; i < familyDefaults->default_count; ++i) {
      auto const &rule = static_data::OBJECT_DEFAULTS[
          familyDefaults->default_begin + i];
      if (index.find_object_field(scopeId(*state), rule.field_code) != nullptr)
        continue;
      auto const *descriptor =
          xdata::xahau_static_protocol().field_by_code(rule.field_code);
      if (descriptor == nullptr) {
        for (std::uint32_t j = 0; j < written; ++j)
          JS_FreeAtom(ctx, names[j].atom);
        js_free(ctx, names);
        return -1;
      }
      auto const name =
          xdata::xahau_static_protocol().field_name(descriptor->name_ordinal);
      JSAtom const atom = JS_NewAtomLen(ctx, name.data, name.size);
      if (atom == JS_ATOM_NULL) {
        for (std::uint32_t j = 0; j < written; ++j)
          JS_FreeAtom(ctx, names[j].atom);
        js_free(ctx, names);
        (void)atomFailure(ctx);
        return -1;
      }
      names[written++] = {true, atom};
    }
  }
  if (written != count) {
    for (std::uint32_t j = 0; j < written; ++j)
      JS_FreeAtom(ctx, names[j].atom);
    js_free(ctx, names);
    return -1;
  }
  *table = names;
  *length = count;
  return 0;
}

[[nodiscard]] int arrayOwnNames(JSContext *ctx, JSPropertyEnum **table,
                                std::uint32_t *length, JSValueConst value) {
  *table = nullptr;
  *length = 0;
  auto *state = arrayState(ctx, value);
  if (state == nullptr)
    return -1;
  auto *owner = ownerFrom(*state);
  auto const *scope = ownerIndex(*owner).scope(scopeId(*state));
  if (scope == nullptr)
    return -1;
  auto const count = scope->field_count() + 1;
  auto *names = static_cast<JSPropertyEnum *>(
      js_malloc(ctx, static_cast<std::size_t>(count) * sizeof(JSPropertyEnum)));
  if (names == nullptr)
    return -1;
  for (std::uint32_t i = 0; i < scope->field_count(); ++i)
    names[i] = {true, JS_NewAtomUInt32(ctx, i)};
  JSAtom const lengthKey = newLengthAtom(ctx);
  if (lengthKey == JS_ATOM_NULL) {
    for (std::uint32_t i = 0; i + 1 < count; ++i)
      JS_FreeAtom(ctx, names[i].atom);
    js_free(ctx, names);
    return -1;
  }
  names[count - 1] = {false, lengthKey};
  *table = names;
  *length = count;
  return 0;
}

[[nodiscard]] int objectDelete(JSContext *ctx, JSValueConst value,
                               JSAtom prop) {
  auto *state = objectState(ctx, value);
  if (state == nullptr)
    return -1;
  auto const *name = fieldNameByAtom(prop);
  return name == nullptr || (name->flags & xdata::field_name_serialized) == 0 ||
         (locateObjectField(*state, name->code).field == nullptr &&
          locateObjectDefault(*state, name->code).rule == nullptr);
}

[[nodiscard]] int arrayDelete(JSContext *ctx, JSValueConst value, JSAtom prop) {
  auto *state = arrayState(ctx, value);
  if (state == nullptr)
    return -1;
  auto *owner = ownerFrom(*state);
  auto const *scope = ownerIndex(*owner).scope(scopeId(*state));
  if (scope == nullptr)
    return -1;
  bool isLength = false;
  if (!isLengthAtom(ctx, prop, isLength))
    return -1;
  if (isLength)
    return 0;
  std::uint32_t index = 0;
  return !arrayPropertyIndex(prop, scope->field_count(), index);
}

[[nodiscard]] JSValue objectWithField(JSContext *ctx, JSValueConst thisValue,
                                      int argc, JSValueConst *argv);

[[nodiscard]] JSValue objectWithoutField(JSContext *ctx, JSValueConst thisValue,
                                         int argc, JSValueConst *argv);

JSClassExoticMethods objectExotic = {
    .get_own_property = objectOwnProperty,
    .get_own_property_names = objectOwnNames,
    .delete_property = objectDelete,
};

JSClassExoticMethods arrayExotic = {
    .get_own_property = arrayOwnProperty,
    .get_own_property_names = arrayOwnNames,
    .delete_property = arrayDelete,
};

JSClassDef ownerClass = {
    .class_name = "CertifiedObjectValue",
    .finalizer = ownerFinalizer,
};

JSClassDef objectClass = {
    .class_name = "STObject",
    .finalizer = objectFinalizer,
    .gc_mark = objectMark,
    .exotic = &objectExotic,
};

JSClassDef arrayClass = {
    .class_name = "STArray",
    .finalizer = arrayFinalizer,
    .gc_mark = arrayMark,
    .exotic = &arrayExotic,
};

JSClassDef iteratorClass = {
    .class_name = "STArray Iterator",
    .finalizer = iteratorFinalizer,
    .gc_mark = iteratorMark,
};

JSCFunctionListEntry const objectPrototype[] = {
    // @binding provider:STObject.has
    JS_CFUNC_DEF("has", 1, objectHas),
    // @binding provider:STObject.get
    JS_CFUNC_DEF("get", 1, objectGet),
    // @binding provider:STObject.fieldBytes
    JS_CFUNC_DEF("fieldBytes", 1, objectFieldBytes),
    // @binding provider:STObject.withField
    JS_CFUNC_DEF("withField", 2, objectWithField),
    // @binding provider:STObject.withoutField
    JS_CFUNC_DEF("withoutField", 1, objectWithoutField),
    // @binding provider:STObject.toBytes
    JS_CFUNC_DEF("toBytes", 0, objectToBytes),
    // @binding provider:STObject.toJSON
    JS_CFUNC_DEF("toJSON", 0, objectToJSON),
};

JSCFunctionListEntry const arrayPrototype[] = {
    // @binding provider:STArray.at
    JS_CFUNC_DEF("at", 1, arrayAt),
    // @binding provider:STArray.toJSON
    JS_CFUNC_DEF("toJSON", 0, arrayToJSON),
    // @binding provider:STArray.[Symbol.iterator]
    JS_CFUNC_DEF("[Symbol.iterator]", 0, arrayIterator),
};

JSCFunctionListEntry const iteratorPrototype[] = {
    JS_CFUNC_DEF("next", 0, iteratorNext),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, iteratorSelf),
};

[[nodiscard]] JSValue unavailableObjectConstructor(
    JSContext *ctx, JSValueConst, int, JSValueConst *) {
  return JS_ThrowTypeError(
      ctx, "serialized object constructors are provider-only");
}

struct StagedObjectNouns {
  JSContext *ctx;
  std::array<JSValue, kObjectViewCount> prototypes;
  std::array<JSValue, kObjectViewCount> constructors;

  explicit StagedObjectNouns(JSContext *context) : ctx(context) {
    prototypes.fill(JS_UNDEFINED);
    constructors.fill(JS_UNDEFINED);
  }

  ~StagedObjectNouns() {
    for (JSValue value : prototypes)
      JS_FreeValue(ctx, value);
    for (JSValue value : constructors)
      JS_FreeValue(ctx, value);
  }

  StagedObjectNouns(StagedObjectNouns const &) = delete;
  StagedObjectNouns &operator=(StagedObjectNouns const &) = delete;
};

[[nodiscard]] bool stageObjectNouns(JSContext *ctx,
                                    StagedObjectNouns &staged) {
  static_assert(static_data::OBJECT_VIEWS.size() == kObjectViewCount);
  for (std::size_t i = 0; i < kObjectViewCount; ++i) {
    auto const &view = static_data::OBJECT_VIEWS[i];
    if (i == static_cast<std::size_t>(ObjectView::st_object)) {
      if (view.parent_view != xdata::ProtocolView::no_ordinal)
        return false;
      staged.prototypes[i] = qjs::makePrototype(ctx, objectPrototype, false);
    } else {
      if (view.parent_view >= i ||
          !JS_IsObject(staged.prototypes[view.parent_view]))
        return false;
      staged.prototypes[i] =
          JS_NewObjectProto(ctx, staged.prototypes[view.parent_view]);
    }
    if (JS_IsException(staged.prototypes[i]))
      return false;

    auto const *name = static_data::OBJECT_VIEW_NAME_BYTES + view.name_offset;
    staged.constructors[i] = JS_NewCFunction2(
        ctx, unavailableObjectConstructor, name, 0, JS_CFUNC_constructor, 0);
    if (JS_IsException(staged.constructors[i]) ||
        (view.parent_view != xdata::ProtocolView::no_ordinal &&
         JS_SetPrototype(ctx, staged.constructors[i],
                         staged.constructors[view.parent_view]) < 0) ||
        JS_SetConstructor(ctx, staged.constructors[i], staged.prototypes[i]) < 0)
      return false;
  }
  for (std::size_t i = 0; i < kObjectViewCount; ++i) {
    if (!qjs::freezeObject(ctx, staged.prototypes[i]) ||
        !qjs::freezeObject(ctx, staged.constructors[i]))
      return false;
  }
  return true;
}

[[nodiscard]] JSValue newOwner(JSContext *ctx, std::uint8_t *bytes,
                               std::uint32_t byteCount, void *index) {
  JSValue value = JS_NewObjectClass(ctx, ownerClassId);
  if (JS_IsException(value))
    return value;
  auto *owner = static_cast<CertifiedObjectValue *>(
      js_malloc(ctx, sizeof(CertifiedObjectValue)));
  if (owner == nullptr) {
    JS_FreeValue(ctx, value);
    return oom(ctx);
  }
  *owner = {bytes, index, byteCount, kProtocolTag};
  JS_SetOpaque(value, owner);
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
  test::gcProbeCreated(test::TrackedEntity::owner, value);
#endif
  return value;
}

enum class ParseVariant : std::uint8_t {
  malformed,
  unknownField,
  invalidField,
  duplicateField,
  wrongTerminator,
  trailingBytes,
  resourceLimit,
};

struct ParseDiagnostic {
  ParseVariant variant = ParseVariant::malformed;
  char const *issue = "malformed";
  char const *expected = nullptr;
  char const *limit = nullptr;
  xdata::ScanStatus status{};
  std::uint32_t maximum = 0;
};

[[nodiscard]] ParseDiagnostic
parseDiagnostic(xdata::ScanStatus status) noexcept {
  using Message = xdata::ScanMessage;
  ParseDiagnostic diagnostic;
  diagnostic.status = status;
  switch (static_cast<Message>(status.message_id)) {
  case Message::unknown_field:
    diagnostic.variant = ParseVariant::unknownField;
    diagnostic.issue = "unknown-field";
    break;
  case Message::duplicate_field:
    diagnostic.variant = ParseVariant::duplicateField;
    diagnostic.issue = "duplicate-field";
    break;
  case Message::illegal_terminator:
    diagnostic.variant = ParseVariant::wrongTerminator;
    diagnostic.issue = "wrong-terminator";
    switch (static_cast<xdata::ExpectedTerminator>(status.aux)) {
    case xdata::ExpectedTerminator::object_end:
      diagnostic.expected = "object-end";
      break;
    case xdata::ExpectedTerminator::array_end:
      diagnostic.expected = "array-end";
      break;
    case xdata::ExpectedTerminator::root_eof:
      diagnostic.expected = "root-eof";
      break;
    }
    break;
  case Message::trailing_bytes:
    diagnostic.variant = ParseVariant::trailingBytes;
    diagnostic.issue = "trailing-bytes";
    break;
  case Message::input_too_large:
    diagnostic.variant = ParseVariant::resourceLimit;
    diagnostic.issue = "resource-limit";
    diagnostic.limit = "bytes";
    diagnostic.maximum = xdata::RecursiveScanLimits{}.max_bytes;
    break;
  case Message::too_many_fields:
    diagnostic.variant = ParseVariant::resourceLimit;
    diagnostic.issue = "resource-limit";
    diagnostic.limit = "fields";
    diagnostic.maximum = xdata::RecursiveScanLimits{}.max_fields;
    break;
  case Message::too_many_scopes:
    diagnostic.variant = ParseVariant::resourceLimit;
    diagnostic.issue = "resource-limit";
    diagnostic.limit = "scopes";
    diagnostic.maximum = xdata::RecursiveScanLimits{}.max_scopes;
    break;
  case Message::nesting_too_deep:
    diagnostic.variant = ParseVariant::resourceLimit;
    diagnostic.issue = "resource-limit";
    diagnostic.limit = "depth";
    diagnostic.maximum = xdata::RecursiveScanLimits{}.max_depth;
    break;
  case Message::non_object_array_element:
  case Message::truncated_vl:
  case Message::invalid_vl:
  case Message::truncated_field:
  case Message::invalid_account_id:
  case Message::invalid_amount:
  case Message::invalid_number:
  case Message::invalid_pathset:
  case Message::invalid_vector256:
  case Message::invalid_issue:
  case Message::invalid_xchain_bridge:
  case Message::noncanonical_payload:
    if (status.field_code != 0) {
      diagnostic.variant = ParseVariant::invalidField;
      diagnostic.issue = "invalid-field";
    }
    break;
  case Message::none:
  case Message::begin_past_end:
  case Message::truncated_field_header:
  case Message::noncanonical_type_code:
  case Message::noncanonical_field_code:
  case Message::too_many_nops:
  case Message::allocation_failed:
  case Message::index_size_overflow:
  case Message::invalid_index:
    break;
  }
  return diagnostic;
}

[[nodiscard]] bool defineStringProperty(JSContext *ctx, JSValueConst object,
                                        JSAtom atom, char const *value,
                                        int flags = JS_PROP_ENUMERABLE) {
  qjs::OwnedValue string(ctx, JS_NewString(ctx, value));
  return !string.isException() &&
         JS_DefinePropertyValue(ctx, object, atom, string.release(), flags) >=
             0;
}

[[nodiscard]] bool defineNumberProperty(JSContext *ctx, JSValueConst object,
                                        JSAtom atom, std::uint32_t value,
                                        int flags = JS_PROP_ENUMERABLE) {
  return JS_DefinePropertyValue(ctx, object, atom, JS_NewUint32(ctx, value),
                                flags) >= 0;
}

[[nodiscard]] bool defineParseFields(JSContext *ctx, JSValueConst object,
                                     ParseDiagnostic const &diagnostic,
                                     bool includeStrings) {
  if (fieldAtoms == nullptr) {
    JS_ThrowInternalError(ctx, "object registrar is unavailable");
    return false;
  }
  if (includeStrings &&
      (!defineStringProperty(ctx, object, diagnosticAtoms[0], "parse") ||
       !defineStringProperty(ctx, object, diagnosticAtoms[1],
                             diagnostic.issue)))
    return false;
  if (!defineNumberProperty(ctx, object, diagnosticAtoms[2],
                            diagnostic.status.offset))
    return false;
  switch (diagnostic.variant) {
  case ParseVariant::unknownField:
  case ParseVariant::invalidField:
  case ParseVariant::duplicateField:
    return defineNumberProperty(ctx, object, diagnosticAtoms[3],
                                diagnostic.status.field_code);
  case ParseVariant::wrongTerminator:
    if (includeStrings && diagnostic.expected != nullptr &&
        !defineStringProperty(ctx, object, diagnosticAtoms[4],
                              diagnostic.expected))
      return false;
    return defineNumberProperty(ctx, object, diagnosticAtoms[5],
                                diagnostic.status.field_code);
  case ParseVariant::resourceLimit:
    if (includeStrings && diagnostic.limit != nullptr &&
        !defineStringProperty(ctx, object, diagnosticAtoms[6],
                              diagnostic.limit))
      return false;
    return defineNumberProperty(ctx, object, diagnosticAtoms[7],
                                diagnostic.maximum) &&
           defineNumberProperty(ctx, object, diagnosticAtoms[8],
                                diagnostic.status.aux);
  case ParseVariant::malformed:
  case ParseVariant::trailingBytes:
    return true;
  }
  return false;
}

[[nodiscard]] JSValue newParseError(JSContext *ctx, xdata::ScanStatus status) {
  qjs::OwnedValue error(ctx, JS_NewObjectProto(ctx, JS_NULL));
  if (error.isException())
    return error.release();
  auto const diagnostic = parseDiagnostic(status);
  if (!defineParseFields(ctx, error.get(), diagnostic, true) ||
      !bindings::result_finish(ctx, error.get()))
    return JS_EXCEPTION;
  return error.release();
}

[[nodiscard]] JSValue
throwExactObjectTypeError(JSContext *ctx, char const *message,
                          xdata::ScanStatus const *status = nullptr) {
  qjs::OwnedValue error(ctx, JS_NewBareTypeErrorExact(ctx));
  if (error.isException())
    return error.release();
  qjs::OwnedValue messageValue(ctx, JS_NewString(ctx, message));
  if (messageValue.isException() ||
      JS_DefinePropertyValueStr(ctx, error.get(), "message",
                                messageValue.release(),
                                JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
    return JS_EXCEPTION;
  if (status != nullptr) {
    auto const diagnostic = parseDiagnostic(*status);
    if (!defineParseFields(ctx, error.get(), diagnostic, false))
      return JS_EXCEPTION;
  }
  return JS_Throw(ctx, error.release());
}

enum class ObjectInputStatus : std::uint8_t {
  ok,
  wrongKind,
  unusable,
};

[[nodiscard]] ObjectInputStatus
acquireObjectInput(JSContext *ctx, JSValueConst input, JSValue &backing,
                   std::uint8_t const *&bytes, std::uint32_t &size) noexcept {
  backing = JS_UNDEFINED;
  bytes = nullptr;
  size = 0;
  std::size_t spanSize = 0;
  auto status =
      JS_GetObjectByteSpanNoThrow(ctx, input, &backing, &bytes, &spanSize);
  if (status == JS_OBJECT_BYTES_WRONG_KIND)
    status = getSTBlobByteSpanNoThrow(ctx, input, &backing, &bytes, &spanSize);
  if (status == JS_OBJECT_BYTES_WRONG_KIND)
    return ObjectInputStatus::wrongKind;
  if (status != JS_OBJECT_BYTES_OK ||
      spanSize > std::numeric_limits<std::uint32_t>::max())
    return ObjectInputStatus::unusable;
  size = static_cast<std::uint32_t>(spanSize);
  return ObjectInputStatus::ok;
}

struct MintOutcome {
  JSValue value = JS_UNDEFINED;
  xdata::ScanStatus status{};
};

[[nodiscard]] xdata::ScanStatus
inputTooLargeStatus(std::uint32_t size) noexcept {
  return {
      static_cast<std::uint16_t>(xdata::ScanIssue::resource_limit),
      static_cast<std::uint16_t>(xdata::ScanMessage::input_too_large),
      0,
      0,
      size,
  };
}

[[nodiscard]] MintOutcome
mintOwnedObjectBytes(JSContext *ctx, std::uint8_t *bytes, std::uint32_t size) {
  xdata::RecursiveScanOptions options{.protocol =
                                          &xdata::xahau_static_protocol()};
  auto result = xdata::guest_exact_object_index({bytes, size}, options,
                                                {ctx, scanRealloc, scanFree});
  if (!result.ok()) {
    js_free(ctx, bytes);
    if (result.status.issue ==
        static_cast<std::uint16_t>(xdata::ScanIssue::out_of_memory))
      return {oom(ctx), result.status};
    return {JS_UNDEFINED, result.status};
  }
  qjs::OwnedValue owner(ctx, newOwner(ctx, bytes, size, result.index));
  if (owner.isException()) {
    scanFree(ctx, result.index);
    js_free(ctx, bytes);
    return {owner.release(), {}};
  }
  JSValue object = newObjectWrapper(ctx, owner.get(), 0);
  return {object, {}};
}

[[nodiscard]] MintOutcome copyAndMintObject(JSContext *ctx,
                                            std::uint8_t const *bytes,
                                            std::uint32_t size) {
  if (size > xdata::RecursiveScanLimits{}.max_bytes)
    return {JS_UNDEFINED, inputTooLargeStatus(size)};
  std::uint8_t *copy = nullptr;
  if (size != 0) {
    copy = static_cast<std::uint8_t *>(js_malloc(ctx, size));
    if (copy == nullptr)
      return {oom(ctx), {}};
    std::memcpy(copy, bytes, size);
  }
  return mintOwnedObjectBytes(ctx, copy, size);
}

struct ScopeShape {
  std::uint32_t fields = 0;
  std::uint32_t scopes = 0;
  std::uint32_t maxDepth = 0;
};

[[nodiscard]] bool scopeShape(xdata::RecursiveIndexView index,
                              std::uint32_t selectedScope, ScopeShape &output,
                              std::uint32_t recursion = 0) noexcept {
  output = {};
  auto const *scope = index.scope(selectedScope);
  if (scope == nullptr || recursion > xdata::RecursiveScanLimits{}.max_depth)
    return false;
  output.fields = scope->field_count();
  output.scopes = 1;
  for (std::uint32_t i = 0; i < scope->field_count(); ++i) {
    auto const *field = index.field(scope->first_field + i);
    if (field == nullptr)
      return false;
    if (field->child_scope == xdata::FieldRecord::no_child)
      continue;
    ScopeShape child;
    if (!scopeShape(index, field->child_scope, child, recursion + 1) ||
        child.fields >
            std::numeric_limits<std::uint32_t>::max() - output.fields ||
        child.scopes >
            std::numeric_limits<std::uint32_t>::max() - output.scopes)
      return false;
    output.fields += child.fields;
    output.scopes += child.scopes;
    auto const depth = child.maxDepth + 1;
    if (depth > output.maxDepth)
      output.maxDepth = depth;
  }
  return true;
}

enum class ReplacementInputKind : std::uint8_t {
  payload,
  indexedScope,
};

struct ReplacementInput {
  ReplacementInputKind kind = ReplacementInputKind::payload;
  Slice payload{};
  CertifiedObjectValue *owner = nullptr;
  xdata::RecursiveIndexView index{};
  std::uint32_t scope = 0;
  ScopeShape shape{};
};

enum class ReplacementInputStatus : std::uint8_t {
  ok,
  mismatch,
  unusable,
  invalid,
};

void writeBigEndian(std::uint8_t *output, std::uint32_t size,
                    std::uint64_t value) noexcept {
  for (std::uint32_t i = 0; i < size; ++i)
    output[i] = static_cast<std::uint8_t>(value >> ((size - i - 1) * 8));
}

[[nodiscard]] ReplacementInputStatus
validateReplacementPayload(JSContext *ctx,
                           xdata::StaticFieldDescriptor const &descriptor,
                           Slice payload, ReplacementInput &output) {
  xdata::RecursiveScanCounters counters;
  xdata::RecursiveScanOptions options{
      .protocol = &xdata::xahau_static_protocol(),
      .counters = &counters,
  };
  auto const status = xdata::guest_exact_validate_field_payload(
      payload, descriptor.code, 0, options);
  if (!status.ok()) {
    if (status.issue ==
        static_cast<std::uint16_t>(xdata::ScanIssue::internal_error))
      JS_ThrowInternalError(ctx, "%s",
                            xdata::scan_message_literal(status.message_id));
    return ReplacementInputStatus::invalid;
  }
  output.kind = ReplacementInputKind::payload;
  output.payload = payload;
  output.shape = {
      static_cast<std::uint32_t>(counters.material_fields),
      static_cast<std::uint32_t>(counters.scope_entries),
      0,
  };
  return ReplacementInputStatus::ok;
}

[[nodiscard]] ReplacementInputStatus acquireReplacementInput(
    JSContext *ctx, xdata::StaticFieldDescriptor const &descriptor,
    JSValueConst value, qjs::OwnedValue &backing, std::uint8_t scratch[64],
    std::uint8_t (&integerScratch)[8], ReplacementInput &output) {
  output = {};
  JSValue rawBacking = JS_UNDEFINED;
  std::uint8_t const *rawBytes = nullptr;
  std::size_t rawSize = 0;
  auto const rawStatus =
      JS_GetObjectByteSpanNoThrow(ctx, value, &rawBacking, &rawBytes, &rawSize);
  if (rawStatus == JS_OBJECT_BYTES_OK) {
    backing = qjs::OwnedValue(ctx, rawBacking);
    if (rawSize > std::numeric_limits<std::uint32_t>::max())
      return ReplacementInputStatus::unusable;
    return validateReplacementPayload(
        ctx, descriptor, {rawBytes, static_cast<std::uint32_t>(rawSize)},
        output);
  }
  if (rawStatus == JS_OBJECT_BYTES_UNUSABLE)
    return ReplacementInputStatus::unusable;

  if (descriptor.materializer == xdata::MaterializerKind::st_object &&
      isSTObject(value)) {
    auto *state =
        static_cast<ObjectState *>(JS_GetOpaque(value, objectClassId));
    auto *owner = state == nullptr ? nullptr : ownerFrom(*state);
    if (owner == nullptr)
      return ReplacementInputStatus::mismatch;
    output.kind = ReplacementInputKind::indexedScope;
    output.owner = owner;
    output.index = ownerIndex(*owner);
    output.scope = scopeId(*state);
    return scopeShape(output.index, output.scope, output.shape)
               ? ReplacementInputStatus::ok
               : ReplacementInputStatus::invalid;
  }
  if (descriptor.materializer == xdata::MaterializerKind::st_array &&
      isSTArray(value)) {
    auto *state = static_cast<ArrayState *>(JS_GetOpaque(value, arrayClassId));
    auto *owner = state == nullptr ? nullptr : ownerFrom(*state);
    if (owner == nullptr)
      return ReplacementInputStatus::mismatch;
    output.kind = ReplacementInputKind::indexedScope;
    output.owner = owner;
    output.index = ownerIndex(*owner);
    output.scope = scopeId(*state);
    return scopeShape(output.index, output.scope, output.shape)
               ? ReplacementInputStatus::ok
               : ReplacementInputStatus::invalid;
  }
  if (descriptor.materializer == xdata::MaterializerKind::number) {
    if (!JS_IsString(value))
      return ReplacementInputStatus::mismatch;
    std::size_t length = 0;
    char const *text = JS_ToCStringLen(ctx, &length, value);
    if (text == nullptr)
      return ReplacementInputStatus::invalid;
    xdata::NormalizedNumber number;
    bool const parsed = xdata::NumberRules::parse_decimal(text, length, number);
    JS_FreeCString(ctx, text);
    if (!parsed)
      return ReplacementInputStatus::invalid;
    writeBigEndian(scratch, 8, static_cast<std::uint64_t>(number.mantissa));
    writeBigEndian(scratch + 8, 4, static_cast<std::uint32_t>(number.exponent));
    return validateReplacementPayload(ctx, descriptor,
                                      {scratch, std::size_t{12}}, output);
  }
  if (descriptor.materializer == xdata::MaterializerKind::ledger_entry_type ||
      descriptor.materializer == xdata::MaterializerKind::transaction_type ||
      descriptor.materializer == xdata::MaterializerKind::transaction_result) {
    if (!JS_IsNumber(value))
      return ReplacementInputStatus::mismatch;
    double numeric = 0;
    if (JS_ToFloat64(ctx, &numeric, value) < 0)
      return ReplacementInputStatus::invalid;
    auto const maximum = descriptor.fixed_size == 1 ? 255.0 : 65535.0;
    if (!std::isfinite(numeric) || std::trunc(numeric) != numeric ||
        numeric < 0 || numeric > maximum)
      return ReplacementInputStatus::invalid;
    writeBigEndian(scratch, descriptor.fixed_size,
                   static_cast<std::uint64_t>(numeric));
    return validateReplacementPayload(ctx, descriptor,
                                      {scratch, descriptor.fixed_size}, output);
  }

  NominalPayloadView nominal;
  if (readNominalPayload(ctx, value, descriptor.materializer, integerScratch,
                         nominal)) {
    return validateReplacementPayload(ctx, descriptor,
                                      {nominal.data, nominal.size}, output);
  }
  return ReplacementInputStatus::mismatch;
}

[[nodiscard]] bool replacementFitsCaps(ObjectState const &state,
                                       std::uint32_t fieldCode,
                                       ReplacementInput const *replacement,
                                       bool removing) noexcept {
  auto *owner = ownerFrom(state);
  if (owner == nullptr)
    return false;
  auto const index = ownerIndex(*owner);
  ScopeShape source;
  if (!scopeShape(index, scopeId(state), source))
    return false;
  std::uint64_t fields = source.fields;
  std::uint64_t scopes = source.scopes;
  auto const *existing = index.find_object_field(scopeId(state), fieldCode);
  if (existing != nullptr) {
    --fields;
    if (existing->child_scope != xdata::FieldRecord::no_child) {
      ScopeShape child;
      if (!scopeShape(index, existing->child_scope, child) ||
          child.fields > fields || child.scopes > scopes)
        return false;
      fields -= child.fields;
      scopes -= child.scopes;
    }
  }
  if (!removing && replacement != nullptr) {
    fields += 1 + replacement->shape.fields;
    scopes += replacement->shape.scopes;
    if (replacement->kind == ReplacementInputKind::indexedScope &&
        replacement->shape.maxDepth >= xdata::RecursiveScanLimits{}.max_depth)
      return false;
  }
  return fields <= xdata::RecursiveScanLimits{}.max_fields &&
         scopes <= xdata::RecursiveScanLimits{}.max_scopes;
}

[[nodiscard]] JSValue replacementFailure(JSContext *ctx, char const *operation,
                                         xdata::ScanStatus status) {
  if (status.issue ==
      static_cast<std::uint16_t>(xdata::ScanIssue::out_of_memory))
    return oom(ctx);
  if (status.issue ==
          static_cast<std::uint16_t>(xdata::ScanIssue::malformed_data) ||
      status.issue ==
          static_cast<std::uint16_t>(xdata::ScanIssue::resource_limit)) {
    char message[256];
    int const written =
        std::snprintf(message, sizeof(message), "%s: %s", operation,
                      xdata::scan_message_literal(status.message_id));
    return written >= 0 && static_cast<std::size_t>(written) < sizeof(message)
               ? throwExactObjectTypeError(ctx, message)
               : throwExactObjectTypeError(ctx, "object replacement failed");
  }
  return JS_ThrowInternalError(ctx, "%s: %s", operation,
                               xdata::scan_message_literal(status.message_id));
}

[[nodiscard]] JSValue objectWithField(JSContext *ctx, JSValueConst thisValue,
                                      int argc, JSValueConst *argv) {
  auto *state = objectState(ctx, thisValue);
  if (state == nullptr)
    return JS_EXCEPTION;
  std::uint32_t code = 0;
  if (!resolveFieldArgument(ctx, argc, argv, code, true))
    return JS_HasException(ctx) ? JS_EXCEPTION
                                : throwExactObjectTypeError(
                                      ctx, "STObject.withField: unknown field");
  auto const *descriptor = xdata::xahau_static_protocol().field_by_code(code);
  if (descriptor == nullptr ||
      descriptor->material_ordinal == xdata::ProtocolView::no_ordinal)
    return throwExactObjectTypeError(ctx, "STObject.withField: unknown field");
  if (argc < 2 || JS_IsUndefined(argv[1]))
    return throwExactObjectTypeError(
        ctx, "STObject.withField: value must be explicit");

  qjs::OwnedValue backing(ctx);
  std::uint8_t scratch[64]{};
  std::uint8_t integerScratch[8]{};
  ReplacementInput replacement;
  auto const acquired = acquireReplacementInput(
      ctx, *descriptor, argv[1], backing, scratch, integerScratch, replacement);
  if (acquired != ReplacementInputStatus::ok) {
    if (JS_HasException(ctx))
      return JS_EXCEPTION;
    return throwExactObjectTypeError(
        ctx, acquired == ReplacementInputStatus::unusable
                 ? "STObject.withField: byte backing is detached or unusable"
             : acquired == ReplacementInputStatus::invalid
                 ? "STObject.withField: invalid canonical field value"
                 : "STObject.withField: value type does not match field");
  }
  if (!replacementFitsCaps(*state, code, &replacement, false))
    return throwExactObjectTypeError(
        ctx, "STObject.withField: replacement exceeds object limits");

  auto *owner = ownerFrom(*state);
  auto const sourceBytes = ownerBytes(*owner);
  auto const sourceIndex = ownerIndex(*owner);
  xdata::CanonicalReplacementSizeResult measured;
  if (replacement.kind == ReplacementInputKind::indexedScope) {
    measured = xdata::canonical_object_with_indexed_field_size(
        sourceBytes, sourceIndex, scopeId(*state), code,
        ownerBytes(*replacement.owner), replacement.index, replacement.scope);
  } else {
    measured = xdata::canonical_object_with_field_size(
        sourceBytes, sourceIndex, scopeId(*state), code, replacement.payload);
  }
  if (!measured.ok())
    return replacementFailure(ctx, "STObject.withField", measured.status);
  if (measured.size > xdata::RecursiveScanLimits{}.max_bytes)
    return throwExactObjectTypeError(
        ctx, "STObject.withField: replacement exceeds byte limit");
  auto *output =
      measured.size == 0
          ? nullptr
          : static_cast<std::uint8_t *>(js_malloc(ctx, measured.size));
  if (measured.size != 0 && output == nullptr)
    return oom(ctx);
  xdata::CanonicalReplacementWriteResult written;
  if (replacement.kind == ReplacementInputKind::indexedScope) {
    written = xdata::canonical_object_with_indexed_field_write(
        sourceBytes, sourceIndex, scopeId(*state), code,
        ownerBytes(*replacement.owner), replacement.index, replacement.scope,
        output, measured.size);
  } else {
    written = xdata::canonical_object_with_field_write(
        sourceBytes, sourceIndex, scopeId(*state), code, replacement.payload,
        output, measured.size);
  }
  backing = qjs::OwnedValue(ctx);
  if (!written.ok() || written.written != measured.size) {
    js_free(ctx, output);
    return written.ok()
               ? JS_ThrowInternalError(
                     ctx, "STObject.withField: replacement size changed")
               : replacementFailure(ctx, "STObject.withField", written.status);
  }
  auto outcome = mintOwnedObjectBytes(ctx, output, measured.size);
  if (JS_IsException(outcome.value) || !JS_IsUndefined(outcome.value))
    return outcome.value;
  return JS_ThrowInternalError(
      ctx, "STObject.withField: emitted object failed certification");
}

[[nodiscard]] JSValue objectWithoutField(JSContext *ctx, JSValueConst thisValue,
                                         int argc, JSValueConst *argv) {
  auto *state = objectState(ctx, thisValue);
  if (state == nullptr)
    return JS_EXCEPTION;
  std::uint32_t code = 0;
  if (!resolveFieldArgument(ctx, argc, argv, code, true))
    return JS_HasException(ctx)
               ? JS_EXCEPTION
               : throwExactObjectTypeError(
                     ctx, "STObject.withoutField: unknown field");
  auto const *descriptor = xdata::xahau_static_protocol().field_by_code(code);
  if (descriptor == nullptr ||
      descriptor->material_ordinal == xdata::ProtocolView::no_ordinal)
    return throwExactObjectTypeError(ctx,
                                     "STObject.withoutField: unknown field");
  if (locateObjectField(*state, code).field == nullptr)
    return JS_DupValue(ctx, thisValue);
  if (!replacementFitsCaps(*state, code, nullptr, true))
    return JS_ThrowInternalError(
        ctx, "STObject.withoutField: invalid certified source shape");

  auto *owner = ownerFrom(*state);
  auto const sourceBytes = ownerBytes(*owner);
  auto const sourceIndex = ownerIndex(*owner);
  auto const measured = xdata::canonical_object_without_field_size(
      sourceBytes, sourceIndex, scopeId(*state), code);
  if (!measured.ok() || measured.no_op())
    return measured.ok()
               ? JS_ThrowInternalError(
                     ctx, "STObject.withoutField: present field became absent")
               : replacementFailure(ctx, "STObject.withoutField",
                                    measured.status);
  auto *output =
      measured.size == 0
          ? nullptr
          : static_cast<std::uint8_t *>(js_malloc(ctx, measured.size));
  if (measured.size != 0 && output == nullptr)
    return oom(ctx);
  auto const written = xdata::canonical_object_without_field_write(
      sourceBytes, sourceIndex, scopeId(*state), code, output, measured.size);
  if (!written.ok() || written.no_op() || written.written != measured.size) {
    js_free(ctx, output);
    return written.ok()
               ? JS_ThrowInternalError(
                     ctx, "STObject.withoutField: replacement size changed")
               : replacementFailure(ctx, "STObject.withoutField",
                                    written.status);
  }
  auto outcome = mintOwnedObjectBytes(ctx, output, measured.size);
  if (JS_IsException(outcome.value) || !JS_IsUndefined(outcome.value))
    return outcome.value;
  return JS_ThrowInternalError(
      ctx, "STObject.withoutField: emitted object failed certification");
}

[[nodiscard]] bool
isObjectDataFailure(xdata::ScanStatus const &status) noexcept {
  auto const issue = static_cast<xdata::ScanIssue>(status.issue);
  return issue == xdata::ScanIssue::malformed_data ||
         issue == xdata::ScanIssue::resource_limit;
}

} // namespace

bool registerObjectTypes(JSContext *ctx) {
  clearRegistrar(JS_GetRuntime(ctx));
  if (!qjs::defineClass(JS_GetRuntime(ctx), &ownerClassId, &ownerClass) ||
      !qjs::defineClass(JS_GetRuntime(ctx), &objectClassId, &objectClass) ||
      !qjs::defineClass(JS_GetRuntime(ctx), &arrayClassId, &arrayClass) ||
      !qjs::defineClass(JS_GetRuntime(ctx), &iteratorClassId, &iteratorClass) ||
      !jshookz::provider::qjs::registerByteClass(
          objectClassId,
          jshookz::provider::qjs::ByteClassFamily::serializedType,
          objectToBytes)) {
    clearRegistrar(JS_GetRuntime(ctx));
    return atomFailure(ctx);
  }

  qjs::OwnedValue currentOwner(ctx, JS_GetClassProto(ctx, ownerClassId));
  qjs::OwnedValue currentArray(ctx, JS_GetClassProto(ctx, arrayClassId));
  qjs::OwnedValue currentIterator(ctx, JS_GetClassProto(ctx, iteratorClassId));
  bool const prototypesReady =
      JS_IsObject(currentOwner.get()) && JS_IsObject(currentArray.get()) &&
      JS_IsObject(currentIterator.get());

  qjs::OwnedValue ownerPrototype(ctx);
  qjs::OwnedValue stagedArrayPrototype(ctx);
  qjs::OwnedValue stagedIteratorPrototype(ctx);
  if (!prototypesReady) {
    ownerPrototype = qjs::OwnedValue(
        ctx, qjs::makePrototype(ctx, std::span<JSCFunctionListEntry const>{}));
    stagedArrayPrototype =
        qjs::OwnedValue(ctx, qjs::makePrototype(ctx, arrayPrototype));
    stagedIteratorPrototype =
        qjs::OwnedValue(ctx, qjs::makePrototype(ctx, iteratorPrototype));
  }
  StagedObjectNouns stagedObjectNouns(ctx);
  if (ownerPrototype.isException() || stagedArrayPrototype.isException() ||
      stagedIteratorPrototype.isException() ||
      (!objectNounsReady && !stageObjectNouns(ctx, stagedObjectNouns)) ||
      !registerAtoms(ctx)) {
    clearRegistrar(JS_GetRuntime(ctx));
    return atomFailure(ctx);
  }

  if (!prototypesReady) {
    // Allocation-free commit: retries never observe a partially installed
    // prototype set or a partially registered generated-atom accelerator.
    JS_SetClassProto(ctx, ownerClassId, ownerPrototype.release());
    JS_SetClassProto(ctx, arrayClassId, stagedArrayPrototype.release());
    JS_SetClassProto(ctx, iteratorClassId, stagedIteratorPrototype.release());
  }
  if (!objectNounsReady) {
    JS_SetClassProto(
        ctx, objectClassId,
        JS_DupValue(ctx, stagedObjectNouns.prototypes[
                             static_cast<std::size_t>(ObjectView::st_object)]));
    for (std::size_t i = 0; i < kObjectViewCount; ++i) {
      objectPrototypes[i] = stagedObjectNouns.prototypes[i];
      stagedObjectNouns.prototypes[i] = JS_UNDEFINED;
      objectConstructors[i] = stagedObjectNouns.constructors[i];
      stagedObjectNouns.constructors[i] = JS_UNDEFINED;
    }
    objectNounsReady = true;
  }
  return !JS_HasException(ctx);
}

void unregisterObjectTypes(JSRuntime *runtime) noexcept {
  clearRegistrar(runtime);
  clearObjectNouns(runtime);
}

bool publishObjectTypes(JSContext *ctx, JSValueConst global) {
  if (!objectNounsReady || !JS_IsObject(global))
    return false;
  std::array<JSAtom, kObjectViewCount> atoms{};
  std::size_t published = 0;
  for (std::size_t i = 0; i < kObjectViewCount; ++i) {
    auto const &view = static_data::OBJECT_VIEWS[i];
    auto const *name = static_data::OBJECT_VIEW_NAME_BYTES + view.name_offset;
    atoms[i] = JS_NewAtomLen(ctx, name, view.name_size);
    if (atoms[i] == JS_ATOM_NULL)
      break;
    if (JS_DefinePropertyValue(ctx, global, atoms[i],
                               JS_DupValue(ctx, objectConstructors[i]),
                               JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE) < 0)
      break;
    ++published;
  }
  if (published != kObjectViewCount) {
    bool const hadException = JS_HasException(ctx);
    qjs::OwnedValue exception(
        ctx, hadException ? JS_GetException(ctx) : JS_UNDEFINED);
    for (std::size_t i = 0; i < published; ++i)
      JS_DeleteProperty(ctx, global, atoms[i], 0);
    for (JSAtom atom : atoms) {
      if (atom != JS_ATOM_NULL)
        JS_FreeAtom(ctx, atom);
    }
    if (hadException)
      JS_Throw(ctx, exception.release());
    return atomFailure(ctx);
  }
  for (JSAtom atom : atoms)
    JS_FreeAtom(ctx, atom);
  return true;
}

bool registeredFieldCode(JSAtom atom, std::uint32_t &code) noexcept {
  code = 0;
  auto const *name = fieldNameByAtom(atom);
  if (name == nullptr || (name->flags & xdata::field_name_serialized) == 0)
    return false;
  code = name->code;
  return true;
}

JSValue makeCertifiedObjectCopy(JSContext *ctx, std::uint8_t const *bytes,
                                std::uint32_t size) {
  if (size != 0 && bytes == nullptr)
    return throwExactObjectTypeError(ctx, "object bytes are unavailable");
  auto outcome = copyAndMintObject(ctx, bytes, size);
  if (JS_IsException(outcome.value) || !JS_IsUndefined(outcome.value))
    return outcome.value;
  if (!isObjectDataFailure(outcome.status))
    return JS_ThrowInternalError(
        ctx, "%s", xdata::scan_message_literal(outcome.status.message_id));
  return throwExactObjectTypeError(
      ctx, xdata::scan_message_literal(outcome.status.message_id),
      &outcome.status);
}

std::uint32_t certifiedObjectMaxBytes() noexcept {
  return xdata::RecursiveScanLimits{}.max_bytes;
}

namespace {

[[nodiscard]] JSValue makeCertifiedExactViewOwned(
    JSContext *ctx, std::uint8_t *bytes, std::uint32_t size,
    ObjectView expected, char const *name) {
  if ((size != 0 && bytes == nullptr) ||
      size > xdata::RecursiveScanLimits{}.max_bytes) {
    js_free(ctx, bytes);
    return JS_ThrowInternalError(
        ctx, "trusted object bytes violate the provider size invariant");
  }
  auto outcome = mintOwnedObjectBytes(ctx, bytes, size);
  if (JS_IsException(outcome.value))
    return outcome.value;
  if (!JS_IsUndefined(outcome.value)) {
    auto const *state = static_cast<ObjectState const *>(
        JS_GetOpaque(outcome.value, objectClassId));
    auto const view = state == nullptr ? ObjectView::st_object
                                       : static_cast<ObjectView>(state->view);
    if (view == expected)
      return outcome.value;
    JS_FreeValue(ctx, outcome.value);
    return JS_ThrowInternalError(
        ctx, "trusted ledger object is not a complete %s", name);
  }
  return JS_ThrowInternalError(
      ctx, "trusted %s certification failed: %s", name,
      xdata::scan_message_literal(outcome.status.message_id));
}

} // namespace

JSValue makeCertifiedTransactionOwned(JSContext *ctx, std::uint8_t *bytes,
                                      std::uint32_t size) {
  if ((size != 0 && bytes == nullptr) ||
      size > xdata::RecursiveScanLimits{}.max_bytes) {
    js_free(ctx, bytes);
    return JS_ThrowInternalError(
        ctx, "trusted object bytes violate the provider size invariant");
  }
  auto outcome = mintOwnedObjectBytes(ctx, bytes, size);
  if (JS_IsException(outcome.value))
    return outcome.value;
  if (!JS_IsUndefined(outcome.value)) {
    auto const *state = static_cast<ObjectState const *>(
        JS_GetOpaque(outcome.value, objectClassId));
    auto const view = state == nullptr ? ObjectView::st_object
                                       : static_cast<ObjectView>(state->view);
    bool transaction = false;
    for (auto const &family : static_data::OBJECT_FAMILIES) {
      if (family.base_view == ObjectView::transaction) {
        transaction = viewBelongsToFamily(view, family);
        break;
      }
    }
    if (transaction)
      return outcome.value;
    JS_FreeValue(ctx, outcome.value);
    return JS_ThrowInternalError(
        ctx, "trusted originating object is not a complete Transaction");
  }
  return JS_ThrowInternalError(
      ctx, "trusted object certification failed: %s",
      xdata::scan_message_literal(outcome.status.message_id));
}

JSValue makeCertifiedAccountRootOwned(JSContext *ctx, std::uint8_t *bytes,
                                      std::uint32_t size) {
  return makeCertifiedExactViewOwned(
      ctx, bytes, size, ObjectView::account_root, "AccountRoot");
}

JSValue makeCertifiedURITokenOwned(JSContext* ctx, std::uint8_t* bytes,
                                   std::uint32_t size) {
  return makeCertifiedExactViewOwned(
      ctx, bytes, size, ObjectView::u_r_i_token, "URIToken");
}

JSValue makeCertifiedObjectOwned(JSContext *ctx, std::uint8_t *bytes,
                                 std::uint32_t size) {
  if ((size != 0 && bytes == nullptr) ||
      size > xdata::RecursiveScanLimits{}.max_bytes) {
    js_free(ctx, bytes);
    return JS_ThrowInternalError(
        ctx, "trusted object bytes violate the provider size invariant");
  }
  auto outcome = mintOwnedObjectBytes(ctx, bytes, size);
  if (JS_IsException(outcome.value) || !JS_IsUndefined(outcome.value))
    return outcome.value;
  return JS_ThrowInternalError(
      ctx, "trusted object certification failed: %s",
      xdata::scan_message_literal(outcome.status.message_id));
}

JSValue makeCertifiedHookLedgerOwned(JSContext *ctx, std::uint8_t *bytes,
                                     std::uint32_t size) {
  return makeCertifiedExactViewOwned(
      ctx, bytes, size, ObjectView::hook_ledger, "HookLedger");
}

JSValue makeCertifiedHookDefinitionOwned(JSContext *ctx, std::uint8_t *bytes,
                                         std::uint32_t size) {
  return makeCertifiedExactViewOwned(
      ctx, bytes, size, ObjectView::hook_definition, "HookDefinition");
}

// @binding provider:util.validateObject
JSValue validateObjectBytes(JSContext *ctx, JSValueConst input) {
  JSValue backing = JS_UNDEFINED;
  std::uint8_t const *bytes = nullptr;
  std::uint32_t size = 0;
  auto const inputStatus = acquireObjectInput(ctx, input, backing, bytes, size);
  if (inputStatus != ObjectInputStatus::ok) {
    JS_FreeValue(ctx, backing);
    if (JS_HasException(ctx))
      return JS_EXCEPTION;
    return throwExactObjectTypeError(
        ctx, inputStatus == ObjectInputStatus::wrongKind
                 ? "expected Uint8Array, ArrayBuffer, or STBlob"
                 : "object byte backing is detached or unusable");
  }
  xdata::RecursiveScanOptions options{.protocol =
                                          &xdata::xahau_static_protocol()};
  auto const status =
      xdata::guest_exact_validate_object({bytes, size}, options);
  JS_FreeValue(ctx, backing);
  if (status.issue ==
      static_cast<std::uint16_t>(xdata::ScanIssue::internal_error))
    return JS_ThrowInternalError(
        ctx, "%s", xdata::scan_message_literal(status.message_id));
  return JS_NewBool(ctx, status.ok());
}

namespace {

[[nodiscard]] MintOutcome mintObjectInput(JSContext *ctx, JSValueConst input) {
  JSValue backing = JS_UNDEFINED;
  std::uint8_t const *bytes = nullptr;
  std::uint32_t size = 0;
  auto const inputStatus = acquireObjectInput(ctx, input, backing, bytes, size);
  if (inputStatus != ObjectInputStatus::ok) {
    JS_FreeValue(ctx, backing);
    if (JS_HasException(ctx))
      return {JS_EXCEPTION, {}};
    return {throwExactObjectTypeError(
                ctx, inputStatus == ObjectInputStatus::wrongKind
                         ? "expected Uint8Array, ArrayBuffer, or STBlob"
                         : "object byte backing is detached or unusable"),
            {}};
  }

  if (size > xdata::RecursiveScanLimits{}.max_bytes) {
    JS_FreeValue(ctx, backing);
    return {JS_UNDEFINED, inputTooLargeStatus(size)};
  }
  std::uint8_t *copy = nullptr;
  if (size != 0) {
    copy = static_cast<std::uint8_t *>(js_malloc(ctx, size));
    if (copy == nullptr) {
      JS_FreeValue(ctx, backing);
      return {oom(ctx), {}};
    }
    std::memcpy(copy, bytes, size);
  }
  // The sole ingress-backing edge is dead before certification/indexing.
  JS_FreeValue(ctx, backing);
  return mintOwnedObjectBytes(ctx, copy, size);
}

} // namespace

// @binding provider:util.safeDecodeObject
JSValue safeDecodeObjectBytes(JSContext *ctx, JSValueConst input) {
  auto outcome = mintObjectInput(ctx, input);
  if (JS_IsException(outcome.value))
    return outcome.value;
  if (!JS_IsUndefined(outcome.value))
    return bindings::result_success(ctx, outcome.value);
  if (!isObjectDataFailure(outcome.status))
    return JS_ThrowInternalError(
        ctx, "%s", xdata::scan_message_literal(outcome.status.message_id));
  qjs::OwnedValue error(ctx, newParseError(ctx, outcome.status));
  if (error.isException())
    return error.release();
  return bindings::result_failure(ctx, error.release());
}

// @binding provider:util.decodeObject
JSValue decodeObjectBytes(JSContext *ctx, JSValueConst input) {
  auto outcome = mintObjectInput(ctx, input);
  if (JS_IsException(outcome.value) || !JS_IsUndefined(outcome.value))
    return outcome.value;
  if (!isObjectDataFailure(outcome.status))
    return JS_ThrowInternalError(
        ctx, "%s", xdata::scan_message_literal(outcome.status.message_id));
  return throwExactObjectTypeError(
      ctx, xdata::scan_message_literal(outcome.status.message_id),
      &outcome.status);
}

bool isSTObject(JSValueConst value) noexcept {
  if (!JS_IsObject(value) || JS_GetClassID(value) != objectClassId)
    return false;
  auto const *state =
      static_cast<ObjectState const *>(JS_GetOpaque(value, objectClassId));
  return state != nullptr && ownerFrom(*state) != nullptr;
}

bool isSTArray(JSValueConst value) noexcept {
  if (!JS_IsObject(value) || JS_GetClassID(value) != arrayClassId)
    return false;
  auto const *state =
      static_cast<ArrayState const *>(JS_GetOpaque(value, arrayClassId));
  return state != nullptr && ownerFrom(*state) != nullptr;
}

#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
bool test::inspectObjectRoot(JSValueConst value,
                             test::ObjectRootMetrics &metrics) noexcept {
  metrics = {};
  if (!JS_IsObject(value) || JS_GetClassID(value) != objectClassId)
    return false;
  auto const *state =
      static_cast<ObjectState const *>(JS_GetOpaque(value, objectClassId));
  auto const *owner = state == nullptr ? nullptr : ownerFrom(*state);
  if (state == nullptr || owner == nullptr)
    return false;
  auto const index = ownerIndex(*owner);
  auto const *header = index.header();
  if (header == nullptr || scopeId(*state) != header->root_scope ||
      !xdata::recursive_index_size(header->scope_count, header->field_count,
                                   metrics.indexBytes))
    return false;
  metrics.view = state->view;
  metrics.cacheCount = state->cacheCount;
  metrics.ownerByteCount = owner->byteCount;
  metrics.ownerFieldCount = header->field_count;
  metrics.ownerScopeCount = header->scope_count;
  metrics.indexBlocks = owner->index == nullptr ? 0 : 1;
  metrics.indexStructurallyValid = index.structurally_valid();
  return true;
}

bool test::inspectArrayCache(JSValueConst value,
                             test::ArrayCacheMetrics &metrics) noexcept {
  metrics = {};
  if (!JS_IsObject(value) || JS_GetClassID(value) != arrayClassId)
    return false;
  auto const *state =
      static_cast<ArrayState const *>(JS_GetOpaque(value, arrayClassId));
  auto const *owner = state == nullptr ? nullptr : ownerFrom(*state);
  if (state == nullptr || owner == nullptr)
    return false;

  auto const index = ownerIndex(*owner);
  auto const *header = index.header();
  auto const *scope = index.scope(scopeId(*state));
  if (header == nullptr || scope == nullptr)
    return false;
  metrics.arrayLength = scope->field_count();
  metrics.ownerByteCount = owner->byteCount;
  metrics.ownerFieldCount = header->field_count;
  metrics.ownerScopeCount = header->scope_count;
  metrics.rootBytes = sizeof(ArrayCacheRoot);
  metrics.branchBytes = sizeof(ArrayCacheBranch);
  metrics.leafBytes = sizeof(ArrayCacheLeaf);
  if (state->cache == nullptr)
    return true;

  metrics.rootPresent = true;
  metrics.branchCount = state->cache->branchCount;
  metrics.pageCount = state->cache->pageCount;
  metrics.valueCount = state->cache->valueCount;
  metrics.reservedVersion = state->cache->reservedVersion;
  for (auto const *branch : state->cache->branches) {
    if (branch == nullptr)
      continue;
    ++metrics.reachableBranches;
    for (auto const *leaf : branch->pages) {
      if (leaf == nullptr)
        continue;
      ++metrics.reachablePages;
      for (JSValueConst cached : leaf->values) {
        if (!JS_IsUndefined(cached))
          ++metrics.reachableValues;
      }
    }
  }
  metrics.requestedBytes = metrics.rootBytes +
                           metrics.reachableBranches * metrics.branchBytes +
                           metrics.reachablePages * metrics.leafBytes;
  metrics.allocationCount =
      1 + metrics.reachableBranches + metrics.reachablePages;
  return true;
}
#endif

bool isCertifiedObjectRange(JSContext *ctx, JSValueConst ownerValue,
                            std::uint8_t const *bytes,
                            std::uint32_t length) noexcept {
  if (ctx == nullptr || JS_GetRuntime(ctx) == nullptr)
    return false;
  auto const *owner = ownerFrom(ownerValue);
  if (owner == nullptr || (length != 0 && bytes == nullptr))
    return false;
  auto const base = reinterpret_cast<std::uintptr_t>(owner->bytes);
  auto const begin = reinterpret_cast<std::uintptr_t>(bytes);
  if (begin < base)
    return false;
  auto const offset = begin - base;
  return offset <= owner->byteCount && length <= owner->byteCount - offset;
}

} // namespace jshookz::provider::types
