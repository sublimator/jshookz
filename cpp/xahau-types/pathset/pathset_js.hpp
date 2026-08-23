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

// Registers public immutable PathSet/Path/PathHop classes plus their private
// iterator class. Only PathSet receives a global factory object; Path and
// PathHop are provider-minted interface values.
[[nodiscard]] bool registerPathSet(JSContext *ctx, JSValueConst global,
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
