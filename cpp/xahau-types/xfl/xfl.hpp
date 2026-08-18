#pragma once

#include <cstdint>

namespace hook {

class XFL
{
    int64_t val_;

public:
    XFL() : val_(0) {}
    explicit XFL(int64_t raw) : val_(raw) {}

    int64_t raw() const { return val_; }

    static constexpr std::uint64_t EXPONENT_BIAS = 97;
    static constexpr std::uint64_t MANTISSA_MASK =
        (std::uint64_t{1} << 54) - 1;
    static constexpr std::uint64_t POSITIVE_MASK = std::uint64_t{1} << 62;

    bool is_negative() const
    {
        return val_ != 0 &&
            (static_cast<std::uint64_t>(val_) & POSITIVE_MASK) == 0;
    }

    bool is_zero() const
    {
        return (static_cast<std::uint64_t>(val_) & MANTISSA_MASK) == 0;
    }

    std::uint64_t mantissa() const
    {
        return static_cast<std::uint64_t>(val_) & MANTISSA_MASK;
    }

    int exponent() const
    {
        if (val_ == 0)
            return 0;
        return static_cast<int>(
                   (static_cast<std::uint64_t>(val_) >> 54) & 0xFF) -
            static_cast<int>(EXPONENT_BIAS);
    }

    static XFL from_components(bool negative, int exponent, int64_t mantissa)
    {
        std::uint64_t v =
            static_cast<std::uint64_t>(mantissa) & MANTISSA_MASK;
        v |= (static_cast<std::uint64_t>(
                  exponent + static_cast<int>(EXPONENT_BIAS)) &
              0xFF)
            << 54;
        if (!negative && mantissa != 0)
            v |= POSITIVE_MASK;
        return XFL(static_cast<std::int64_t>(v));
    }

    XFL operator+(const XFL& o) const;
    XFL operator-(const XFL& o) const;
    XFL operator*(const XFL& o) const;
    XFL operator/(const XFL& o) const;

    bool operator==(const XFL& o) const { return val_ == o.val_; }
    bool operator<(const XFL& o) const;
};

}  // namespace hook
