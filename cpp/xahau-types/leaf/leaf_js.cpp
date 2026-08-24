#include "leaf/leaf.hpp"

#include "js.hpp"
#include "object/nominal_payload.hpp"
#include "object/object.hpp"
#include "quickjs.hpp"
#include "runtime_profile_limits.h"

#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
#include "tests/object_gc_lifetime_probe_hooks.hpp"
#endif

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

namespace jshookz::provider::types {
namespace {

namespace qjs = jshookz::provider::qjs;
namespace coreqjs = ::jshookz::qjs;
namespace xdata = catl::xdata;

enum class LeafKind : std::uint8_t {
  hash128,
  hash160,
  hash192,
  currency,
  issue,
  vector256,
  xchainBridge,
  count,
};

constexpr std::size_t kindCount = static_cast<std::size_t>(LeafKind::count);

JSClassID classIds[kindCount]{};
JSClassID vectorIteratorClassId{};
JSRuntime *registeredRuntime{};

template <std::size_t N> struct FixedState {
  JSValue owner = JS_UNDEFINED;
  std::uint8_t const *data = nullptr;
  std::uint8_t bytes[N];
};

struct IssueState {
  JSValue owner = JS_UNDEFINED;
  std::uint8_t const *data = nullptr;
  std::uint8_t bytes[44];
  std::uint32_t length;
  JSValue cache[3];
};

struct VectorCacheLeaf {
  JSValue values[32];
};

struct VectorCacheBranch {
  VectorCacheLeaf *pages[32];
};

struct VectorCacheRoot {
  std::uint32_t branchCount;
  std::uint32_t pageCount;
  std::uint32_t valueCount;
  std::uint32_t reservedVersion;
  VectorCacheBranch *branches[32];
};

struct VectorState {
  JSValue owner = JS_UNDEFINED;
  std::uint8_t const *data = nullptr;
  std::uint32_t byteLength;
  std::uint32_t count;
  VectorCacheRoot *cache;
};

struct VectorIteratorState {
  JSValue vector;
  std::uint32_t cursor;
};

struct BridgePart {
  std::uint32_t offset;
  std::uint32_t length;
};

struct BridgeState {
  JSValue owner = JS_UNDEFINED;
  std::uint8_t const *data = nullptr;
  std::uint8_t bytes[130];
  std::uint32_t length;
  BridgePart parts[4];
  JSValue cache[4];
};

static_assert(offsetof(VectorCacheRoot, branches) == 16);
static_assert(sizeof(VectorCacheRoot) == 16 + 32 * sizeof(void *));
static_assert(sizeof(VectorCacheBranch) == 32 * sizeof(void *));
static_assert(sizeof(VectorCacheLeaf) == 32 * sizeof(JSValue));
static_assert(sizeof(BridgePart) == 8);

[[nodiscard]] constexpr std::size_t kindIndex(LeafKind kind) noexcept {
  return static_cast<std::size_t>(kind);
}

[[nodiscard]] JSClassID classId(LeafKind kind) noexcept {
  return classIds[kindIndex(kind)];
}

#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
[[nodiscard]] test::HiddenEdge ownerEdge(LeafKind kind) noexcept {
  switch (kind) {
  case LeafKind::hash128:
    return test::HiddenEdge::hash128Owner;
  case LeafKind::hash160:
    return test::HiddenEdge::hash160Owner;
  case LeafKind::hash192:
    return test::HiddenEdge::hash192Owner;
  case LeafKind::currency:
    return test::HiddenEdge::currencyOwner;
  case LeafKind::issue:
    return test::HiddenEdge::issueOwner;
  case LeafKind::vector256:
    return test::HiddenEdge::vector256Owner;
  case LeafKind::xchainBridge:
    return test::HiddenEdge::xchainBridgeOwner;
  case LeafKind::count:
    break;
  }
  return test::HiddenEdge::none;
}

[[nodiscard]] test::TrackedEntity trackedEntity(LeafKind kind) noexcept {
  switch (kind) {
  case LeafKind::hash128:
    return test::TrackedEntity::hash128;
  case LeafKind::hash160:
    return test::TrackedEntity::hash160;
  case LeafKind::hash192:
    return test::TrackedEntity::hash192;
  case LeafKind::currency:
    return test::TrackedEntity::currency;
  case LeafKind::issue:
    return test::TrackedEntity::issue;
  case LeafKind::vector256:
    return test::TrackedEntity::vector256;
  case LeafKind::xchainBridge:
    return test::TrackedEntity::xchainBridge;
  case LeafKind::count:
    break;
  }
  return test::TrackedEntity::count;
}

[[nodiscard]] bool plantLeafCycles(JSContext *ctx, LeafKind kind,
                                   JSValueConst owner, JSValueConst value) {
  if (!test::gcProbePlantCycle(ctx, ownerEdge(kind), owner, value, value))
    return false;
  switch (kind) {
  case LeafKind::hash192:
  case LeafKind::currency:
    return test::gcProbePlantPendingOwnerCycle(
        ctx, test::HiddenEdge::issueCacheValue, owner);
  case LeafKind::issue:
    return test::gcProbePlantPendingOwnerCycle(
        ctx, test::HiddenEdge::xchainBridgeCacheValue, owner);
  default:
    return true;
  }
}
#endif

[[nodiscard]] JSValue oom(JSContext *ctx) {
  return JS_HasException(ctx) ? JS_EXCEPTION : JS_ThrowOutOfMemory(ctx);
}

[[nodiscard]] JSValue internalError(JSContext *ctx, char const *message) {
  return JS_ThrowInternalError(ctx, "%s", message);
}

[[nodiscard]] bool runtimeReady(JSContext *ctx) noexcept {
  return registeredRuntime != nullptr &&
         registeredRuntime == JS_GetRuntime(ctx);
}

template <class State>
[[nodiscard]] State *exactState(JSContext *ctx, JSValueConst value,
                                LeafKind kind) {
  auto *state = static_cast<State *>(JS_GetOpaque2(ctx, value, classId(kind)));
  if (state == nullptr)
    return nullptr;
  if (!runtimeReady(ctx)) {
    JS_ThrowTypeError(ctx, "rich leaf has invalid provider provenance");
    return nullptr;
  }
  return state;
}

template <class State>
[[nodiscard]] JSValue newStateObject(JSContext *ctx, LeafKind kind,
                                     State *state) {
  if (!runtimeReady(ctx)) {
    js_free(ctx, state);
    return internalError(ctx, "rich leaf classes are not registered");
  }
  JSValue object = JS_NewObjectClass(ctx, classId(kind));
  if (JS_IsException(object)) {
    js_free(ctx, state);
    return object;
  }
  JS_SetOpaque(object, state);
  if (JS_PreventExtensions(ctx, object) < 0) {
    JS_FreeValue(ctx, object);
    return JS_EXCEPTION;
  }
  return object;
}

template <std::size_t N>
[[nodiscard]] JSValue newFixed(JSContext *ctx, LeafKind kind,
                               JSValueConst owner, std::uint8_t const *bytes,
                               std::uint32_t length, char const *typeName) {
  if (length != N || bytes == nullptr)
    return internalError(ctx, typeName);
  if (!JS_IsUndefined(owner) &&
      !isCertifiedObjectRange(ctx, owner, bytes, length))
    return JS_HasException(ctx)
               ? JS_EXCEPTION
               : JS_ThrowTypeError(ctx,
                                   "rich leaf certified owner is required");
  auto *state =
      static_cast<FixedState<N> *>(js_malloc(ctx, sizeof(FixedState<N>)));
  if (state == nullptr)
    return oom(ctx);
  state->owner = JS_UNDEFINED;
  if (JS_IsUndefined(owner)) {
    std::memcpy(state->bytes, bytes, N);
    state->data = state->bytes;
  } else {
    state->data = bytes;
  }
  JSValue value = newStateObject(ctx, kind, state);
  if (!JS_IsException(value) && !JS_IsUndefined(owner)) {
    state->owner = JS_DupValue(ctx, owner);
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
    test::gcProbeCreated(trackedEntity(kind), value);
    if (!plantLeafCycles(ctx, kind, owner, value)) {
      JS_FreeValue(ctx, value);
      return JS_EXCEPTION;
    }
#endif
  }
  return value;
}

template <std::size_t N>
[[nodiscard]] JSValue
newFixedDerived(JSContext *ctx, LeafKind kind, JSValueConst owner,
                std::uint8_t const *bytes, std::uint32_t length,
                char const *typeName) {
  if (JS_IsUndefined(owner) || length != N || bytes == nullptr)
    return internalError(ctx, typeName);
  auto *state =
      static_cast<FixedState<N> *>(js_malloc(ctx, sizeof(FixedState<N>)));
  if (state == nullptr)
    return oom(ctx);
  state->owner = JS_UNDEFINED;
  std::memcpy(state->bytes, bytes, N);
  state->data = state->bytes;
  JSValue value = newStateObject(ctx, kind, state);
  if (!JS_IsException(value)) {
    state->owner = JS_DupValue(ctx, owner);
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
    test::gcProbeCreated(trackedEntity(kind), value);
    if (!plantLeafCycles(ctx, kind, owner, value)) {
      JS_FreeValue(ctx, value);
      return JS_EXCEPTION;
    }
#endif
  }
  return value;
}

template <class State>
void freePlainState(JSRuntime *runtime, JSValueConst value,
                    LeafKind kind) noexcept {
  auto *state = static_cast<State *>(JS_GetOpaque(value, classId(kind)));
  if (state == nullptr)
    return;
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
  if (!JS_IsUndefined(state->owner))
    test::gcProbeFinalized(trackedEntity(kind), value);
#endif
  JS_FreeValueRT(runtime, state->owner);
  js_free_rt(runtime, state);
}

template <class State>
void markPlainState(JSRuntime *runtime, JSValueConst value, JS_MarkFunc *mark,
                    LeafKind kind) noexcept {
  auto const *state =
      static_cast<State const *>(JS_GetOpaque(value, classId(kind)));
  if (state != nullptr) {
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
    if (!test::gcProbeMarkEnabled(ownerEdge(kind)))
      return;
#endif
    JS_MarkValue(runtime, state->owner, mark);
  }
}

void hash128Finalizer(JSRuntime *rt, JSValue value) {
  freePlainState<FixedState<16>>(rt, value, LeafKind::hash128);
}

void hash160Finalizer(JSRuntime *rt, JSValue value) {
  freePlainState<FixedState<20>>(rt, value, LeafKind::hash160);
}

void hash192Finalizer(JSRuntime *rt, JSValue value) {
  freePlainState<FixedState<24>>(rt, value, LeafKind::hash192);
}

void currencyFinalizer(JSRuntime *rt, JSValue value) {
  freePlainState<FixedState<20>>(rt, value, LeafKind::currency);
}

void hash128Mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark) {
  markPlainState<FixedState<16>>(rt, value, mark, LeafKind::hash128);
}

void hash160Mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark) {
  markPlainState<FixedState<20>>(rt, value, mark, LeafKind::hash160);
}

