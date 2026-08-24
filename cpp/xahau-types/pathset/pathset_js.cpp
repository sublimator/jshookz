#include "pathset/pathset_js.hpp"

#include "object/nominal_payload.hpp"
#include "quickjs.hpp"

#include <catl/core/types.h>
#include <catl/xdata/parser-context.h>
#include <catl/xdata/pathset-rules.h>
#include <catl/xdata/types/pathset.h>

#include "runtime_profile_limits.h"

#include <array>
#include <cstring>
#include <limits>
#include <span>
#include <type_traits>

namespace jshookz::provider::types {
namespace {

namespace qjs = jshookz::provider::qjs;
namespace coreqjs = jshookz::qjs;
namespace xdata = catl::xdata;

constexpr std::uint32_t kMaximumPayloadBytes =
    xdata::xahau_profile_limits::serialized_object_max_bytes;
constexpr std::uint8_t kAccountCached = 1u << 0;
constexpr std::uint8_t kCurrencyCached = 1u << 1;
constexpr std::uint8_t kIssuerCached = 1u << 2;

JSClassID pathSetClassId;
JSClassID pathClassId;
JSClassID pathHopClassId;
JSClassID iteratorClassId;
PathSetLeafMaterializers leafMaterializers{};

struct PathSetState {
  JSValue owner = JS_UNDEFINED;
  std::uint8_t const *bytes = nullptr;
  std::uint32_t *directory = nullptr;
  std::uint32_t length = 0;
  std::uint32_t pathCount = 0;
  std::uint32_t hopCount = 0;
};

struct PathState {
  JSValue parent = JS_UNDEFINED;
  std::uint32_t pathIndex = 0;
};

struct PathHopState {
  JSValue parent = JS_UNDEFINED;
  std::array<std::uint8_t, 20> account{};
  std::array<std::uint8_t, 20> currency{};
  std::array<std::uint8_t, 20> issuer{};
  std::uint32_t hopOrdinal = 0;
  std::uint8_t cachedComponents = 0;
};

enum class IteratorKind : std::uint8_t {
  pathSet,
  path,
};

struct IteratorState {
  JSValue parent = JS_UNDEFINED;
  std::uint32_t cursor = 0;
  IteratorKind kind = IteratorKind::pathSet;
};

static_assert(std::is_trivially_copyable_v<PathSetState>);
static_assert(std::is_trivially_copyable_v<PathState>);
static_assert(std::is_trivially_copyable_v<PathHopState>);
static_assert(std::is_trivially_copyable_v<IteratorState>);

[[nodiscard]] JSValue oom(JSContext *ctx) {
  return JS_HasException(ctx) ? JS_EXCEPTION : JS_ThrowOutOfMemory(ctx);
}

template <class State> [[nodiscard]] State *allocateState(JSContext *ctx) {
  auto *state = static_cast<State *>(js_mallocz(ctx, sizeof(State)));
  if (state == nullptr)
    (void)oom(ctx);
  return state;
}

template <class State>
void freeState(JSRuntime *runtime, State *state) noexcept {
  js_free_rt(runtime, state);
}

void pathSetFinalizer(JSRuntime *runtime, JSValue value) {
  auto *state =
      static_cast<PathSetState *>(JS_GetOpaque(value, pathSetClassId));
  if (state == nullptr)
    return;
  if (state->directory != nullptr)
    js_free_rt(runtime, state->directory);
  JS_FreeValueRT(runtime, state->owner);
  freeState(runtime, state);
}

void pathSetMark(JSRuntime *runtime, JSValueConst value, JS_MarkFunc *mark) {
  auto const *state =
      static_cast<PathSetState const *>(JS_GetOpaque(value, pathSetClassId));
  if (state != nullptr)
    JS_MarkValue(runtime, state->owner, mark);
}

void pathFinalizer(JSRuntime *runtime, JSValue value) {
  auto *state = static_cast<PathState *>(JS_GetOpaque(value, pathClassId));
  if (state == nullptr)
    return;
  JS_FreeValueRT(runtime, state->parent);
  freeState(runtime, state);
}

void pathMark(JSRuntime *runtime, JSValueConst value, JS_MarkFunc *mark) {
  auto const *state =
      static_cast<PathState const *>(JS_GetOpaque(value, pathClassId));
  if (state != nullptr)
    JS_MarkValue(runtime, state->parent, mark);
}

void pathHopFinalizer(JSRuntime *runtime, JSValue value) {
  auto *state =
      static_cast<PathHopState *>(JS_GetOpaque(value, pathHopClassId));
  if (state == nullptr)
    return;
  JS_FreeValueRT(runtime, state->parent);
  freeState(runtime, state);
}

void pathHopMark(JSRuntime *runtime, JSValueConst value, JS_MarkFunc *mark) {
  auto const *state =
      static_cast<PathHopState const *>(JS_GetOpaque(value, pathHopClassId));
  if (state != nullptr)
    JS_MarkValue(runtime, state->parent, mark);
}

void iteratorFinalizer(JSRuntime *runtime, JSValue value) {
  auto *state =
      static_cast<IteratorState *>(JS_GetOpaque(value, iteratorClassId));
  if (state == nullptr)
    return;
  JS_FreeValueRT(runtime, state->parent);
  freeState(runtime, state);
}

void iteratorMark(JSRuntime *runtime, JSValueConst value, JS_MarkFunc *mark) {
  auto const *state =
      static_cast<IteratorState const *>(JS_GetOpaque(value, iteratorClassId));
  if (state != nullptr)
    JS_MarkValue(runtime, state->parent, mark);
}

JSClassDef const pathSetClass = {
    .class_name = "PathSet",
    .finalizer = pathSetFinalizer,
    .gc_mark = pathSetMark,
    .call = nullptr,
    .exotic = nullptr,
};

JSClassDef const pathClass = {
    .class_name = "Path",
    .finalizer = pathFinalizer,
    .gc_mark = pathMark,
    .call = nullptr,
    .exotic = nullptr,
};

JSClassDef const pathHopClass = {
    .class_name = "PathHop",
    .finalizer = pathHopFinalizer,
    .gc_mark = pathHopMark,
    .call = nullptr,
    .exotic = nullptr,
};

JSClassDef const iteratorClass = {
    .class_name = "Path Iterator",
    .finalizer = iteratorFinalizer,
    .gc_mark = iteratorMark,
    .call = nullptr,
    .exotic = nullptr,
};

[[nodiscard]] PathSetState *pathSetState(JSContext *ctx, JSValueConst value) {
  return static_cast<PathSetState *>(JS_GetOpaque2(ctx, value, pathSetClassId));
}

[[nodiscard]] PathState *pathState(JSContext *ctx, JSValueConst value) {
  return static_cast<PathState *>(JS_GetOpaque2(ctx, value, pathClassId));
}

[[nodiscard]] PathHopState *pathHopState(JSContext *ctx, JSValueConst value) {
  return static_cast<PathHopState *>(JS_GetOpaque2(ctx, value, pathHopClassId));
}

[[nodiscard]] bool preventExtensions(JSContext *ctx, JSValue value) {
  return JS_PreventExtensions(ctx, value) >= 0;
}

[[nodiscard]] bool certifyPathSet(std::uint8_t const *bytes,
                                  std::uint32_t length) noexcept {
  if (bytes == nullptr || length == 0 || length > kMaximumPayloadBytes)
    return false;
  xdata::ParserContext parser{Slice{bytes, length}};
  xdata::PathSetNullSink sink;
  return xdata::PathSetRules::walk<xdata::PathSetRuleMode::CertifyWire>(parser,
                                                                        sink) &&
         !parser.failed() && parser.pos() == length;
}

[[nodiscard]] bool ensureDirectory(JSContext *ctx, PathSetState &state) {
  if (state.directory != nullptr)
    return true;
  auto const shape =
      xdata::PathSetRules::measure_directory({state.bytes, state.length});
  if (!shape || shape->paths > std::numeric_limits<std::uint32_t>::max() ||
      shape->hops > std::numeric_limits<std::uint32_t>::max() ||
      shape->bytes > std::numeric_limits<std::uint32_t>::max()) {
    JS_ThrowInternalError(ctx, "PathSet directory exceeds provider limits");
    return false;
  }
  auto *directory = static_cast<std::uint32_t *>(js_malloc(ctx, shape->bytes));
  if (directory == nullptr) {
    (void)oom(ctx);
    return false;
  }
  if (!xdata::PathSetRules::fill_directory(
          {state.bytes, state.length}, *shape,
          std::span<std::uint32_t>{directory, shape->words})) {
    js_free(ctx, directory);
    JS_ThrowInternalError(ctx, "PathSet directory fill failed");
    return false;
  }
  state.pathCount = static_cast<std::uint32_t>(shape->paths);
  state.hopCount = static_cast<std::uint32_t>(shape->hops);
  state.directory = directory;
  return true;
}

[[nodiscard]] JSValue newPathSet(JSContext *ctx, JSValueConst owner,
                                 std::uint8_t const *bytes,
                                 std::uint32_t length) {
  if (!JS_IsObject(owner) ||
      !leafMaterializers.certifiedRange(ctx, owner, bytes, length))
    return JS_HasException(ctx)
               ? JS_EXCEPTION
               : JS_ThrowTypeError(ctx, "PathSet: certified owner is required");
  if (!certifyPathSet(bytes, length))
    return JS_ThrowTypeError(ctx, "PathSet: invalid canonical bytes");

  JSValue value = JS_NewObjectClass(ctx, pathSetClassId);
  if (JS_IsException(value))
    return value;
  auto *state = allocateState<PathSetState>(ctx);
  if (state == nullptr) {
    JS_FreeValue(ctx, value);
    return JS_EXCEPTION;
  }
  state->owner = JS_DupValue(ctx, owner);
  state->bytes = bytes;
  state->length = length;
  JS_SetOpaque(value, state);
  if (!preventExtensions(ctx, value)) {
    JS_FreeValue(ctx, value);
    return JS_EXCEPTION;
  }
  return value;
}

[[nodiscard]] JSValue newPath(JSContext *ctx, JSValueConst parent,
                              std::uint32_t index) {
  JSValue value = JS_NewObjectClass(ctx, pathClassId);
  if (JS_IsException(value))
    return value;
  auto *state = allocateState<PathState>(ctx);
  if (state == nullptr) {
    JS_FreeValue(ctx, value);
    return JS_EXCEPTION;
  }
  state->parent = JS_DupValue(ctx, parent);
  state->pathIndex = index;
  JS_SetOpaque(value, state);
  if (!preventExtensions(ctx, value)) {
    JS_FreeValue(ctx, value);
    return JS_EXCEPTION;
  }
  return value;
}

[[nodiscard]] JSValue newPathHop(JSContext *ctx, JSValueConst parent,
                                 std::uint32_t ordinal) {
  JSValue value = JS_NewObjectClass(ctx, pathHopClassId);
  if (JS_IsException(value))
    return value;
  auto *state = allocateState<PathHopState>(ctx);
  if (state == nullptr) {
    JS_FreeValue(ctx, value);
    return JS_EXCEPTION;
  }
  state->parent = JS_DupValue(ctx, parent);
  state->hopOrdinal = ordinal;
  JS_SetOpaque(value, state);
  if (!preventExtensions(ctx, value)) {
    JS_FreeValue(ctx, value);
    return JS_EXCEPTION;
  }
  return value;
}

[[nodiscard]] JSValue newIterator(JSContext *ctx, JSValueConst parent,
                                  IteratorKind kind) {
  JSValue value = JS_NewObjectClass(ctx, iteratorClassId);
  if (JS_IsException(value))
    return value;
  auto *state = allocateState<IteratorState>(ctx);
  if (state == nullptr) {
    JS_FreeValue(ctx, value);
    return JS_EXCEPTION;
  }
  state->parent = JS_DupValue(ctx, parent);
  state->kind = kind;
  JS_SetOpaque(value, state);
  if (!preventExtensions(ctx, value)) {
    JS_FreeValue(ctx, value);
    return JS_EXCEPTION;
  }
  return value;
}

[[nodiscard]] bool readIndex(JSContext *ctx, int argc, JSValueConst *argv,
                             std::uint32_t &index) {
  if (argc < 1) {
    JS_ThrowTypeError(ctx, "at() requires an index");
    return false;
  }
  std::uint64_t wide = 0;
  if (JS_ToIndex(ctx, &wide, argv[0]) < 0)
    return false;
  if (wide > std::numeric_limits<std::uint32_t>::max()) {
    index = std::numeric_limits<std::uint32_t>::max();
    return true;
  }
  index = static_cast<std::uint32_t>(wide);
  return true;
}

[[nodiscard]] PathSetState *parentState(JSContext *ctx, JSValueConst parent) {
  return static_cast<PathSetState *>(
      JS_GetOpaque2(ctx, parent, pathSetClassId));
}

[[nodiscard]] JSValue pathSetLength(JSContext *ctx, JSValueConst thisValue) {
  auto *state = pathSetState(ctx, thisValue);
  if (state == nullptr || !ensureDirectory(ctx, *state))
    return JS_EXCEPTION;
  return JS_NewUint32(ctx, state->pathCount);
}

[[nodiscard]] JSValue pathSetAt(JSContext *ctx, JSValueConst thisValue,
                                int argc, JSValueConst *argv) {
  auto *state = pathSetState(ctx, thisValue);
  std::uint32_t index = 0;
  if (state == nullptr || !readIndex(ctx, argc, argv, index))
    return JS_EXCEPTION;
  if (!ensureDirectory(ctx, *state))
    return JS_EXCEPTION;
  return index < state->pathCount ? newPath(ctx, thisValue, index)
                                  : JS_UNDEFINED;
}

[[nodiscard]] JSValue pathSetToBytes(JSContext *ctx, JSValueConst thisValue,
                                     int, JSValueConst *) {
  auto const *state = pathSetState(ctx, thisValue);
  return state == nullptr ? JS_EXCEPTION
                          : qjs::uint8Array(ctx, {state->bytes, state->length});
}

[[nodiscard]] JSValue pathLength(JSContext *ctx, JSValueConst thisValue) {
  auto *path = pathState(ctx, thisValue);
  if (path == nullptr)
    return JS_EXCEPTION;
  auto *parent = parentState(ctx, path->parent);
  if (parent == nullptr || !ensureDirectory(ctx, *parent))
    return JS_EXCEPTION;
  if (path->pathIndex >= parent->pathCount)
    return JS_ThrowInternalError(ctx, "Path provenance is invalid");
  std::uint32_t const begin = parent->directory[path->pathIndex];
  std::uint32_t const end = parent->directory[path->pathIndex + 1];
  return JS_NewUint32(ctx, end - begin);
}

[[nodiscard]] JSValue pathAt(JSContext *ctx, JSValueConst thisValue, int argc,
                             JSValueConst *argv) {
  auto *path = pathState(ctx, thisValue);
  std::uint32_t index = 0;
  if (path == nullptr || !readIndex(ctx, argc, argv, index))
    return JS_EXCEPTION;
  auto *parent = parentState(ctx, path->parent);
  if (parent == nullptr || !ensureDirectory(ctx, *parent))
    return JS_EXCEPTION;
  if (path->pathIndex >= parent->pathCount)
    return JS_ThrowInternalError(ctx, "Path provenance is invalid");
  std::uint32_t const begin = parent->directory[path->pathIndex];
  std::uint32_t const end = parent->directory[path->pathIndex + 1];
  return index < end - begin ? newPathHop(ctx, path->parent, begin + index)
                             : JS_UNDEFINED;
}

struct HopComponents {
  std::uint8_t const *account = nullptr;
  std::uint8_t const *currency = nullptr;
  std::uint8_t const *issuer = nullptr;
};

[[nodiscard]] bool resolveHop(PathSetState const &parent, std::uint32_t ordinal,
                              HopComponents &output) noexcept {
  if (parent.directory == nullptr || ordinal >= parent.hopCount)
    return false;
  std::uint32_t const pathWords = parent.pathCount + 1;
  std::uint32_t const offset = parent.directory[pathWords + ordinal];
  if (offset >= parent.length)
    return false;
  std::uint8_t const type = parent.bytes[offset];
  std::uint32_t cursor = offset + 1;
  auto take = [&](std::uint8_t const *&component) noexcept {
    if (cursor > parent.length || parent.length - cursor < 20)
      return false;
    component = parent.bytes + cursor;
    cursor += 20;
    return true;
  };
  if ((type & xdata::PathSet::TYPE_ACCOUNT) != 0 && !take(output.account))
    return false;
  if ((type & xdata::PathSet::TYPE_CURRENCY) != 0 && !take(output.currency))
    return false;
  if ((type & xdata::PathSet::TYPE_ISSUER) != 0 && !take(output.issuer))
    return false;
  return true;
}

enum class Component : std::uint8_t {
  account,
  currency,
  issuer,
};

[[nodiscard]] JSValue pathHopComponent(JSContext *ctx, JSValueConst thisValue,
                                       Component component) {
  auto *hop = pathHopState(ctx, thisValue);
  if (hop == nullptr)
    return JS_EXCEPTION;
  auto *parent = parentState(ctx, hop->parent);
  if (parent == nullptr || !ensureDirectory(ctx, *parent))
    return JS_EXCEPTION;
  HopComponents resolved;
  if (!resolveHop(*parent, hop->hopOrdinal, resolved))
    return JS_ThrowInternalError(ctx, "PathHop provenance is invalid");

  std::uint8_t const *source = nullptr;
  std::array<std::uint8_t, 20> *cache = nullptr;
  std::uint8_t bit = 0;
  bool isCurrency = false;
  switch (component) {
  case Component::account:
    source = resolved.account;
    cache = &hop->account;
    bit = kAccountCached;
    break;
  case Component::currency:
    source = resolved.currency;
    cache = &hop->currency;
    bit = kCurrencyCached;
    isCurrency = true;
    break;
  case Component::issuer:
    source = resolved.issuer;
    cache = &hop->issuer;
    bit = kIssuerCached;
    break;
  }
  if (source == nullptr)
    return JS_UNDEFINED;
  if ((hop->cachedComponents & bit) == 0) {
    std::memcpy(cache->data(), source, cache->size());
    hop->cachedComponents |= bit;
  }
  return isCurrency
             ? leafMaterializers.currency(ctx, cache->data(), cache->size())
             : leafMaterializers.accountID(ctx, cache->data(), cache->size());
}

[[nodiscard]] JSValue pathHopAccount(JSContext *ctx, JSValueConst value) {
  return pathHopComponent(ctx, value, Component::account);
}

[[nodiscard]] JSValue pathHopCurrency(JSContext *ctx, JSValueConst value) {
  return pathHopComponent(ctx, value, Component::currency);
}

[[nodiscard]] JSValue pathHopIssuer(JSContext *ctx, JSValueConst value) {
  return pathHopComponent(ctx, value, Component::issuer);
}

[[nodiscard]] JSValue pathSetIterator(JSContext *ctx, JSValueConst value, int,
                                      JSValueConst *) {
  auto *state = pathSetState(ctx, value);
  if (state == nullptr || !ensureDirectory(ctx, *state))
    return JS_EXCEPTION;
  return newIterator(ctx, value, IteratorKind::pathSet);
}

[[nodiscard]] JSValue pathIterator(JSContext *ctx, JSValueConst value, int,
                                   JSValueConst *) {
  auto *path = pathState(ctx, value);
  if (path == nullptr)
    return JS_EXCEPTION;
  auto *parent = parentState(ctx, path->parent);
  if (parent == nullptr || !ensureDirectory(ctx, *parent))
    return JS_EXCEPTION;
  return newIterator(ctx, value, IteratorKind::path);
}

[[nodiscard]] JSValue iteratorSelf(JSContext *ctx, JSValueConst value, int,
                                   JSValueConst *) {
  auto *state =
      static_cast<IteratorState *>(JS_GetOpaque2(ctx, value, iteratorClassId));
  return state == nullptr ? JS_EXCEPTION : JS_DupValue(ctx, value);
}

[[nodiscard]] JSValue iteratorEnvelope(JSContext *ctx, JSValue value,
                                       bool done) {
  qjs::OwnedValue result(ctx, JS_NewObject(ctx));
  if (result.isException()) {
    JS_FreeValue(ctx, value);
    return result.release();
  }
  if (JS_SetPropertyStr(ctx, result.get(), "value", value) < 0)
    return JS_EXCEPTION;
  if (JS_SetPropertyStr(ctx, result.get(), "done", JS_NewBool(ctx, done)) < 0)
    return JS_EXCEPTION;
  return result.release();
}

[[nodiscard]] JSValue iteratorNext(JSContext *ctx, JSValueConst thisValue, int,
                                   JSValueConst *) {
  auto *iterator = static_cast<IteratorState *>(
      JS_GetOpaque2(ctx, thisValue, iteratorClassId));
  if (iterator == nullptr)
    return JS_EXCEPTION;

  JSValue value = JS_UNDEFINED;
  bool done = false;
  if (iterator->kind == IteratorKind::pathSet) {
    auto *parent = pathSetState(ctx, iterator->parent);
    if (parent == nullptr || !ensureDirectory(ctx, *parent))
      return JS_EXCEPTION;
    done = iterator->cursor >= parent->pathCount;
    if (!done)
      value = newPath(ctx, iterator->parent, iterator->cursor);
  } else {
    auto *path = pathState(ctx, iterator->parent);
    if (path == nullptr)
      return JS_EXCEPTION;
    auto *parent = parentState(ctx, path->parent);
    if (parent == nullptr || !ensureDirectory(ctx, *parent))
      return JS_EXCEPTION;
    if (path->pathIndex >= parent->pathCount)
      return JS_ThrowInternalError(ctx, "Path provenance is invalid");
    std::uint32_t const begin = parent->directory[path->pathIndex];
    std::uint32_t const count = parent->directory[path->pathIndex + 1] - begin;
    done = iterator->cursor >= count;
    if (!done)
      value = newPathHop(ctx, path->parent, begin + iterator->cursor);
  }
  if (JS_IsException(value))
    return value;
  JSValue const envelope = iteratorEnvelope(ctx, value, done);
  if (!JS_IsException(envelope) && !done)
    ++iterator->cursor;
  return envelope;
}

JSCFunctionListEntry const pathSetPrototype[] = {
    // @binding provider:PathSet.length
    JS_CGETSET_DEF("length", pathSetLength, nullptr),
    // @binding provider:PathSet.at
    JS_CFUNC_DEF("at", 1, pathSetAt),
    // @binding provider:PathSet.toBytes
    JS_CFUNC_DEF("toBytes", 0, pathSetToBytes),
    // @binding provider:PathSet.[Symbol.iterator]
    JS_CFUNC_DEF("[Symbol.iterator]", 0, pathSetIterator),
};

JSCFunctionListEntry const pathPrototype[] = {
    // @binding provider:Path.length
    JS_CGETSET_DEF("length", pathLength, nullptr),
    // @binding provider:Path.at
    JS_CFUNC_DEF("at", 1, pathAt),
    // @binding provider:Path.[Symbol.iterator]
    JS_CFUNC_DEF("[Symbol.iterator]", 0, pathIterator),
};

JSCFunctionListEntry const pathHopPrototype[] = {
    // @binding provider:PathHop.account
    JS_CGETSET_DEF("account", pathHopAccount, nullptr),
    // @binding provider:PathHop.currency
    JS_CGETSET_DEF("currency", pathHopCurrency, nullptr),
    // @binding provider:PathHop.issuer
    JS_CGETSET_DEF("issuer", pathHopIssuer, nullptr),
};

JSCFunctionListEntry const iteratorPrototype[] = {
    JS_CFUNC_DEF("next", 0, iteratorNext),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, iteratorSelf),
};

} // namespace

