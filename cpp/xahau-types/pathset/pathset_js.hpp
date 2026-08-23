#pragma once

#include <quickjs.h>

#include <cstdint>

namespace jshookz::provider::types {

struct PathSetLeafMaterializers {
  JSValue (*accountID)(JSContext *, std::uint8_t const *,
                       std::uint32_t) = nullptr;
  JSValue (*currency)(JSContext *, std::uint8_t const *,
                      std::uint32_t) = nullptr;
  // Allocation-free/no-JavaScript proof that owner is the provider's exact
  // certified immutable backing and that the complete range lies within it.
  bool (*certifiedRange)(JSContext *, JSValueConst, std::uint8_t const *,
                         std::uint32_t) noexcept = nullptr;
};

// Registers hidden immutable PathSet/Path/PathHop classes plus their hidden
// iterator class. Every instance is provider-minted; no global is published.
[[nodiscard]] bool registerPathSet(JSContext *ctx,
                                   PathSetLeafMaterializers const &leaves);

// Provider-only materializer over a canonical field payload. `owner` must be
// the certified immutable backing owner for [bytes, bytes + length). The
// PathSet retains that exact owner; it neither copies the backing nor creates
// a per-child owner. Complete CertifyWire validation runs before publication.
[[nodiscard]] JSValue makePathSetBytes(JSContext *ctx, JSValueConst owner,
                                       std::uint8_t const *bytes,
                                       std::uint32_t length);

[[nodiscard]] bool isPathSet(JSValueConst value) noexcept;
[[nodiscard]] bool isPath(JSValueConst value) noexcept;
[[nodiscard]] bool isPathHop(JSValueConst value) noexcept;

} // namespace jshookz::provider::types