void hash192Mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark) {
  markPlainState<FixedState<24>>(rt, value, mark, LeafKind::hash192);
}

void currencyMark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark) {
  markPlainState<FixedState<20>>(rt, value, mark, LeafKind::currency);
}

void issueFinalizer(JSRuntime *runtime, JSValue value) {
  auto *state =
      static_cast<IssueState *>(JS_GetOpaque(value, classId(LeafKind::issue)));
  if (state == nullptr)
    return;
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
  if (!JS_IsUndefined(state->owner))
    test::gcProbeFinalized(test::TrackedEntity::issue, value);
#endif
  JS_FreeValueRT(runtime, state->owner);
  for (JSValue cached : state->cache)
    JS_FreeValueRT(runtime, cached);
  js_free_rt(runtime, state);
}

void issueMark(JSRuntime *runtime, JSValueConst value, JS_MarkFunc *mark) {
  auto *state =
      static_cast<IssueState *>(JS_GetOpaque(value, classId(LeafKind::issue)));
  if (state == nullptr)
    return;
  if (
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
      test::gcProbeMarkEnabled(test::HiddenEdge::issueOwner)
#else
      true
#endif
  )
    JS_MarkValue(runtime, state->owner, mark);
  for (JSValue cached : state->cache) {
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
    if (!test::gcProbeMarkEnabled(test::HiddenEdge::issueCacheValue))
      continue;
#endif
    JS_MarkValue(runtime, cached, mark);
  }
}

[[nodiscard]] std::uint8_t const *
vectorBytes(VectorState const *state) noexcept {
  return state->data;
}

void vectorFinalizer(JSRuntime *runtime, JSValue value) {
  auto *state = static_cast<VectorState *>(
      JS_GetOpaque(value, classId(LeafKind::vector256)));
  if (state == nullptr)
    return;
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
  if (!JS_IsUndefined(state->owner))
    test::gcProbeFinalized(test::TrackedEntity::vector256, value);
#endif
  JS_FreeValueRT(runtime, state->owner);
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
  js_free_rt(runtime, state);
}

void vectorMark(JSRuntime *runtime, JSValueConst value, JS_MarkFunc *mark) {
  auto *state = static_cast<VectorState *>(
      JS_GetOpaque(value, classId(LeafKind::vector256)));
  if (state == nullptr)
    return;
  if (
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
      test::gcProbeMarkEnabled(test::HiddenEdge::vector256Owner)
#else
      true
#endif
  )
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
        if (!test::gcProbeMarkEnabled(test::HiddenEdge::vector256CacheValue))
          continue;
#endif
        JS_MarkValue(runtime, cached, mark);
      }
    }
  }
}

void vectorIteratorFinalizer(JSRuntime *runtime, JSValue value) {
  auto *state = static_cast<VectorIteratorState *>(
      JS_GetOpaque(value, vectorIteratorClassId));
  if (state == nullptr)
    return;
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
  if (test::gcProbeSelected(test::HiddenEdge::vector256Iterator))
    test::gcProbeFinalized(test::TrackedEntity::vectorIterator, value);
#endif
  JS_FreeValueRT(runtime, state->vector);
  js_free_rt(runtime, state);
}

void vectorIteratorMark(JSRuntime *runtime, JSValueConst value,
                        JS_MarkFunc *mark) {
  auto *state = static_cast<VectorIteratorState *>(
      JS_GetOpaque(value, vectorIteratorClassId));
  if (state != nullptr) {
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
    if (!test::gcProbeMarkEnabled(test::HiddenEdge::vector256Iterator))
      return;
#endif
    JS_MarkValue(runtime, state->vector, mark);
  }
}

void bridgeFinalizer(JSRuntime *runtime, JSValue value) {
  auto *state = static_cast<BridgeState *>(
      JS_GetOpaque(value, classId(LeafKind::xchainBridge)));
  if (state == nullptr)
    return;
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
  if (!JS_IsUndefined(state->owner))
    test::gcProbeFinalized(test::TrackedEntity::xchainBridge, value);
#endif
  JS_FreeValueRT(runtime, state->owner);
  for (JSValue cached : state->cache)
    JS_FreeValueRT(runtime, cached);
  js_free_rt(runtime, state);
}

void bridgeMark(JSRuntime *runtime, JSValueConst value, JS_MarkFunc *mark) {
  auto *state = static_cast<BridgeState *>(
      JS_GetOpaque(value, classId(LeafKind::xchainBridge)));
  if (state == nullptr)
    return;
  if (
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
      test::gcProbeMarkEnabled(test::HiddenEdge::xchainBridgeOwner)
#else
      true
#endif
  )
    JS_MarkValue(runtime, state->owner, mark);
  for (JSValue cached : state->cache) {
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
    if (!test::gcProbeMarkEnabled(test::HiddenEdge::xchainBridgeCacheValue))
      continue;
#endif
    JS_MarkValue(runtime, cached, mark);
  }
}

JSClassExoticMethods vectorExotic{};

JSClassDef classDefs[kindCount] = {
    {.class_name = "Hash128",
     .finalizer = hash128Finalizer,
     .gc_mark = hash128Mark},
    {.class_name = "Hash160",
     .finalizer = hash160Finalizer,
     .gc_mark = hash160Mark},
    {.class_name = "Hash192",
     .finalizer = hash192Finalizer,
     .gc_mark = hash192Mark},
    {.class_name = "Currency",
     .finalizer = currencyFinalizer,
     .gc_mark = currencyMark},
    {.class_name = "Issue", .finalizer = issueFinalizer, .gc_mark = issueMark},
    {.class_name = "Vector256",
     .finalizer = vectorFinalizer,
     .gc_mark = vectorMark,
     .exotic = &vectorExotic},
    {.class_name = "XChainBridge",
     .finalizer = bridgeFinalizer,
     .gc_mark = bridgeMark},
};

JSClassDef vectorIteratorClass = {
    .class_name = "Vector256 Iterator",
    .finalizer = vectorIteratorFinalizer,
    .gc_mark = vectorIteratorMark,
};

[[nodiscard]] bool allZero(std::uint8_t const *bytes,
                           std::uint32_t length) noexcept {
  std::uint8_t combined = 0;
  for (std::uint32_t i = 0; i < length; ++i)
    combined |= bytes[i];
  return combined == 0;
}

[[nodiscard]] bool isNoAccount(std::uint8_t const *bytes) noexcept {
  for (std::uint32_t i = 0; i < 19; ++i) {
    if (bytes[i] != 0)
      return false;
  }
  return bytes[19] == 1;
}

