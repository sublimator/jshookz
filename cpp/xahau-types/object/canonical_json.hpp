#pragma once

#include <quickjs.h>

#include <cstdint>

namespace jshookz::provider::types {

inline constexpr std::uint32_t canonicalJSONMaximumPayloadBytes = 1'048'576;
inline constexpr std::uint32_t canonicalJSONMaximumNodes = 65'536;

// Provider-only canonical JSON factories. Every successful call returns a
// fresh JSON-compatible value. The payload is normally already certified;
// every entry point still validates its own extent and representation before
// reading so an integration mistake becomes an internal JS error, not UB.
[[nodiscard]] JSValue makeAccountIDCanonicalJSON(JSContext *ctx,
                                                 std::uint8_t const *bytes,
                                                 std::uint32_t length) noexcept;
[[nodiscard]] JSValue makeAmountCanonicalJSON(JSContext *ctx,
                                              std::uint8_t const *bytes,
                                              std::uint32_t length) noexcept;
[[nodiscard]] JSValue makeCurrencyCanonicalJSON(JSContext *ctx,
                                                std::uint8_t const *bytes,
                                                std::uint32_t length) noexcept;
[[nodiscard]] JSValue makeIssueCanonicalJSON(JSContext *ctx,
                                             std::uint8_t const *bytes,
                                             std::uint32_t length) noexcept;
[[nodiscard]] JSValue makePathSetCanonicalJSON(JSContext *ctx,
                                               std::uint8_t const *bytes,
                                               std::uint32_t length) noexcept;
[[nodiscard]] JSValue makeVector256CanonicalJSON(JSContext *ctx,
                                                 std::uint8_t const *bytes,
                                                 std::uint32_t length) noexcept;
[[nodiscard]] JSValue
makeXChainBridgeCanonicalJSON(JSContext *ctx, std::uint8_t const *bytes,
                              std::uint32_t length) noexcept;

} // namespace jshookz::provider::types
