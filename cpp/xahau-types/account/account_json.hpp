#pragma once

#include <cstdint>

#include <quickjs.h>

namespace jshookz::provider::types {

inline constexpr std::uint32_t accountIDPayloadBytes = 20;
inline constexpr std::uint32_t accountIDClassicMaxChars = 35;

// Fixed native result for one canonical XRPL/Xahau classic account address.
// The terminator is provided for native probes; QuickJS uses the exact length.
struct AccountIDClassicString {
  char chars[accountIDClassicMaxChars + 1];
  std::uint32_t length;
};

// Stack-only Base58Check encoding for the classic-account version (0x00).
// The output is reset before validation and is published only on success.
[[nodiscard]] bool
encodeAccountIDClassic(std::uint8_t const *bytes, std::uint32_t length,
                       AccountIDClassicString *output) noexcept;

// Provider-only canonical JSON leaf factory. It performs no native allocation;
// JS_NewStringLen is its sole fallible allocation and propagates QuickJS OOM.
[[nodiscard]] JSValue
makeAccountIDCanonicalJSONString(JSContext *ctx, std::uint8_t const *bytes,
                                 std::uint32_t length) noexcept;

} // namespace jshookz::provider::types