[[nodiscard]] bool validIssue(std::uint8_t const *bytes,
                              std::uint32_t length) noexcept {
  if (bytes == nullptr)
    return false;
  if (length == 20)
    return allZero(bytes, 20);
  if (length == 44)
    return !allZero(bytes, 20) && isNoAccount(bytes + 20);
  if (length != 40 || allZero(bytes, 20) || isNoAccount(bytes + 20))
    return false;
  return !allZero(bytes + 20, 20);
}

[[nodiscard]] std::uint32_t issueExtent(std::uint8_t const *bytes,
                                        std::uint32_t remaining) noexcept {
  if (remaining < 20)
    return 0;
  if (allZero(bytes, 20))
    return 20;
  if (remaining < 40)
    return 0;
  if (isNoAccount(bytes + 20))
    return remaining >= 44 ? 44 : 0;
  return 40;
}

template <std::size_t N>
[[nodiscard]] FixedState<N> *fixedState(JSContext *ctx, JSValueConst value,
                                        LeafKind kind) {
  return exactState<FixedState<N>>(ctx, value, kind);
}

[[nodiscard]] JSValue fixedBytes(JSContext *ctx, JSValueConst value,
                                 LeafKind kind, std::uint32_t length) {
  std::uint8_t const *bytes = nullptr;
  switch (length) {
  case 16: {
    auto *state = fixedState<16>(ctx, value, kind);
    if (state != nullptr)
      bytes = state->data;
    break;
  }
  case 20: {
    auto *state = fixedState<20>(ctx, value, kind);
    if (state != nullptr)
      bytes = state->data;
    break;
  }
  case 24: {
    auto *state = fixedState<24>(ctx, value, kind);
    if (state != nullptr)
      bytes = state->data;
    break;
  }
  default:
    break;
  }
  return bytes == nullptr ? JS_EXCEPTION
                          : qjs::uint8Array(ctx, {bytes, length});
}

template <std::size_t N>
[[nodiscard]] JSValue fixedHex(JSContext *ctx, JSValueConst value,
                               LeafKind kind) {
  auto *state = fixedState<N>(ctx, value, kind);
  if (state == nullptr)
    return JS_EXCEPTION;
  constexpr char digits[] = "0123456789ABCDEF";
  char output[N * 2];
  for (std::size_t i = 0; i < N; ++i) {
    output[i * 2] = digits[state->data[i] >> 4];
    output[i * 2 + 1] = digits[state->data[i] & 0x0f];
  }
  return JS_NewStringLen(ctx, output, sizeof(output));
}

template <std::size_t N>
[[nodiscard]] JSValue fixedIsZero(JSContext *ctx, JSValueConst value,
                                  LeafKind kind) {
  auto *state = fixedState<N>(ctx, value, kind);
  return state == nullptr ? JS_EXCEPTION
                          : JS_NewBool(ctx, allZero(state->data, N));
}

template <std::size_t N>
[[nodiscard]] JSValue fixedEquals(JSContext *ctx, JSValueConst left,
                                  JSValueConst right, LeafKind kind) {
  auto *lhs = fixedState<N>(ctx, left, kind);
  if (lhs == nullptr)
    return JS_EXCEPTION;
  auto *rhs = fixedState<N>(ctx, right, kind);
  if (rhs == nullptr)
    return JS_EXCEPTION;
  return JS_NewBool(ctx, std::memcmp(lhs->data, rhs->data, N) == 0);
}

template <std::size_t N>
[[nodiscard]] JSValue fixedCompare(JSContext *ctx, JSValueConst left,
                                   JSValueConst right, LeafKind kind) {
  auto *lhs = fixedState<N>(ctx, left, kind);
  if (lhs == nullptr)
    return JS_EXCEPTION;
  auto *rhs = fixedState<N>(ctx, right, kind);
  if (rhs == nullptr)
    return JS_EXCEPTION;
  int const order = std::memcmp(lhs->data, rhs->data, N);
  return JS_NewInt32(ctx, order < 0 ? -1 : order > 0 ? 1 : 0);
}

JSValue hashByteLength(JSContext *ctx, JSValueConst value, int magic) {
  LeafKind const kind = static_cast<LeafKind>(magic);
  std::uint32_t const length = kind == LeafKind::hash128   ? 16
                               : kind == LeafKind::hash160 ? 20
                                                           : 24;
  void *state =
      length == 16   ? static_cast<void *>(fixedState<16>(ctx, value, kind))
      : length == 20 ? static_cast<void *>(fixedState<20>(ctx, value, kind))
                     : static_cast<void *>(fixedState<24>(ctx, value, kind));
  return state == nullptr ? JS_EXCEPTION : JS_NewUint32(ctx, length);
}

JSValue hashToBytesMagic(JSContext *ctx, JSValueConst value, int,
                         JSValueConst *, int magic) {
  LeafKind const kind = static_cast<LeafKind>(magic);
  return fixedBytes(ctx, value, kind,
                    kind == LeafKind::hash128   ? 16
                    : kind == LeafKind::hash160 ? 20
                                                : 24);
}

JSValue hashToHexMagic(JSContext *ctx, JSValueConst value, int, JSValueConst *,
                       int magic) {
  LeafKind const kind = static_cast<LeafKind>(magic);
  if (kind == LeafKind::hash128)
    return fixedHex<16>(ctx, value, kind);
  if (kind == LeafKind::hash160)
    return fixedHex<20>(ctx, value, kind);
  return fixedHex<24>(ctx, value, kind);
}

JSValue hashIsZeroMagic(JSContext *ctx, JSValueConst value, int, JSValueConst *,
                        int magic) {
  LeafKind const kind = static_cast<LeafKind>(magic);
  if (kind == LeafKind::hash128)
    return fixedIsZero<16>(ctx, value, kind);
  if (kind == LeafKind::hash160)
    return fixedIsZero<20>(ctx, value, kind);
  return fixedIsZero<24>(ctx, value, kind);
}

JSValue hashEqualsMagic(JSContext *ctx, JSValueConst value, int argc,
                        JSValueConst *argv, int magic) {
  LeafKind const kind = static_cast<LeafKind>(magic);
  JSValueConst other = argc == 0 ? JS_UNDEFINED : argv[0];
  if (kind == LeafKind::hash128)
    return fixedEquals<16>(ctx, value, other, kind);
  if (kind == LeafKind::hash160)
    return fixedEquals<20>(ctx, value, other, kind);
  return fixedEquals<24>(ctx, value, other, kind);
}

JSValue hashCompareMagic(JSContext *ctx, JSValueConst value, int argc,
                         JSValueConst *argv, int magic) {
  LeafKind const kind = static_cast<LeafKind>(magic);
  JSValueConst other = argc == 0 ? JS_UNDEFINED : argv[0];
  if (kind == LeafKind::hash128)
    return fixedCompare<16>(ctx, value, other, kind);
  if (kind == LeafKind::hash160)
    return fixedCompare<20>(ctx, value, other, kind);
  return fixedCompare<24>(ctx, value, other, kind);
}

JSValue hash128ToBytes(JSContext *ctx, JSValueConst value, int argc,
                       JSValueConst *argv) {
  return hashToBytesMagic(ctx, value, argc, argv,
                          static_cast<int>(LeafKind::hash128));
}

JSValue hash160ToBytes(JSContext *ctx, JSValueConst value, int argc,
                       JSValueConst *argv) {
  return hashToBytesMagic(ctx, value, argc, argv,
                          static_cast<int>(LeafKind::hash160));
}

JSValue hash192ToBytes(JSContext *ctx, JSValueConst value, int argc,
                       JSValueConst *argv) {
  return hashToBytesMagic(ctx, value, argc, argv,
                          static_cast<int>(LeafKind::hash192));
}

[[nodiscard]] bool currencyIsNative(FixedState<20> const &state) noexcept {
  return allZero(state.data, 20);
}

[[nodiscard]] bool currencyIsText(std::uint8_t const *bytes) noexcept {
  for (std::uint32_t i = 0; i < 12; ++i) {
    if (bytes[i] != 0)
      return false;
  }
  for (std::uint32_t i = 15; i < 20; ++i) {
    if (bytes[i] != 0)
      return false;
  }
  constexpr char allowed[] =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
      "<>(){}[]|?!@#$%^&*";
  for (std::uint32_t i = 12; i < 15; ++i) {
    bool found = false;
    for (char const *cursor = allowed; *cursor != '\0'; ++cursor) {
      if (static_cast<std::uint8_t>(*cursor) == bytes[i]) {
        found = true;
        break;
      }
    }
    if (!found)
      return false;
  }
  return !(bytes[12] == 'X' && bytes[13] == 'A' && bytes[14] == 'H');
}

JSValue currencyByteLength(JSContext *ctx, JSValueConst value) {
  auto *state = fixedState<20>(ctx, value, LeafKind::currency);
  return state == nullptr ? JS_EXCEPTION : JS_NewUint32(ctx, 20);
}

