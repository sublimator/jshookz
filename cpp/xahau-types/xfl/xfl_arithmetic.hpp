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

}  // namespace hook
