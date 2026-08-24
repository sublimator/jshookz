#include "object_gc_lifetime_probe_hooks.hpp"

#include <array>
#include <cstring>

namespace jshookz::provider::types::test {
namespace {

struct ProbeState {
  HiddenEdge selected = HiddenEdge::none;
  bool disableSelectedMark = false;
  JSValue pendingTarget = JS_UNDEFINED;
  void *source = nullptr;
  bool sourceFinalized = false;
  std::array<EntityCounts, static_cast<std::size_t>(TrackedEntity::count)>
      counts{};
};

ProbeState state;

struct RadixProbeState {
  RadixPoison poison = RadixPoison::none;
  RadixAllocationLedger ledger{};
};

RadixProbeState radixState;

[[nodiscard]] std::size_t indexOf(TrackedEntity entity) noexcept {
  return static_cast<std::size_t>(entity);
}

[[nodiscard]] void *objectPointer(JSValueConst value) noexcept {
  return JS_IsObject(value) ? JS_VALUE_GET_PTR(value) : nullptr;
}

} // namespace

void configureGCProbe(HiddenEdge edge, bool disableSelectedMark) noexcept {
  state = {};
  state.selected = edge;
  state.disableSelectedMark = disableSelectedMark;
}

void enableAllGCProbeMarks() noexcept { state.disableSelectedMark = false; }

bool gcProbeMarkEnabled(HiddenEdge edge) noexcept {
  return edge != state.selected || !state.disableSelectedMark;
}

bool gcProbePlantCycle(JSContext *ctx, HiddenEdge edge,
                       JSValueConst propertyOwner, JSValueConst propertyValue,
                       JSValueConst markSource) {
  if (edge != state.selected)
    return true;
  if (state.source != nullptr)
    return JS_ThrowInternalError(ctx, "GC probe cycle was planted twice"),
           false;
  state.source = objectPointer(markSource);
  if (state.source == nullptr)
    return JS_ThrowInternalError(ctx, "GC probe mark source is not an object"),
           false;
  return JS_SetPropertyStr(ctx, propertyOwner, "__gc_lifetime_probe_cycle__",
                           JS_DupValue(ctx, propertyValue)) >= 0;
}

bool gcProbePlantPendingCycle(JSContext *ctx, HiddenEdge edge,
                              JSValueConst propertyOwner) {
  if (edge != state.selected || JS_IsUndefined(state.pendingTarget))
    return true;
  return gcProbePlantCycle(ctx, edge, propertyOwner, state.pendingTarget,
                           state.pendingTarget);
}

void gcProbeSetPendingTarget(JSValueConst target) noexcept {
  state.pendingTarget = target;
}

void gcProbeClearPendingTarget() noexcept {
  state.pendingTarget = JS_UNDEFINED;
}

void gcProbeCreated(TrackedEntity entity, JSValueConst) noexcept {
  ++state.counts[indexOf(entity)].created;
}

void gcProbeFinalized(TrackedEntity entity, JSValueConst value) noexcept {
  ++state.counts[indexOf(entity)].finalized;
  if (state.source != nullptr && objectPointer(value) == state.source)
    state.sourceFinalized = true;
}

EntityCounts gcProbeCounts(TrackedEntity entity) noexcept {
  return state.counts[indexOf(entity)];
}

bool gcProbeSourceFinalized() noexcept { return state.sourceFinalized; }

bool gcProbeAllFinalized() noexcept {
  for (auto const &entry : state.counts) {
    if (entry.created != entry.finalized)
      return false;
  }
  return true;
}

char const *hiddenEdgeName(HiddenEdge edge) noexcept {
  switch (edge) {
  case HiddenEdge::none:
    return "none";
  case HiddenEdge::objectOwner:
    return "object-owner";
  case HiddenEdge::objectCacheValue:
    return "object-cache-value";
  case HiddenEdge::arrayOwner:
    return "array-owner";
  case HiddenEdge::arrayCacheValue:
    return "array-cache-value";
  case HiddenEdge::iteratorArray:
    return "iterator-array";
  case HiddenEdge::fieldTableDescriptor:
    return "field-table-descriptor";
  }
  return "unknown";
}

bool parseHiddenEdge(char const *name, HiddenEdge &edge) noexcept {
  if (name == nullptr)
    return false;
  for (HiddenEdge candidate :
       {HiddenEdge::objectOwner, HiddenEdge::objectCacheValue,
        HiddenEdge::arrayOwner, HiddenEdge::arrayCacheValue,
        HiddenEdge::iteratorArray, HiddenEdge::fieldTableDescriptor}) {
    if (std::strcmp(name, hiddenEdgeName(candidate)) == 0) {
      edge = candidate;
      return true;
    }
  }
  return false;
}

void configureRadixProbe(RadixPoison poison) noexcept {
  radixState = {};
  radixState.poison = poison;
}

bool gcProbeCorruptRadixCounts() noexcept {
  return radixState.poison == RadixPoison::corruptCounts;
}

bool gcProbeAllocateOnRadixHit() noexcept {
  return radixState.poison == RadixPoison::allocateOnHit;
}

void gcProbeRadixAllocated(RadixAllocationKind kind,
                           std::size_t requestedBytes) noexcept {
  switch (kind) {
  case RadixAllocationKind::root:
    ++radixState.ledger.rootAllocations;
    break;
  case RadixAllocationKind::branch:
    ++radixState.ledger.branchAllocations;
    break;
  case RadixAllocationKind::leaf:
    ++radixState.ledger.leafAllocations;
    break;
  case RadixAllocationKind::hitPoison:
    ++radixState.ledger.hitPoisonAllocations;
    break;
  }
  radixState.ledger.requestedBytes += requestedBytes;
}

RadixAllocationLedger gcProbeRadixLedger() noexcept {
  return radixState.ledger;
}

} // namespace jshookz::provider::types::test