JSValue currencyNative(JSContext *ctx, JSValueConst value) {
  auto *state = fixedState<20>(ctx, value, LeafKind::currency);
  return state == nullptr ? JS_EXCEPTION
                          : JS_NewBool(ctx, currencyIsNative(*state));
}

JSValue currencyToBytes(JSContext *ctx, JSValueConst value, int,
                        JSValueConst *) {
  return fixedBytes(ctx, value, LeafKind::currency, 20);
}

JSValue currencyToHex(JSContext *ctx, JSValueConst value, int, JSValueConst *) {
  return fixedHex<20>(ctx, value, LeafKind::currency);
}

JSValue currencyToString(JSContext *ctx, JSValueConst value, int,
                         JSValueConst *) {
  auto *state = fixedState<20>(ctx, value, LeafKind::currency);
  if (state == nullptr)
    return JS_EXCEPTION;
  if (currencyIsNative(*state))
    return JS_NewStringLen(ctx, "XAH", 3);
  if (isNoAccount(state->data))
    return JS_NewStringLen(ctx, "1", 1);
  if (currencyIsText(state->data))
    return JS_NewStringLen(ctx,
                           reinterpret_cast<char const *>(state->data + 12), 3);
  return fixedHex<20>(ctx, value, LeafKind::currency);
}

JSValue currencyEquals(JSContext *ctx, JSValueConst value, int argc,
                       JSValueConst *argv) {
  return fixedEquals<20>(ctx, value, argc == 0 ? JS_UNDEFINED : argv[0],
                         LeafKind::currency);
}

enum class IssueVariant : std::uint8_t { native, iou, mpt };

[[nodiscard]] IssueVariant issueVariant(IssueState const &state) noexcept {
  return state.length == 20   ? IssueVariant::native
         : state.length == 44 ? IssueVariant::mpt
                              : IssueVariant::iou;
}

JSValue issueKind(JSContext *ctx, JSValueConst value) {
  auto *state = exactState<IssueState>(ctx, value, LeafKind::issue);
  if (state == nullptr)
    return JS_EXCEPTION;
  switch (issueVariant(*state)) {
  case IssueVariant::native:
    return JS_NewStringLen(ctx, "native", 6);
  case IssueVariant::iou:
    return JS_NewStringLen(ctx, "iou", 3);
  case IssueVariant::mpt:
    return JS_NewStringLen(ctx, "mpt", 3);
  }
  return JS_EXCEPTION;
}

[[nodiscard]] JSValue publishIssueChild(JSContext *ctx, IssueState &state,
                                        std::uint32_t slot, JSValue local) {
  if (JS_IsException(local))
    return local;
  if (JS_IsObject(local) && JS_PreventExtensions(ctx, local) < 0) {
    JS_FreeValue(ctx, local);
    return JS_EXCEPTION;
  }
  state.cache[slot] = local;
  return JS_DupValue(ctx, local);
}

JSValue issueCurrency(JSContext *ctx, JSValueConst value) {
  auto *state = exactState<IssueState>(ctx, value, LeafKind::issue);
  if (state == nullptr)
    return JS_EXCEPTION;
  if (issueVariant(*state) == IssueVariant::mpt)
    return JS_UNDEFINED;
  if (!JS_IsUndefined(state->cache[0]))
    return JS_DupValue(ctx, state->cache[0]);
  return publishIssueChild(
      ctx, *state, 0,
      JS_IsUndefined(state->owner)
          ? makeCurrencyBytes(ctx, state->data, 20)
          : makeCurrencyView(ctx, state->owner, state->data, 20));
}

JSValue issueIssuer(JSContext *ctx, JSValueConst value) {
  auto *state = exactState<IssueState>(ctx, value, LeafKind::issue);
  if (state == nullptr)
    return JS_EXCEPTION;
  if (issueVariant(*state) != IssueVariant::iou)
    return JS_UNDEFINED;
  if (!JS_IsUndefined(state->cache[1]))
    return JS_DupValue(ctx, state->cache[1]);
  return publishIssueChild(
      ctx, *state, 1,
      JS_IsUndefined(state->owner)
          ? makeAccountIDBytes(ctx, state->data + 20, 20)
          : makeAccountIDView(ctx, state->owner, state->data + 20, 20));
}

JSValue issueMptId(JSContext *ctx, JSValueConst value) {
  auto *state = exactState<IssueState>(ctx, value, LeafKind::issue);
  if (state == nullptr)
    return JS_EXCEPTION;
  if (issueVariant(*state) != IssueVariant::mpt)
    return JS_UNDEFINED;
  if (!JS_IsUndefined(state->cache[2]))
    return JS_DupValue(ctx, state->cache[2]);
  // Pinned xahaud stores the MPTID sequence in native uint32 byte order,
  // while the standalone Issue wire writes it big-endian.
  std::uint8_t mptId[24];
  mptId[0] = state->data[43];
  mptId[1] = state->data[42];
  mptId[2] = state->data[41];
  mptId[3] = state->data[40];
  std::memcpy(mptId + 4, state->data, 20);
  return publishIssueChild(
      ctx, *state, 2,
      JS_IsUndefined(state->owner)
          ? makeHash192Bytes(ctx, mptId, sizeof(mptId))
          : makeHash192DerivedBytes(ctx, state->owner, mptId, sizeof(mptId)));
}

JSValue issueToBytes(JSContext *ctx, JSValueConst value, int, JSValueConst *) {
  auto *state = exactState<IssueState>(ctx, value, LeafKind::issue);
  return state == nullptr ? JS_EXCEPTION
                          : qjs::uint8Array(ctx, {state->data, state->length});
}

JSValue issueEquals(JSContext *ctx, JSValueConst value, int argc,
                    JSValueConst *argv) {
  auto *left = exactState<IssueState>(ctx, value, LeafKind::issue);
  if (left == nullptr)
    return JS_EXCEPTION;
  auto *right = exactState<IssueState>(ctx, argc == 0 ? JS_UNDEFINED : argv[0],
                                       LeafKind::issue);
  if (right == nullptr)
    return JS_EXCEPTION;
  return JS_NewBool(
      ctx, left->length == right->length &&
               std::memcmp(left->data, right->data, left->length) == 0);
}

[[nodiscard]] JSValue vectorValue(JSContext *ctx, VectorState &state,
                                  std::uint32_t index) {
  if (index >= state.count)
    return JS_UNDEFINED;
  std::uint32_t const rootIndex = (index >> 10) & 31;
  std::uint32_t const branchIndex = (index >> 5) & 31;
  std::uint32_t const slotIndex = index & 31;
  if (state.cache != nullptr) {
    auto *branch = state.cache->branches[rootIndex];
    auto *leaf = branch == nullptr ? nullptr : branch->pages[branchIndex];
    if (leaf != nullptr && !JS_IsUndefined(leaf->values[slotIndex]))
      return JS_DupValue(ctx, leaf->values[slotIndex]);
  }

  qjs::OwnedValue local(
      ctx, JS_IsUndefined(state.owner)
               ? makeHash256Bytes(ctx, vectorBytes(&state) + index * 32, 32)
               : makeHash256View(ctx, state.owner,
                                 vectorBytes(&state) + index * 32, 32));
  if (local.isException())
    return local.release();
  if (JS_PreventExtensions(ctx, local.get()) < 0)
    return JS_EXCEPTION;

  VectorCacheRoot *newRoot = nullptr;
  VectorCacheBranch *newBranch = nullptr;
  VectorCacheLeaf *newLeaf = nullptr;
  auto *root = state.cache;
  auto *branch = root == nullptr ? nullptr : root->branches[rootIndex];
  auto *leaf = branch == nullptr ? nullptr : branch->pages[branchIndex];
  if (root == nullptr) {
    newRoot = static_cast<VectorCacheRoot *>(
        js_mallocz(ctx, sizeof(VectorCacheRoot)));
    if (newRoot == nullptr)
      return oom(ctx);
    root = newRoot;
  }
  if (branch == nullptr) {
    newBranch = static_cast<VectorCacheBranch *>(
        js_mallocz(ctx, sizeof(VectorCacheBranch)));
    if (newBranch == nullptr) {
      js_free(ctx, newRoot);
      return oom(ctx);
    }
    branch = newBranch;
  }
  if (leaf == nullptr) {
    newLeaf =
        static_cast<VectorCacheLeaf *>(js_malloc(ctx, sizeof(VectorCacheLeaf)));
    if (newLeaf == nullptr) {
      js_free(ctx, newBranch);
      js_free(ctx, newRoot);
      return oom(ctx);
    }
    for (JSValue &cached : newLeaf->values)
      cached = JS_UNDEFINED;
    leaf = newLeaf;
  }

  // No allocation or callback is reachable after the first publication.
  if (newRoot != nullptr)
    state.cache = newRoot;
  if (newBranch != nullptr) {
    root->branches[rootIndex] = newBranch;
    ++root->branchCount;
  }
  if (newLeaf != nullptr) {
    branch->pages[branchIndex] = newLeaf;
    ++root->pageCount;
  }
  leaf->values[slotIndex] = local.release();
  ++root->valueCount;
  return JS_DupValue(ctx, leaf->values[slotIndex]);
}