bool registerPathSet(JSContext *ctx, PathSetLeafMaterializers const &leaves) {
  if (leaves.accountID == nullptr || leaves.currency == nullptr ||
      leaves.certifiedRange == nullptr)
    return false;
  leafMaterializers = leaves;
  auto *runtime = JS_GetRuntime(ctx);
  if (!coreqjs::defineClass(runtime, &pathSetClassId, &pathSetClass) ||
      !coreqjs::defineClass(runtime, &pathClassId, &pathClass) ||
      !coreqjs::defineClass(runtime, &pathHopClassId, &pathHopClass) ||
      !coreqjs::defineClass(runtime, &iteratorClassId, &iteratorClass) ||
      !qjs::registerByteClass(pathSetClassId,
                              qjs::ByteClassFamily::serializedType,
                              pathSetToBytes) ||
      !coreqjs::installPrototype(ctx, pathSetClassId, pathSetPrototype) ||
      !coreqjs::installPrototype(ctx, pathClassId, pathPrototype) ||
      !coreqjs::installPrototype(ctx, pathHopClassId, pathHopPrototype) ||
      !coreqjs::installPrototype(ctx, iteratorClassId, iteratorPrototype))
    return false;
  return !JS_HasException(ctx);
}

JSValue makePathSetBytes(JSContext *ctx, JSValueConst owner,
                         std::uint8_t const *bytes, std::uint32_t length) {
  return newPathSet(ctx, owner, bytes, length);
}

