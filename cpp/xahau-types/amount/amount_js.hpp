#pragma once

#include <quickjs.h>

#include <cstdint>

namespace jshookz::provider::types {

enum class AmountIssueKind : std::uint8_t {
  native,
  iou,
  mpt,
};

// Existing nominal leaf factories supplied by the provider registrar. Amount
// never publishes a structural stand-in for one of these values. `issue`
// receives no identity bytes for native, currency||issuer for IOU, and the
// 24-byte issuance id for MPT. The read callbacks accept only their exact
// nominal classes and copy into the fixed-size output supplied by Amount.
struct AmountLeafMaterializers {
  JSValue (*accountID)(JSContext *, std::uint8_t const *,
                       std::uint32_t) = nullptr;
  JSValue (*currency)(JSContext *, std::uint8_t const *,
                      std::uint32_t) = nullptr;
  JSValue (*hash192)(JSContext *, std::uint8_t const *,
                     std::uint32_t) = nullptr;
  JSValue (*decimal)(JSContext *, bool, std::uint64_t, std::int32_t) = nullptr;
  JSValue (*issue)(JSContext *, AmountIssueKind, std::uint8_t const *,
                   std::uint32_t) = nullptr;
  bool (*readAccountID)(JSContext *, JSValueConst,
                        std::uint8_t *) noexcept = nullptr;
  bool (*readCurrency)(JSContext *, JSValueConst,
                       std::uint8_t *) noexcept = nullptr;
  bool (*readHash192)(JSContext *, JSValueConst,
                      std::uint8_t *) noexcept = nullptr;
  bool (*readDecimal)(JSContext *, JSValueConst, bool *, std::uint64_t *,
                      std::int32_t *) noexcept = nullptr;
};

// Registers the immutable nominal Amount class and its frozen prototype. The
// registrar rejects an incomplete nominal-leaf table rather than publishing
// values with the wrong runtime identity.
[[nodiscard]] bool registerAmount(JSContext *ctx, JSValueConst global,
                                  AmountLeafMaterializers const &leaves);

// Provider-only canonical materializer. It validates the complete Amount
// representation, copies at most 48 bytes inline, and publishes no borrowed
// storage. No public constructor route reaches this function.
[[nodiscard]] JSValue makeAmountBytes(JSContext *ctx, std::uint8_t const *bytes,
                                      std::uint32_t length);

[[nodiscard]] bool isAmount(JSValueConst value) noexcept;

} // namespace jshookz::provider::types