[[nodiscard]] bool readAtIndex(JSContext *ctx, int argc, JSValueConst *argv,
                               std::uint32_t length, std::uint32_t &index) {
  if (argc == 0)
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

JSValue vectorAt(JSContext *ctx, JSValueConst value, int argc,
                 JSValueConst *argv) {
  auto *state = exactState<VectorState>(ctx, value, LeafKind::vector256);
  if (state == nullptr)
    return JS_EXCEPTION;
  std::uint32_t index = 0;
  if (!readAtIndex(ctx, argc, argv, state->count, index))
    return JS_HasException(ctx) ? JS_EXCEPTION : JS_UNDEFINED;
  return vectorValue(ctx, *state, index);
}

JSValue vectorToBytes(JSContext *ctx, JSValueConst value, int, JSValueConst *) {
  auto *state = exactState<VectorState>(ctx, value, LeafKind::vector256);
  return state == nullptr
             ? JS_EXCEPTION
             : qjs::uint8Array(ctx, {vectorBytes(state), state->byteLength});
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

JSValue vectorIteratorSelf(JSContext *ctx, JSValueConst value, int,
                           JSValueConst *) {
  auto *state = static_cast<VectorIteratorState *>(
      JS_GetOpaque2(ctx, value, vectorIteratorClassId));
  return state == nullptr ? JS_EXCEPTION : JS_DupValue(ctx, value);
}

JSValue vectorIteratorNext(JSContext *ctx, JSValueConst value, int,
                           JSValueConst *) {
  auto *iterator = static_cast<VectorIteratorState *>(
      JS_GetOpaque2(ctx, value, vectorIteratorClassId));
  if (iterator == nullptr)
    return JS_EXCEPTION;
  auto *state =
      exactState<VectorState>(ctx, iterator->vector, LeafKind::vector256);
  if (state == nullptr)
    return JS_EXCEPTION;
  if (iterator->cursor >= state->count)
    return newIteratorResult(ctx, JS_UNDEFINED, true);
  qjs::OwnedValue element(ctx, vectorValue(ctx, *state, iterator->cursor));
  if (element.isException())
    return element.release();
  JSValue result = newIteratorResult(ctx, element.release(), false);
  if (!JS_IsException(result))
    ++iterator->cursor;
  return result;
}

JSValue vectorIterator(JSContext *ctx, JSValueConst value, int,
                       JSValueConst *) {
  if (exactState<VectorState>(ctx, value, LeafKind::vector256) == nullptr)
    return JS_EXCEPTION;
  JSValue iterator = JS_NewObjectClass(ctx, vectorIteratorClassId);
  if (JS_IsException(iterator))
    return iterator;
  auto *state = static_cast<VectorIteratorState *>(
      js_malloc(ctx, sizeof(VectorIteratorState)));
  if (state == nullptr) {
    JS_FreeValue(ctx, iterator);
    return oom(ctx);
  }
  state->vector = JS_DupValue(ctx, value);
  state->cursor = 0;
  JS_SetOpaque(iterator, state);
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
  if (test::gcProbeSelected(test::HiddenEdge::vector256Iterator)) {
    test::gcProbeCreated(test::TrackedEntity::vectorIterator, iterator);
    auto const *vector = static_cast<VectorState const *>(
        JS_GetOpaque(value, classId(LeafKind::vector256)));
    if (vector == nullptr || JS_IsUndefined(vector->owner) ||
        !test::gcProbePlantCycle(ctx, test::HiddenEdge::vector256Iterator,
                                 vector->owner, iterator, iterator)) {
      JS_FreeValue(ctx, iterator);
      return JS_EXCEPTION;
    }
  }
#endif
  if (JS_PreventExtensions(ctx, iterator) < 0) {
    JS_FreeValue(ctx, iterator);
    return JS_EXCEPTION;
  }
  return iterator;
}

[[nodiscard]] bool isLength(JSContext *ctx, JSAtom candidate, bool &result) {
  JSAtom const atom = JS_NewAtom(ctx, "length");
  if (atom == JS_ATOM_NULL) {
    if (!JS_HasException(ctx))
      JS_ThrowOutOfMemory(ctx);
    return false;
  }
  result = candidate == atom;
  JS_FreeAtom(ctx, atom);
  return true;
}

[[nodiscard]] bool vectorPropertyIndex(JSAtom atom, std::uint32_t length,
                                       std::uint32_t &index) noexcept {
  if (!JS_AtomIsTaggedInt(atom))
    return false;
  index = JS_AtomToTaggedInt(atom);
  return index < length;
}

int vectorOwnProperty(JSContext *ctx, JSPropertyDescriptor *descriptor,
                      JSValueConst value, JSAtom atom) {
  if (descriptor != nullptr) {
    descriptor->flags = 0;
    descriptor->value = JS_UNDEFINED;
    descriptor->getter = JS_UNDEFINED;
    descriptor->setter = JS_UNDEFINED;
  }
  auto *state = exactState<VectorState>(ctx, value, LeafKind::vector256);
  if (state == nullptr)
    return -1;
  bool lengthProperty = false;
  if (!isLength(ctx, atom, lengthProperty))
    return -1;
  // @binding provider:Vector256.length
  if (lengthProperty) {
    if (descriptor != nullptr)
      descriptor->value = JS_NewUint32(ctx, state->count);
    return 1;
  }
  std::uint32_t index = 0;
  if (!vectorPropertyIndex(atom, state->count, index))
    return 0;
  if (descriptor != nullptr) {
    descriptor->value = vectorValue(ctx, *state, index);
    if (JS_IsException(descriptor->value)) {
      descriptor->value = JS_UNDEFINED;
      return -1;
    }
    descriptor->flags = JS_PROP_ENUMERABLE;
  }
  return 1;
}

int vectorOwnNames(JSContext *ctx, JSPropertyEnum **table,
                   std::uint32_t *length, JSValueConst value) {
  *table = nullptr;
  *length = 0;
  auto *state = exactState<VectorState>(ctx, value, LeafKind::vector256);
  if (state == nullptr)
    return -1;
  if (state->count == std::numeric_limits<std::uint32_t>::max())
    return -1;
  std::uint32_t const count = state->count + 1;
  auto *names = static_cast<JSPropertyEnum *>(
      js_malloc(ctx, static_cast<std::size_t>(count) * sizeof(JSPropertyEnum)));
  if (names == nullptr)
    return -1;
  for (std::uint32_t i = 0; i < state->count; ++i)
    names[i] = {true, JS_NewAtomUInt32(ctx, i)};
  JSAtom const lengthKey = JS_NewAtom(ctx, "length");
  if (lengthKey == JS_ATOM_NULL) {
    if (!JS_HasException(ctx))
      JS_ThrowOutOfMemory(ctx);
    for (std::uint32_t i = 0; i < state->count; ++i)
      JS_FreeAtom(ctx, names[i].atom);
    js_free(ctx, names);
    return -1;
  }
  names[count - 1] = {false, lengthKey};
  *table = names;
  *length = count;
  return 0;
}

int vectorDelete(JSContext *ctx, JSValueConst value, JSAtom atom) {
  auto *state = exactState<VectorState>(ctx, value, LeafKind::vector256);
  if (state == nullptr)
    return -1;
  bool lengthProperty = false;
  if (!isLength(ctx, atom, lengthProperty))
    return -1;
  if (lengthProperty)
    return 0;
  std::uint32_t index = 0;
  return !vectorPropertyIndex(atom, state->count, index);
}

[[nodiscard]] JSValue publishBridgeChild(JSContext *ctx, BridgeState &state,
                                         std::uint32_t slot, JSValue local) {
  if (JS_IsException(local))
    return local;
  if (JS_IsObject(local) && JS_PreventExtensions(ctx, local) < 0) {
    JS_FreeValue(ctx, local);
    return JS_EXCEPTION;
  }
  state.cache[slot] = local;
  return JS_DupValue(ctx, local);
}

[[nodiscard]] JSValue bridgePartValue(JSContext *ctx, BridgeState &state,
                                      std::uint32_t slot) {
  if (slot >= 4)
    return internalError(ctx, "invalid XChainBridge part");
  if (!JS_IsUndefined(state.cache[slot]))
    return JS_DupValue(ctx, state.cache[slot]);
  BridgePart const part = state.parts[slot];
  if (part.offset > state.length || part.length > state.length - part.offset)
    return internalError(ctx, "invalid certified XChainBridge metadata");
  if ((slot & 1U) != 0)
    return publishBridgeChild(
        ctx, state, slot,
        JS_IsUndefined(state.owner)
            ? makeIssueBytes(ctx, state.data + part.offset, part.length)
            : makeIssueView(ctx, state.owner, state.data + part.offset,
                            part.length));
  std::uint8_t zero[20]{};
  return publishBridgeChild(
      ctx, state, slot,
      JS_IsUndefined(state.owner)
          ? makeAccountIDBytes(
                ctx, part.length == 0 ? zero : state.data + part.offset, 20)
          : makeAccountIDView(ctx, state.owner, state.data + part.offset,
                              part.length));
}

JSValue bridgeGetter(JSContext *ctx, JSValueConst value, int magic) {
  auto *state = exactState<BridgeState>(ctx, value, LeafKind::xchainBridge);
  return state == nullptr
             ? JS_EXCEPTION
             : bridgePartValue(ctx, *state, static_cast<std::uint32_t>(magic));
}

JSValue bridgeToBytes(JSContext *ctx, JSValueConst value, int, JSValueConst *) {
  auto *state = exactState<BridgeState>(ctx, value, LeafKind::xchainBridge);
  return state == nullptr ? JS_EXCEPTION
                          : qjs::uint8Array(ctx, {state->data, state->length});
}

JSValue bridgeEquals(JSContext *ctx, JSValueConst value, int argc,
                     JSValueConst *argv) {
  auto *left = exactState<BridgeState>(ctx, value, LeafKind::xchainBridge);
  if (left == nullptr)
    return JS_EXCEPTION;
  auto *right = exactState<BridgeState>(ctx, argc == 0 ? JS_UNDEFINED : argv[0],
                                        LeafKind::xchainBridge);
  if (right == nullptr)
    return JS_EXCEPTION;
  return JS_NewBool(
      ctx, left->length == right->length &&
               std::memcmp(left->data, right->data, left->length) == 0);
}

JSCFunctionListEntry const hash128Prototype[] = {
    // @binding provider:Hash128.byteLength
    JS_CGETSET_MAGIC_DEF("byteLength", hashByteLength, nullptr,
                         static_cast<int>(LeafKind::hash128)),
    // @binding provider:Hash128.toBytes
    JS_CFUNC_MAGIC_DEF("toBytes", 0, hashToBytesMagic,
                       static_cast<int>(LeafKind::hash128)),
    // @binding provider:Hash128.toHex
    JS_CFUNC_MAGIC_DEF("toHex", 0, hashToHexMagic,
                       static_cast<int>(LeafKind::hash128)),
    // @binding provider:Hash128.isZero
    JS_CFUNC_MAGIC_DEF("isZero", 0, hashIsZeroMagic,
                       static_cast<int>(LeafKind::hash128)),
    // @binding provider:Hash128.equals
    JS_CFUNC_MAGIC_DEF("equals", 1, hashEqualsMagic,
                       static_cast<int>(LeafKind::hash128)),
    // @binding provider:Hash128.compare
    JS_CFUNC_MAGIC_DEF("compare", 1, hashCompareMagic,
                       static_cast<int>(LeafKind::hash128)),
};

JSCFunctionListEntry const hash160Prototype[] = {
    // @binding provider:Hash160.byteLength
    JS_CGETSET_MAGIC_DEF("byteLength", hashByteLength, nullptr,
                         static_cast<int>(LeafKind::hash160)),
    // @binding provider:Hash160.toBytes
    JS_CFUNC_MAGIC_DEF("toBytes", 0, hashToBytesMagic,
                       static_cast<int>(LeafKind::hash160)),
    // @binding provider:Hash160.toHex
    JS_CFUNC_MAGIC_DEF("toHex", 0, hashToHexMagic,
                       static_cast<int>(LeafKind::hash160)),
    // @binding provider:Hash160.isZero
    JS_CFUNC_MAGIC_DEF("isZero", 0, hashIsZeroMagic,
                       static_cast<int>(LeafKind::hash160)),
    // @binding provider:Hash160.equals
    JS_CFUNC_MAGIC_DEF("equals", 1, hashEqualsMagic,
                       static_cast<int>(LeafKind::hash160)),
    // @binding provider:Hash160.compare
    JS_CFUNC_MAGIC_DEF("compare", 1, hashCompareMagic,
                       static_cast<int>(LeafKind::hash160)),
};

JSCFunctionListEntry const hash192Prototype[] = {
    // @binding provider:Hash192.byteLength
    JS_CGETSET_MAGIC_DEF("byteLength", hashByteLength, nullptr,
                         static_cast<int>(LeafKind::hash192)),
    // @binding provider:Hash192.toBytes
    JS_CFUNC_MAGIC_DEF("toBytes", 0, hashToBytesMagic,
                       static_cast<int>(LeafKind::hash192)),
    // @binding provider:Hash192.toHex
    JS_CFUNC_MAGIC_DEF("toHex", 0, hashToHexMagic,
                       static_cast<int>(LeafKind::hash192)),
    // @binding provider:Hash192.isZero
    JS_CFUNC_MAGIC_DEF("isZero", 0, hashIsZeroMagic,
                       static_cast<int>(LeafKind::hash192)),
    // @binding provider:Hash192.equals
    JS_CFUNC_MAGIC_DEF("equals", 1, hashEqualsMagic,
                       static_cast<int>(LeafKind::hash192)),
    // @binding provider:Hash192.compare
    JS_CFUNC_MAGIC_DEF("compare", 1, hashCompareMagic,
                       static_cast<int>(LeafKind::hash192)),
};

JSCFunctionListEntry const currencyPrototype[] = {
    // @binding provider:Currency.byteLength
    JS_CGETSET_DEF("byteLength", currencyByteLength, nullptr),
    // @binding provider:Currency.isNative
    JS_CGETSET_DEF("isNative", currencyNative, nullptr),
    // @binding provider:Currency.toBytes
    JS_CFUNC_DEF("toBytes", 0, currencyToBytes),
    // @binding provider:Currency.toHex
    JS_CFUNC_DEF("toHex", 0, currencyToHex),
    // @binding provider:Currency.toString
    JS_CFUNC_DEF("toString", 0, currencyToString),
    // @binding provider:Currency.equals
    JS_CFUNC_DEF("equals", 1, currencyEquals),
};

JSCFunctionListEntry const issuePrototype[] = {
    // @binding provider:Issue.kind
    JS_CGETSET_DEF("kind", issueKind, nullptr),
    // @binding provider:Issue.currency
    JS_CGETSET_DEF("currency", issueCurrency, nullptr),
    // @binding provider:Issue.issuer
    JS_CGETSET_DEF("issuer", issueIssuer, nullptr),
    // @binding provider:Issue.mptIssuanceId
    JS_CGETSET_DEF("mptIssuanceId", issueMptId, nullptr),
    // @binding provider:Issue.toBytes
    JS_CFUNC_DEF("toBytes", 0, issueToBytes),
    // @binding provider:Issue.equals
    JS_CFUNC_DEF("equals", 1, issueEquals),
};

JSCFunctionListEntry const vectorPrototype[] = {
    // @binding provider:Vector256.at
    JS_CFUNC_DEF("at", 1, vectorAt),
    // @binding provider:Vector256.toBytes
    JS_CFUNC_DEF("toBytes", 0, vectorToBytes),
    // @binding provider:Vector256.[Symbol.iterator]
    JS_CFUNC_DEF("[Symbol.iterator]", 0, vectorIterator),
};

JSCFunctionListEntry const vectorIteratorPrototype[] = {
    JS_CFUNC_DEF("next", 0, vectorIteratorNext),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, vectorIteratorSelf),
};

JSCFunctionListEntry const bridgePrototype[] = {
    // @binding provider:XChainBridge.LockingChainDoor
    JS_CGETSET_MAGIC_DEF("LockingChainDoor", bridgeGetter, nullptr, 0),
    // @binding provider:XChainBridge.LockingChainIssue
    JS_CGETSET_MAGIC_DEF("LockingChainIssue", bridgeGetter, nullptr, 1),
    // @binding provider:XChainBridge.IssuingChainDoor
    JS_CGETSET_MAGIC_DEF("IssuingChainDoor", bridgeGetter, nullptr, 2),
    // @binding provider:XChainBridge.IssuingChainIssue
    JS_CGETSET_MAGIC_DEF("IssuingChainIssue", bridgeGetter, nullptr, 3),
    // @binding provider:XChainBridge.toBytes
    JS_CFUNC_DEF("toBytes", 0, bridgeToBytes),
    // @binding provider:XChainBridge.equals
    JS_CFUNC_DEF("equals", 1, bridgeEquals),
};

[[nodiscard]] bool installPrototypes(JSContext *ctx) {
  return coreqjs::installPrototype(ctx, classId(LeafKind::hash128),
                                   hash128Prototype) &&
         coreqjs::installPrototype(ctx, classId(LeafKind::hash160),
                                   hash160Prototype) &&
         coreqjs::installPrototype(ctx, classId(LeafKind::hash192),
                                   hash192Prototype) &&
         coreqjs::installPrototype(ctx, classId(LeafKind::currency),
                                   currencyPrototype) &&
         coreqjs::installPrototype(ctx, classId(LeafKind::issue),
                                   issuePrototype) &&
         coreqjs::installPrototype(ctx, classId(LeafKind::vector256),
                                   vectorPrototype) &&
         coreqjs::installPrototype(ctx, vectorIteratorClassId,
                                   vectorIteratorPrototype) &&
         coreqjs::installPrototype(ctx, classId(LeafKind::xchainBridge),
                                   bridgePrototype);
}

[[nodiscard]] bool registerByteFamilies() noexcept {
  return qjs::registerByteClass(classId(LeafKind::hash128),
                                qjs::ByteClassFamily::serializedType,
                                hash128ToBytes) &&
         qjs::registerByteClass(classId(LeafKind::hash160),
                                qjs::ByteClassFamily::serializedType,
                                hash160ToBytes) &&
         qjs::registerByteClass(classId(LeafKind::hash192),
                                qjs::ByteClassFamily::serializedType,
                                hash192ToBytes) &&
         qjs::registerByteClass(classId(LeafKind::currency),
                                qjs::ByteClassFamily::serializedType,
                                currencyToBytes) &&
         qjs::registerByteClass(classId(LeafKind::issue),
                                qjs::ByteClassFamily::serializedType,
                                issueToBytes) &&
         qjs::registerByteClass(classId(LeafKind::vector256),
                                qjs::ByteClassFamily::serializedType,
                                vectorToBytes) &&
         qjs::registerByteClass(classId(LeafKind::xchainBridge),
                                qjs::ByteClassFamily::serializedType,
                                bridgeToBytes);
}

} // namespace

