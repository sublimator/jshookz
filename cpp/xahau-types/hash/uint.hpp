#pragma once

#include <cstdint>

namespace hook {

// Origin picks the cheap form. from(n) / add stay an integer. A slice
// from an STObject stays bytes. Do not invent the other side at birth.
// Cache only expensive derived values (hex, JS bigint), not a 4-byte
// big-endian encode. Hash / AccountID are not this: parsing hex or
// base58 already produces bytes, so the string can be kept as the
// validated repr.

class UInt
{
    std::uint64_t value_{};
    std::uint8_t bits_{};

public:
    UInt() = default;
    UInt(std::uint8_t bits, std::uint64_t value) : value_(value), bits_(bits) {}

    std::uint8_t bits() const { return bits_; }
    std::uint64_t value() const { return value_; }
    std::uint8_t byteLength() const { return static_cast<std::uint8_t>(bits_ / 8); }
};

}  // namespace hook
