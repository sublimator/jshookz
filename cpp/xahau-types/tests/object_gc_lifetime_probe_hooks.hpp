#pragma once

#include <quickjs.h>

#include <cstddef>
#include <cstdint>

namespace jshookz::provider::types::test {

enum class HiddenEdge : std::uint8_t {
  none,
  objectOwner,
  objectCacheValue,
  arrayOwner,
  arrayCacheValue,
  iteratorArray,
  fieldTableDescriptor,
};

enum class TrackedEntity : std::uint8_t {
  owner,
  object,
  array,
  iterator,
  fieldTable,
  fieldDescriptor,
  count,
};

struct EntityCounts {
  std::uint32_t created = 0;
  std::uint32_t finalized = 0;
};

enum class RadixAllocationKind : std::uint8_t {
  root,
  branch,
  leaf,
  hitPoison,
};

enum class RadixPoison : std::uint8_t {
  none,
  corruptCounts,
  allocateOnHit,
};

struct RadixAllocationLedger {
  std::uint32_t rootAllocations = 0;
  std::uint32_t branchAllocations = 0;
  std::uint32_t leafAllocations = 0;
  std::uint32_t hitPoisonAllocations = 0;
  std::size_t requestedBytes = 0;
};

struct ArrayCacheMetrics {
  bool rootPresent = false;
  std::uint32_t branchCount = 0;
  std::uint32_t pageCount = 0;
  std::uint32_t valueCount = 0;
  std::uint32_t reachableBranches = 0;
  std::uint32_t reachablePages = 0;
  std::uint32_t reachableValues = 0;
  std::uint32_t reservedVersion = 0;
  std::uint32_t arrayLength = 0;
  std::uint32_t ownerByteCount = 0;
  std::uint32_t ownerFieldCount = 0;
  std::uint32_t ownerScopeCount = 0;
  std::size_t rootBytes = 0;
  std::size_t branchBytes = 0;
  std::size_t leafBytes = 0;
  std::size_t requestedBytes = 0;
  std::size_t allocationCount = 0;
};

void configureGCProbe(HiddenEdge edge, bool disableSelectedMark) noexcept;
void enableAllGCProbeMarks() noexcept;

[[nodiscard]] bool gcProbeMarkEnabled(HiddenEdge edge) noexcept;
[[nodiscard]] bool gcProbePlantCycle(JSContext *ctx, HiddenEdge edge,
                                     JSValueConst propertyOwner,
                                     JSValueConst propertyValue,
                                     JSValueConst markSource);
[[nodiscard]] bool gcProbePlantPendingCycle(JSContext *ctx, HiddenEdge edge,
                                            JSValueConst propertyOwner);

void gcProbeSetPendingTarget(JSValueConst target) noexcept;
void gcProbeClearPendingTarget() noexcept;
void gcProbeCreated(TrackedEntity entity, JSValueConst value) noexcept;
void gcProbeFinalized(TrackedEntity entity, JSValueConst value) noexcept;

[[nodiscard]] EntityCounts gcProbeCounts(TrackedEntity entity) noexcept;
[[nodiscard]] bool gcProbeSourceFinalized() noexcept;
[[nodiscard]] bool gcProbeAllFinalized() noexcept;
[[nodiscard]] char const *hiddenEdgeName(HiddenEdge edge) noexcept;
[[nodiscard]] bool parseHiddenEdge(char const *name, HiddenEdge &edge) noexcept;

void configureRadixProbe(RadixPoison poison) noexcept;
[[nodiscard]] bool gcProbeCorruptRadixCounts() noexcept;
[[nodiscard]] bool gcProbeAllocateOnRadixHit() noexcept;
void gcProbeRadixAllocated(RadixAllocationKind kind,
                           std::size_t requestedBytes) noexcept;
[[nodiscard]] RadixAllocationLedger gcProbeRadixLedger() noexcept;

// Implemented in object_js.cpp where the private ArrayState and
// ArrayCacheRoot are visible. This is allocation-free, has no JavaScript
// surface, and exists only in the separately compiled probe binary.
[[nodiscard]] bool inspectArrayCache(JSValueConst array,
                                     ArrayCacheMetrics &metrics) noexcept;

} // namespace jshookz::provider::types::test