bool registerRichLeafTypes(JSContext *ctx) {
  if (ctx == nullptr)
    return false;
  JSRuntime *runtime = JS_GetRuntime(ctx);
  if (registeredRuntime != nullptr)
    return registeredRuntime == runtime;

  vectorExotic.get_own_property = vectorOwnProperty;
  vectorExotic.get_own_property_names = vectorOwnNames;
  vectorExotic.delete_property = vectorDelete;

  for (std::size_t i = 0; i < kindCount; ++i) {
    if (!coreqjs::defineClass(runtime, &classIds[i], &classDefs[i]))
      return false;
  }
  if (!coreqjs::defineClass(runtime, &vectorIteratorClassId,
                            &vectorIteratorClass) ||
      !installPrototypes(ctx)) {
    return false;
  }
  registeredRuntime = runtime;
  if (!registerByteFamilies()) {
    registeredRuntime = nullptr;
    return false;
  }
  return !JS_HasException(ctx);
}

void unregisterRichLeafTypes(JSRuntime *runtime) noexcept {
  if (runtime != nullptr && runtime == registeredRuntime)
    registeredRuntime = nullptr;
}

JSValue makeHash128Bytes(JSContext *ctx, std::uint8_t const *bytes,
                         std::uint32_t length) {
  return newFixed<16>(ctx, LeafKind::hash128, JS_UNDEFINED, bytes, length,
                      "Hash128 construction requires 16 certified bytes");
}

