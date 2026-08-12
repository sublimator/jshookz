/*
 * types.hpp - C++ types compiled into the WASM module
 *
 * These live INSIDE the WASM sandbox alongside QuickJS.
 * They can be used by the JS wrapper functions to provide
 * rich typed operations without crossing the WASM boundary.
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <array>
#include <string_view>
#include <optional>
#include <algorithm>

namespace hook {

// --- Fixed-size hash types ---

template<size_t N>
class Hash {
    std::array<uint8_t, N> data_{};
public:
    Hash() = default;

    explicit Hash(const uint8_t* src, size_t len) {
        size_t copy = std::min(len, N);
        std::memcpy(data_.data(), src, copy);
    }

    explicit Hash(std::string_view hex) {
        from_hex(hex);
    }

    const uint8_t* data() const { return data_.data(); }
    uint8_t* data() { return data_.data(); }
    static constexpr size_t size() { return N; }

    bool operator==(const Hash& o) const { return data_ == o.data_; }
    bool operator!=(const Hash& o) const { return data_ != o.data_; }
    bool operator<(const Hash& o) const { return data_ < o.data_; }

    bool is_zero() const {
        for (auto b : data_) if (b != 0) return false;
        return true;
    }

    // Hex encode to buffer, returns chars written
    size_t to_hex(char* out, size_t out_len) const {
        static const char hex[] = "0123456789ABCDEF";
        size_t needed = N * 2;
        if (out_len < needed) return 0;
        for (size_t i = 0; i < N; i++) {
            out[i * 2]     = hex[data_[i] >> 4];
            out[i * 2 + 1] = hex[data_[i] & 0x0F];
        }
        return needed;
    }

    bool from_hex(std::string_view hex) {
        if (hex.size() != N * 2) return false;
        for (size_t i = 0; i < N; i++) {
            auto hi = hex_nibble(hex[i * 2]);
            auto lo = hex_nibble(hex[i * 2 + 1]);
            if (hi < 0 || lo < 0) return false;
            data_[i] = (hi << 4) | lo;
        }
        return true;
    }

private:
    static int hex_nibble(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }
};

using Hash128 = Hash<16>;
using Hash160 = Hash<20>;  // AccountID size
using Hash256 = Hash<32>;
using Hash512 = Hash<64>;

// --- AccountID (20-byte hash with r-address encoding awareness) ---

class AccountID : public Hash160 {
public:
    using Hash160::Hash160;
    using Hash160::operator==;

    // Could add r-address encode/decode here later
};

// --- XFL (Xahau floating point - 64-bit encoded) ---
// Hooks use a custom 64-bit float format for amounts

class XFL {
    int64_t val_;
public:
    XFL() : val_(0) {}
    explicit XFL(int64_t raw) : val_(raw) {}

    int64_t raw() const { return val_; }

    // XFL encoding: sign(1) | exponent(8) | mantissa(54)
    // Exponent is biased by 97
    static constexpr int64_t EXPONENT_BIAS = 97;
    static constexpr int64_t MANTISSA_MASK = (1LL << 54) - 1;

    bool is_negative() const { return val_ < 0; }
    bool is_zero() const { return (val_ & MANTISSA_MASK) == 0; }

    int64_t mantissa() const {
        return val_ & MANTISSA_MASK;
    }

    int exponent() const {
        return (int)((val_ >> 54) & 0xFF) - EXPONENT_BIAS;
    }

    static XFL from_components(bool negative, int exponent, int64_t mantissa) {
        int64_t v = mantissa & MANTISSA_MASK;
        v |= ((int64_t)(exponent + EXPONENT_BIAS) & 0xFF) << 54;
        if (negative) v = -v;  // simplified
        return XFL(v);
    }

    // Basic arithmetic (simplified — real impl needs proper normalization)
    XFL operator+(const XFL& o) const;  // TODO
    XFL operator-(const XFL& o) const;  // TODO
    XFL operator*(const XFL& o) const;  // TODO
    XFL operator/(const XFL& o) const;  // TODO

    bool operator==(const XFL& o) const { return val_ == o.val_; }
    bool operator<(const XFL& o) const;   // TODO: proper comparison
};

// --- Amount (XRP drops or IOU with currency + issuer) ---

struct Currency {
    std::array<uint8_t, 20> code{};  // 3-char ASCII or 20-byte hex

    bool is_xrp() const {
        for (auto b : code) if (b != 0) return false;
        return true;
    }

    static Currency from_ascii(const char* s) {
        Currency c;
        // Standard 3-char currency code goes in bytes 12-14
        size_t len = std::min(strlen(s), size_t(3));
        std::memcpy(c.code.data() + 12, s, len);
        return c;
    }
};

class Amount {
    int64_t drops_{};       // For XRP: drops. For IOU: XFL-encoded value
    Currency currency_{};
    AccountID issuer_{};
    bool is_iou_{false};
public:
    Amount() = default;

    // XRP amount (in drops)
    static Amount xrp(int64_t drops) {
        Amount a;
        a.drops_ = drops;
        return a;
    }

    // IOU amount
    static Amount iou(XFL value, Currency currency, AccountID issuer) {
        Amount a;
        a.drops_ = value.raw();
        a.currency_ = currency;
        a.issuer_ = issuer;
        a.is_iou_ = true;
        return a;
    }

    bool is_xrp() const { return !is_iou_; }
    bool is_iou() const { return is_iou_; }
    int64_t drops() const { return drops_; }
    XFL iou_value() const { return XFL(drops_); }
    const Currency& currency() const { return currency_; }
    const AccountID& issuer() const { return issuer_; }
};

// --- Keylet (34 bytes: 2-byte type + 32-byte key) ---

class Keylet {
    uint16_t type_{};
    Hash256 key_{};
public:
    Keylet() = default;
    Keylet(uint16_t type, Hash256 key) : type_(type), key_(key) {}

    uint16_t type() const { return type_; }
    const Hash256& key() const { return key_; }

    const uint8_t* data() const { return reinterpret_cast<const uint8_t*>(this); }
    static constexpr size_t size() { return 34; }

    bool operator==(const Keylet& o) const { return type_ == o.type_ && key_ == o.key_; }
};

} // namespace hook