bool isPathSet(JSValueConst value) noexcept {
  return JS_IsObject(value) && JS_GetClassID(value) == pathSetClassId &&
         JS_GetOpaque(value, pathSetClassId) != nullptr;
}

bool isPath(JSValueConst value) noexcept {
  return JS_IsObject(value) && JS_GetClassID(value) == pathClassId &&
         JS_GetOpaque(value, pathClassId) != nullptr;
}

bool isPathHop(JSValueConst value) noexcept {
  return JS_IsObject(value) && JS_GetClassID(value) == pathHopClassId &&
         JS_GetOpaque(value, pathHopClassId) != nullptr;
}

bool detail::readPathSetNominalPayload(JSContext *ctx, JSValueConst input,
                                       NominalPayloadView &output) noexcept {
  output = {};
  if (ctx == nullptr || !JS_IsObject(input) ||
      JS_GetClassID(input) != pathSetClassId)
    return false;
  auto const *state = static_cast<PathSetState const *>(
      JS_GetOpaque(input, pathSetClassId));
  if (state == nullptr || state->length == 0 ||
      state->length > kMaximumPayloadBytes || state->bytes == nullptr ||
      leafMaterializers.certifiedRange == nullptr ||
      !leafMaterializers.certifiedRange(ctx, state->owner, state->bytes,
                                        state->length))
    return false;
  output = {state->bytes, state->length};
  return true;
}

} // namespace jshookz::provider::types
