#pragma once

#include <quickjs.h>

#include <cstdint>

namespace jshookz::provider::types {

enum class AmountIssueKind : std::uint8_t {
  native,
  iou,
  mpt,
};

// Existing nominal leaf materializers supplied by the provider registrar.
// Amount never publishes a structural stand-in for one of these values.
// `issue` receives no identity bytes for native, currency||issuer for IOU, and
// the 24-byte issuance id for MPT.
struct AmountLeafMaterializers {
  JSValue (*accountID)(JSContext *, JSValueConst, std::uint8_t const *,
                       std::uint32_t) = nullptr;
  JSValue (*currency)(JSContext *, JSValueConst, std::uint8_t const *,
                      std::uint32_t) = nullptr;
  JSValue (*hash192)(JSContext *, JSValueConst, std::uint8_t const *,
                     std::uint32_t) = nullptr;
  JSValue (*decimal)(JSContext *, bool, std::uint64_t, std::int32_t) = nullptr;
  JSValue (*issue)(JSContext *, JSValueConst, AmountIssueKind,
                   std::uint8_t const *, std::uint32_t) = nullptr;
};

// Registers the hidden immutable nominal Amount class and its frozen instance
// prototype. No value-level global or authoring factory is published.
[[nodiscard]] bool registerAmount(JSContext *ctx,
                                  AmountLeafMaterializers const &leaves);

// Provider-only canonical materializer. It validates the complete Amount
// representation, copies at most 48 bytes inline, and publishes no borrowed
// storage. No public constructor route reaches this function.
[[nodiscard]] JSValue makeAmountBytes(JSContext *ctx, std::uint8_t const *bytes,
                                      std::uint32_t length);
[[nodiscard]] JSValue makeAmountView(JSContext *ctx, JSValueConst owner,
                                     std::uint8_t const *bytes,
                                     std::uint32_t length);

[[nodiscard]] bool isAmount(JSValueConst value) noexcept;

} // namespace jshookz::provider::types