JSValue makeHash128View(JSContext *ctx, JSValueConst owner,
                        std::uint8_t const *bytes, std::uint32_t length) {
  return newFixed<16>(ctx, LeafKind::hash128, owner, bytes, length,
                      "Hash128 construction requires 16 certified bytes");
}

JSValue makeHash160Bytes(JSContext *ctx, std::uint8_t const *bytes,
                         std::uint32_t length) {
  return newFixed<20>(ctx, LeafKind::hash160, JS_UNDEFINED, bytes, length,
                      "Hash160 construction requires 20 certified bytes");
}

JSValue makeHash160View(JSContext *ctx, JSValueConst owner,
                        std::uint8_t const *bytes, std::uint32_t length) {
  return newFixed<20>(ctx, LeafKind::hash160, owner, bytes, length,
                      "Hash160 construction requires 20 certified bytes");
}

JSValue makeHash192Bytes(JSContext *ctx, std::uint8_t const *bytes,
                         std::uint32_t length) {
  return newFixed<24>(ctx, LeafKind::hash192, JS_UNDEFINED, bytes, length,
                      "Hash192 construction requires 24 certified bytes");
}

JSValue makeHash192View(JSContext *ctx, JSValueConst owner,
                        std::uint8_t const *bytes, std::uint32_t length) {
  return newFixed<24>(ctx, LeafKind::hash192, owner, bytes, length,
                      "Hash192 construction requires 24 certified bytes");
}

JSValue makeHash192DerivedBytes(JSContext *ctx, JSValueConst owner,
                                std::uint8_t const *bytes,
                                std::uint32_t length) {
  return newFixedDerived<24>(
      ctx, LeafKind::hash192, owner, bytes, length,
      "Hash192 derived construction requires a certified owner");
}

bool readHash192Bytes(JSContext *ctx, JSValueConst value,
                      std::uint8_t *output) noexcept {
  if (output == nullptr || !runtimeReady(ctx) || !JS_IsObject(value) ||
      JS_GetClassID(value) != classId(LeafKind::hash192))
    return false;
  auto const *state = static_cast<FixedState<24> const *>(
      JS_GetOpaque(value, classId(LeafKind::hash192)));
  if (state == nullptr)
    return false;
  std::memcpy(output, state->data, 24);
  return true;
}

JSValue makeCurrencyBytes(JSContext *ctx, std::uint8_t const *bytes,
                          std::uint32_t length) {
  return newFixed<20>(ctx, LeafKind::currency, JS_UNDEFINED, bytes, length,
                      "Currency construction requires 20 certified bytes");
}

JSValue makeCurrencyView(JSContext *ctx, JSValueConst owner,
                         std::uint8_t const *bytes, std::uint32_t length) {
  return newFixed<20>(ctx, LeafKind::currency, owner, bytes, length,
                      "Currency construction requires 20 certified bytes");
}

bool readCurrencyBytes(JSContext *ctx, JSValueConst value,
                       std::uint8_t *output) noexcept {
  if (output == nullptr || !runtimeReady(ctx) || !JS_IsObject(value) ||
      JS_GetClassID(value) != classId(LeafKind::currency))
    return false;
  auto const *state = static_cast<FixedState<20> const *>(
      JS_GetOpaque(value, classId(LeafKind::currency)));
  if (state == nullptr)
    return false;
  std::memcpy(output, state->data, 20);
  return true;
}

bool detail::readRichLeafNominalPayload(
    JSContext *ctx, JSValueConst input,
    catl::xdata::MaterializerKind expected,
    NominalPayloadView &output) noexcept {
  output = {};
  if (ctx == nullptr || !runtimeReady(ctx) || !JS_IsObject(input))
    return false;

  LeafKind kind = LeafKind::count;
  std::uint32_t fixedLength = 0;
  switch (expected) {
  case catl::xdata::MaterializerKind::hash128:
    kind = LeafKind::hash128;
    fixedLength = 16;
    break;
  case catl::xdata::MaterializerKind::hash160:
    kind = LeafKind::hash160;
    fixedLength = 20;
    break;
  case catl::xdata::MaterializerKind::hash192:
    kind = LeafKind::hash192;
    fixedLength = 24;
    break;
  case catl::xdata::MaterializerKind::currency:
    kind = LeafKind::currency;
    fixedLength = 20;
    break;
  case catl::xdata::MaterializerKind::issue:
    kind = LeafKind::issue;
    break;
  case catl::xdata::MaterializerKind::vector256:
    kind = LeafKind::vector256;
    break;
  case catl::xdata::MaterializerKind::xchain_bridge:
    kind = LeafKind::xchainBridge;
    break;
  default:
    return false;
  }
  if (JS_GetClassID(input) != classId(kind))
    return false;

  if (fixedLength != 0) {
    void const *bytes = nullptr;
    if (fixedLength == 16) {
      auto const *state = static_cast<FixedState<16> const *>(
          JS_GetOpaque(input, classId(kind)));
      bytes = state == nullptr ? nullptr : state->data;
    } else if (fixedLength == 20) {
      auto const *state = static_cast<FixedState<20> const *>(
          JS_GetOpaque(input, classId(kind)));
      bytes = state == nullptr ? nullptr : state->data;
    } else {
      auto const *state = static_cast<FixedState<24> const *>(
          JS_GetOpaque(input, classId(kind)));
      bytes = state == nullptr ? nullptr : state->data;
    }
    if (bytes == nullptr)
      return false;
    output = {static_cast<std::uint8_t const *>(bytes), fixedLength};
    return true;
  }

  if (kind == LeafKind::issue) {
    auto const *state = static_cast<IssueState const *>(
        JS_GetOpaque(input, classId(kind)));
    if (state == nullptr ||
        (state->length != 20 && state->length != 40 && state->length != 44))
      return false;
    output = {state->data, state->length};
    return true;
  }
  if (kind == LeafKind::vector256) {
    auto const *state = static_cast<VectorState const *>(
        JS_GetOpaque(input, classId(kind)));
    if (state == nullptr || (state->byteLength & 31U) != 0 ||
        state->count != state->byteLength / 32 ||
        state->count >
            xdata::xahau_profile_limits::serialized_object_max_fields)
      return false;
    output = {vectorBytes(state), state->byteLength};
    return true;
  }
  auto const *state = static_cast<BridgeState const *>(
      JS_GetOpaque(input, classId(kind)));
  if (state == nullptr || state->length > sizeof(state->bytes))
    return false;
  output = {state->data, state->length};
  return true;
}

