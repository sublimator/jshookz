#pragma once

#include "xfl/xfl.hpp"

#include <cstdint>

namespace hook {

enum class XFLArithmeticIssue : std::uint8_t
{
    none,
    overflow,
    division_by_zero,
    invalid,
    count,
};

struct XFLArithmeticResult
{
    XFL value{};
    XFLArithmeticIssue issue = XFLArithmeticIssue::none;

    [[nodiscard]] constexpr bool ok() const noexcept
    {
        return issue == XFLArithmeticIssue::none;
    }
};

[[nodiscard]] XFLArithmeticResult
addXahauFloatV1(XFL const& left, XFL const& right) noexcept;

[[nodiscard]] XFLArithmeticResult
subtractXahauFloatV1(XFL const& left, XFL const& right) noexcept;

[[nodiscard]] XFLArithmeticResult
multiplyXahauFloatV1(XFL const& left, XFL const& right) noexcept;

[[nodiscard]] XFLArithmeticResult
divideXahauFloatV1(XFL const& left, XFL const& right) noexcept;

// Canonicalize the public decimal constructor with the pinned xahaud
// float_set law. A non-zero input that underflows or overflows is invalid;
// zero always mints canonical zero.
[[nodiscard]] XFLArithmeticResult
setXahauFloatV1(std::int64_t mantissa, std::int32_t exponent) noexcept;

}  // namespace hook
