#include "xfl/xfl_arithmetic.hpp"

#include <array>
#include <cstdint>
#include <limits>

namespace hook {
namespace {

constexpr std::uint64_t minMagnitude = 1'000'000'000'000'000ull;
constexpr std::uint64_t maxMagnitude = 9'999'999'999'999'999ull;
constexpr std::int32_t minExponent = -96;
constexpr std::int32_t maxExponent = 80;
constexpr std::array<std::uint64_t, 17> powersOfTen{
    1ull,
    10ull,
    100ull,
    1'000ull,
    10'000ull,
    100'000ull,
    1'000'000ull,
    10'000'000ull,
    100'000'000ull,
    1'000'000'000ull,
    10'000'000'000ull,
    100'000'000'000ull,
    1'000'000'000'000ull,
    10'000'000'000'000ull,
    100'000'000'000'000ull,
    1'000'000'000'000'000ull,
    10'000'000'000'000'000ull,
};

static_assert(2 * maxMagnitude < std::numeric_limits<std::int64_t>::max());

[[nodiscard]] std::uint64_t
alignedMagnitude(std::uint64_t magnitude, std::int32_t delta) noexcept
{
#if defined(CONFIG_TEST_XFL_GAP_LOOP_MUTANT)
    // Deliberate non-product mutation: prove the packaged maximum-alignment
    // fuel lane rejects a tempting allocation-free O(exponent-gap) kernel.
    while (delta > 0) {
        magnitude /= 10;
        --delta;
    }
    return magnitude;
#else
    // A canonical mantissa has at most 16 digits. Delta 16 can retain one
    // last digit; every larger gap is exact decimal dust under legacy XFL.
    if (delta > 16)
        return 0;
    return magnitude / powersOfTen[static_cast<std::size_t>(delta)];
#endif
}

[[nodiscard]] std::int64_t
signedMagnitude(XFL const& value, std::uint64_t magnitude) noexcept
{
    auto const signedValue = static_cast<std::int64_t>(magnitude);
    return value.is_negative() ? -signedValue : signedValue;
}

[[nodiscard]] XFLArithmeticResult
normalize(std::int64_t signedMantissa, std::int32_t exponent) noexcept
{
    // The aligned sum is bounded by +/-2*(10^16-1), so negation is defined.
    bool const negative = signedMantissa < 0;
    std::uint64_t magnitude = static_cast<std::uint64_t>(
        negative ? -signedMantissa : signedMantissa);

    while (magnitude < minMagnitude && exponent > minExponent) {
        magnitude *= 10;
        --exponent;
    }
    while (magnitude > maxMagnitude) {
        if (exponent >= maxExponent)
            return {{}, XFLArithmeticIssue::overflow};
        magnitude /= 10;
        ++exponent;
    }

    if (exponent < minExponent || magnitude < minMagnitude)
        return {};
    if (exponent > maxExponent)
        return {{}, XFLArithmeticIssue::overflow};

    return {
        XFL::from_components(
            negative, exponent, static_cast<std::int64_t>(magnitude)),
        XFLArithmeticIssue::none,
    };
}

[[nodiscard]] XFL
canonicalNegate(XFL const& value) noexcept
{
    if (value.is_zero())
        return {};
    return XFL::from_components(
        !value.is_negative(),
        value.exponent(),
        static_cast<std::int64_t>(value.mantissa()));
}

}  // namespace

XFLArithmeticResult
addXahauFloatV1(XFL const& left, XFL const& right) noexcept
{
    if (left.is_zero())
        return {right, XFLArithmeticIssue::none};
    if (right.is_zero())
        return {left, XFLArithmeticIssue::none};

    std::int32_t const leftExponent = left.exponent();
    std::int32_t const rightExponent = right.exponent();
    std::int32_t const exponent =
        leftExponent > rightExponent ? leftExponent : rightExponent;
    std::uint64_t const leftMagnitude = alignedMagnitude(
        left.mantissa(), exponent - leftExponent);
    std::uint64_t const rightMagnitude = alignedMagnitude(
        right.mantissa(), exponent - rightExponent);
    std::int64_t const sum = signedMagnitude(left, leftMagnitude) +
        signedMagnitude(right, rightMagnitude);

    // This inclusive legacy clamp is observable at the minimum exponent.
    if (sum >= -10 && sum <= 10)
        return {};
    return normalize(sum, exponent);
}

XFLArithmeticResult
subtractXahauFloatV1(XFL const& left, XFL const& right) noexcept
{
    return addXahauFloatV1(left, canonicalNegate(right));
}

}  // namespace hook
