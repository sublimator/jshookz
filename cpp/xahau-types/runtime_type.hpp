#pragma once

#include <quickjs.h>

#include <cstdint>

namespace jshookz::provider::types {

// Stable native classifier coordinates.  They are embedded as the magic
// value of each C-owned Symbol.hasInstance function; no JavaScript-readable
// property participates in nominal proof.
enum class RuntimeTypeId : std::int32_t {
  accountID,
  amount,
  currency,
  hash,
  hash128,
  hash160,
  hash192,
  hash256,
  iouAmount,
  issue,
  mptAmount,
  nativeAmount,
  path,
  pathHop,
  pathSet,
  result,
  stArray,
  stBlob,
  stObject,
  serializedField,
  uInt,
  uInt8,
  uInt16,
  uInt32,
  uInt64,
  vector256,
  voidResult,
  xChainBridge,
  xflDecimal,
};

// Install the one canonical native classifier on an existing mutable factory.
// The property is non-writable, non-enumerable, and non-configurable.  The
// caller freezes the complete factory after installing its remaining statics.
[[nodiscard]] bool installRuntimeTypeClassifier(JSContext *ctx,
                                                JSValueConst target,
                                                RuntimeTypeId type);

// Publish a frozen ordinary object carrying only the canonical classifier.
// It is deliberately neither callable nor constructible and has no own
// `prototype` property.
[[nodiscard]] bool publishRuntimeType(JSContext *ctx, JSValueConst global,
                                      char const *name, RuntimeTypeId type);

// Allocation-free, no-JavaScript classification seam used by the C callback
// and native proof.  It reads only exact QuickJS class identity and existing
// immutable native width/variant state.
[[nodiscard]] bool runtimeTypeClassifies(RuntimeTypeId type,
                                         JSValueConst value) noexcept;

} // namespace jshookz::provider::types