namespace {

enum class PayloadOrigin : std::uint8_t {
  copied,
  ownerView,
  ownerDerived,
};

[[nodiscard]] JSValue newIssue(JSContext *ctx, JSValueConst owner,
                               std::uint8_t const *bytes, std::uint32_t length,
                               PayloadOrigin origin) {
  if (!validIssue(bytes, length))
    return internalError(ctx, "Issue construction requires certified bytes");
  if (origin == PayloadOrigin::ownerView &&
      !isCertifiedObjectRange(ctx, owner, bytes, length))
    return JS_HasException(ctx)
               ? JS_EXCEPTION
               : JS_ThrowTypeError(ctx, "Issue: certified owner is required");
  if (origin == PayloadOrigin::ownerDerived && !JS_IsObject(owner))
    return internalError(ctx, "Issue derived value requires certified owner");
  auto *state = static_cast<IssueState *>(js_malloc(ctx, sizeof(IssueState)));
  if (state == nullptr)
    return oom(ctx);
  state->owner = JS_UNDEFINED;
  if (origin == PayloadOrigin::ownerView) {
    state->data = bytes;
  } else {
    std::memcpy(state->bytes, bytes, length);
    state->data = state->bytes;
  }
  state->length = length;
  for (JSValue &cached : state->cache)
    cached = JS_UNDEFINED;
  JSValue value = newStateObject(ctx, LeafKind::issue, state);
  if (!JS_IsException(value) && origin != PayloadOrigin::copied) {
    state->owner = JS_DupValue(ctx, owner);
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
    test::gcProbeCreated(test::TrackedEntity::issue, value);
    if (!plantLeafCycles(ctx, LeafKind::issue, owner, value)) {
      JS_FreeValue(ctx, value);
      return JS_EXCEPTION;
    }
#endif
  }
  return value;
}

[[nodiscard]] JSValue newVector256(JSContext *ctx, JSValueConst owner,
                                   std::uint8_t const *bytes,
                                   std::uint32_t length, PayloadOrigin origin) {
  if ((length != 0 && bytes == nullptr) || (length & 31U) != 0 ||
      length / 32 >
          xdata::xahau_profile_limits::serialized_object_max_fields)
    return internalError(
        ctx, "Vector256 construction requires certified hash payloads");
  if (origin == PayloadOrigin::ownerView &&
      !isCertifiedObjectRange(ctx, owner, bytes, length))
    return JS_HasException(ctx)
               ? JS_EXCEPTION
               : JS_ThrowTypeError(ctx,
                                   "Vector256: certified owner is required");
  std::size_t const allocation =
      sizeof(VectorState) +
      (origin == PayloadOrigin::copied ? length : std::uint32_t{0});
  auto *state = static_cast<VectorState *>(js_malloc(ctx, allocation));
  if (state == nullptr)
    return oom(ctx);
  state->owner = JS_UNDEFINED;
  state->byteLength = length;
  state->count = length / 32;
  state->cache = nullptr;
  if (origin == PayloadOrigin::copied) {
    state->data = reinterpret_cast<std::uint8_t const *>(state + 1);
    if (length != 0)
      std::memcpy(state + 1, bytes, length);
  } else {
    state->data = bytes;
  }
  JSValue value = newStateObject(ctx, LeafKind::vector256, state);
  if (!JS_IsException(value) && origin != PayloadOrigin::copied) {
    state->owner = JS_DupValue(ctx, owner);
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
    test::gcProbeCreated(test::TrackedEntity::vector256, value);
    if (!plantLeafCycles(ctx, LeafKind::vector256, owner, value)) {
      JS_FreeValue(ctx, value);
      return JS_EXCEPTION;
    }
#endif
  }
  return value;
}

[[nodiscard]] JSValue newXChainBridge(JSContext *ctx, JSValueConst owner,
                                      std::uint8_t const *bytes,
                                      std::uint32_t length,
                                      PayloadOrigin origin) {
  if (bytes == nullptr || length > 130)
    return internalError(ctx,
                         "XChainBridge construction requires certified bytes");
  if (origin == PayloadOrigin::ownerView &&
      !isCertifiedObjectRange(ctx, owner, bytes, length))
    return JS_HasException(ctx)
               ? JS_EXCEPTION
               : JS_ThrowTypeError(ctx,
                                   "XChainBridge: certified owner is required");
  BridgePart parts[4]{};
  std::uint32_t position = 0;
  for (std::uint32_t side = 0; side < 2; ++side) {
    if (position >= length)
      return internalError(ctx, "certified XChainBridge is truncated");
    std::uint32_t const doorLength = bytes[position++];
    if ((doorLength != 0 && doorLength != 20) || doorLength > length - position)
      return internalError(ctx, "certified XChainBridge door is invalid");
    parts[side * 2] = {position, doorLength};
    position += doorLength;
    std::uint32_t const issueLength =
        issueExtent(bytes + position, length - position);
    if (issueLength == 0 || !validIssue(bytes + position, issueLength))
      return internalError(ctx, "certified XChainBridge issue is invalid");
    parts[side * 2 + 1] = {position, issueLength};
    position += issueLength;
  }
  if (position != length)
    return internalError(ctx, "certified XChainBridge has trailing bytes");

  auto *state = static_cast<BridgeState *>(js_malloc(ctx, sizeof(BridgeState)));
  if (state == nullptr)
    return oom(ctx);
  state->owner = JS_UNDEFINED;
  if (origin == PayloadOrigin::ownerView) {
    state->data = bytes;
  } else {
    std::memcpy(state->bytes, bytes, length);
    state->data = state->bytes;
  }
  state->length = length;
  std::memcpy(state->parts, parts, sizeof(parts));
  for (JSValue &cached : state->cache)
    cached = JS_UNDEFINED;
  JSValue value = newStateObject(ctx, LeafKind::xchainBridge, state);
  if (!JS_IsException(value) && origin != PayloadOrigin::copied) {
    state->owner = JS_DupValue(ctx, owner);
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
    test::gcProbeCreated(test::TrackedEntity::xchainBridge, value);
    if (!plantLeafCycles(ctx, LeafKind::xchainBridge, owner, value)) {
      JS_FreeValue(ctx, value);
      return JS_EXCEPTION;
    }
#endif
  }
  return value;
}

} // namespace

JSValue makeIssueBytes(JSContext *ctx, std::uint8_t const *bytes,
                       std::uint32_t length) {
  return newIssue(ctx, JS_UNDEFINED, bytes, length, PayloadOrigin::copied);
}

JSValue makeIssueView(JSContext *ctx, JSValueConst owner,
                      std::uint8_t const *bytes, std::uint32_t length) {
  return newIssue(ctx, owner, bytes, length, PayloadOrigin::ownerView);
}

JSValue makeIssueDerivedBytes(JSContext *ctx, JSValueConst owner,
                              std::uint8_t const *bytes, std::uint32_t length) {
  return newIssue(ctx, owner, bytes, length, PayloadOrigin::ownerDerived);
}

JSValue makeVector256Bytes(JSContext *ctx, std::uint8_t const *bytes,
                           std::uint32_t length) {
  return newVector256(ctx, JS_UNDEFINED, bytes, length, PayloadOrigin::copied);
}

JSValue makeVector256View(JSContext *ctx, JSValueConst owner,
                          std::uint8_t const *bytes, std::uint32_t length) {
  return newVector256(ctx, owner, bytes, length, PayloadOrigin::ownerView);
}

JSValue makeXChainBridgeBytes(JSContext *ctx, std::uint8_t const *bytes,
                              std::uint32_t length) {
  return newXChainBridge(ctx, JS_UNDEFINED, bytes, length,
                         PayloadOrigin::copied);
}

JSValue makeXChainBridgeView(JSContext *ctx, JSValueConst owner,
                             std::uint8_t const *bytes, std::uint32_t length) {
  return newXChainBridge(ctx, owner, bytes, length, PayloadOrigin::ownerView);
}

} // namespace jshookz::provider::types
