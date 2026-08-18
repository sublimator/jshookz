#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace hook {

template <size_t N>
class Hash
{
    std::array<uint8_t, N> data_{};

public:
    Hash() = default;

    explicit Hash(const uint8_t* src, size_t len)
    {
        size_t copy = std::min(len, N);
        std::memcpy(data_.data(), src, copy);
    }

    explicit Hash(std::string_view hex)
    {
        from_hex(hex);
    }

    const uint8_t* data() const { return data_.data(); }
    uint8_t* data() { return data_.data(); }
    static constexpr size_t size() { return N; }

    bool operator==(const Hash& o) const { return data_ == o.data_; }
    bool operator!=(const Hash& o) const { return data_ != o.data_; }
    bool operator<(const Hash& o) const { return data_ < o.data_; }

    bool is_zero() const
    {
        for (auto b : data_)
            if (b != 0)
                return false;
        return true;
    }

    size_t to_hex(char* out, size_t out_len) const
    {
        static const char hex[] = "0123456789ABCDEF";
        size_t needed = N * 2;
        if (out_len < needed)
            return 0;
        for (size_t i = 0; i < N; i++) {
            out[i * 2] = hex[data_[i] >> 4];
            out[i * 2 + 1] = hex[data_[i] & 0x0F];
        }
        return needed;
    }

    bool from_hex(std::string_view hex)
    {
        if (hex.size() != N * 2)
            return false;
        for (size_t i = 0; i < N; i++) {
            auto hi = hex_nibble(hex[i * 2]);
            auto lo = hex_nibble(hex[i * 2 + 1]);
            if (hi < 0 || lo < 0)
                return false;
            data_[i] = (hi << 4) | lo;
        }
        return true;
    }

private:
    static int hex_nibble(char c)
    {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    }
};

using Hash128 = Hash<16>;
using Hash160 = Hash<20>;
using Hash256 = Hash<32>;
using Hash512 = Hash<64>;

}  // namespace hook
