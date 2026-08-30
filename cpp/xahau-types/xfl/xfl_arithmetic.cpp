#include "xfl/xfl_arithmetic.hpp"

#include <array>
#include <climits>
#include <cstdint>
#include <limits>

namespace hook {
namespace {

constexpr std::uint64_t minMagnitude = 1'000'000'000'000'000ull;
constexpr std::uint64_t maxMagnitude = 9'999'999'999'999'999ull;
constexpr std::int32_t minExponent = -96;
constexpr std::int32_t maxExponent = 80;
[[maybe_unused]] constexpr std::uint64_t order15Threshold =
    999'999'999'999'998ull;
[[maybe_unused]] constexpr std::uint64_t order16Threshold =
    9'999'999'999'999'979ull;
[[maybe_unused]] constexpr std::uint64_t order17Threshold =
    99'999'999'999'999'593ull;
[[maybe_unused]] constexpr std::uint64_t order18Threshold =
    999'999'999'999'995'969ull;
constexpr std::uint64_t maxDivideQuotient = 10'111'111'111'111'118ull;
constexpr std::uint32_t maxDividePositions = 16;
constexpr std::uint32_t maxDivideSubtractions = 167;
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
static_assert(sizeof(unsigned __int128) * CHAR_BIT >= 128);

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

[[nodiscard]] XFLArithmeticResult
normalizeLive(std::uint64_t magnitude, std::int32_t exponent,
              bool negative = false) noexcept
{
    if (magnitude == 0)
        return {};

#if defined(CONFIG_TEST_XFL_GAP_LOOP_MUTANT)
    // Deliberate non-product mutation: make cost depend on the exponent gap
    // while preserving the value, so the relational fuel gate must fire.
    volatile std::uint64_t gapValue = magnitude;
    std::int32_t gap = exponent < 0 ? -exponent : exponent;
    while (gap-- > 0) {
        gapValue ^= static_cast<std::uint64_t>(gap);
        gapValue ^= static_cast<std::uint64_t>(gap);
    }
    magnitude = gapValue;
#endif

    // Pinned xahaud classifies decimal order through host log10. Its rounded
    // transition points are part of xahauFloatV1, so preserve them with exact
    // integer comparisons rather than replacing them with digit counting.
    std::int32_t order = 18;
#if defined(CONFIG_TEST_XFL_DIGIT_COUNT_MUTANT)
    if (magnitude < 1'000'000'000'000'000ull)
        order = 14;
    else if (magnitude < 10'000'000'000'000'000ull)
        order = 15;
    else if (magnitude < 100'000'000'000'000'000ull)
        order = 16;
    else if (magnitude < 1'000'000'000'000'000'000ull)
        order = 17;
#else
    if (magnitude < order15Threshold) {
        order = 0;
        for (std::uint64_t remaining = magnitude; remaining >= 10;
             remaining /= 10)
            ++order;
    } else if (magnitude < order16Threshold)
        order = 15;
    else if (magnitude < order17Threshold)
        order = 16;
    else if (magnitude < order18Threshold)
        order = 17;
#endif

    std::int32_t const adjust = 15 - order;
    if (adjust > 0) {
        magnitude *= powersOfTen[static_cast<std::size_t>(adjust)];
        exponent -= adjust;
    } else if (adjust < 0) {
        magnitude /= powersOfTen[static_cast<std::size_t>(-adjust)];
        exponent -= adjust;
    }

    if (magnitude == 0)
        return {};
    if (magnitude < minMagnitude) {
        if (magnitude == minMagnitude - 1)
            ++magnitude;
        else {
            magnitude *= 10;
            --exponent;
        }
    }
    if (magnitude > maxMagnitude) {
        if (magnitude == maxMagnitude + 1)
            --magnitude;
        else {
            magnitude /= 10;
            ++exponent;
        }
    }

    if (exponent < minExponent || magnitude == 0)
        return {};
    if (exponent > maxExponent)
        return {{}, XFLArithmeticIssue::overflow};
    if (magnitude < minMagnitude || magnitude > maxMagnitude)
        return {{}, XFLArithmeticIssue::invalid};

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
setXahauFloatV1(std::int64_t mantissa, std::int32_t exponent) noexcept
{
    if (mantissa == 0)
        return {};

    // Pinned xahaud deliberately nudges INT64_MIN inward before taking its
    // magnitude so negation remains defined.
    if (mantissa == std::numeric_limits<std::int64_t>::min())
        ++mantissa;
    bool const negative = mantissa < 0;
    std::uint64_t const magnitude = static_cast<std::uint64_t>(
        negative ? -mantissa : mantissa);
    XFLArithmeticResult const result =
        normalizeLive(magnitude, exponent, negative);

    // float_set exposes every non-zero underflow/overflow/canonicalization
    // failure as INVALID_FLOAT. Keep that one local failure category here.
    if (!result.ok() || result.value.is_zero())
        return {{}, XFLArithmeticIssue::invalid};
    return result;
}

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

    // The legacy IOUAmount add path applies this inclusive clamp at every
    // aligned exponent, even when the residual could otherwise normalize.
    if (sum >= -10 && sum <= 10)
        return {};
    return normalize(sum, exponent);
}

XFLArithmeticResult
subtractXahauFloatV1(XFL const& left, XFL const& right) noexcept
{
    return addXahauFloatV1(left, canonicalNegate(right));
}

XFLArithmeticResult
multiplyXahauFloatV1(XFL const& left, XFL const& right) noexcept
{
    if (left.is_zero() || right.is_zero())
        return {};

    unsigned __int128 product =
        static_cast<unsigned __int128>(left.mantissa()) * right.mantissa();
#if defined(CONFIG_TEST_XFL_NARROW_PRODUCT_MUTANT)
    product = static_cast<std::uint64_t>(product);
#endif
    std::uint64_t magnitude =
        static_cast<std::uint64_t>(product / minMagnitude);
#if defined(CONFIG_TEST_XFL_NEAREST_MUTANT)
    if (product % minMagnitude != 0)
        ++magnitude;
#endif
    std::int32_t const exponent = static_cast<std::int32_t>(left.exponent()) +
        static_cast<std::int32_t>(right.exponent()) + 15;
    return normalizeLive(
        magnitude, exponent, left.is_negative() != right.is_negative());
}

XFLArithmeticResult
divideXahauFloatV1(XFL const& left, XFL const& right) noexcept
{
#if defined(CONFIG_TEST_XFL_NUMERATOR_ZERO_FIRST_MUTANT)
    if (left.is_zero())
        return {};
#endif
    if (right.is_zero())
        return {{}, XFLArithmeticIssue::division_by_zero};
#if !defined(CONFIG_TEST_XFL_NUMERATOR_ZERO_FIRST_MUTANT)
    if (left.is_zero())
        return {};
#endif

    XFL const positiveOne = XFL::from_components(false, -15, minMagnitude);
    if (right.raw() == positiveOne.raw())
        return {left, XFLArithmeticIssue::none};

    std::uint64_t numerator = left.mantissa();
    std::int32_t numeratorExponent = left.exponent();
    std::uint64_t denominator = right.mantissa();
    std::int32_t denominatorExponent = right.exponent();

    XFLArithmeticResult const normalizedNumerator =
        normalizeLive(numerator, numeratorExponent);
    XFLArithmeticResult const normalizedDenominator =
        normalizeLive(denominator, denominatorExponent);
    if (!normalizedNumerator.ok() || !normalizedDenominator.ok())
        return {{}, XFLArithmeticIssue::invalid};
    if (normalizedNumerator.value.is_zero())
        return {};
    if (normalizedDenominator.value.is_zero())
        return {{}, XFLArithmeticIssue::division_by_zero};

    numerator = normalizedNumerator.value.mantissa();
    numeratorExponent = normalizedNumerator.value.exponent();
    denominator = normalizedDenominator.value.mantissa();
    denominatorExponent = normalizedDenominator.value.exponent();

#if defined(CONFIG_TEST_XFL_EXACT_DIVIDE_MUTANT)
    unsigned __int128 const scaled =
        static_cast<unsigned __int128>(numerator) * minMagnitude;
    std::uint64_t const exactQuotient =
        static_cast<std::uint64_t>(scaled / denominator);
    return normalizeLive(
        exactQuotient,
        numeratorExponent - denominatorExponent - 15,
        left.is_negative() != right.is_negative());
#endif

    if (denominator > numerator) {
        denominator /= 10;
        ++denominatorExponent;
    }
    if (denominator == 0)
        return {{}, XFLArithmeticIssue::division_by_zero};
    if (denominator < numerator && denominator * 10 <= numerator) {
        denominator *= 10;
        --denominatorExponent;
    }

    std::uint64_t quotient = 0;
    std::int32_t quotientExponent = numeratorExponent - denominatorExponent;
    std::uint32_t totalSubtractions = 0;

    for (std::uint32_t position = 0;
         position < maxDividePositions && denominator > 0; ++position) {
        std::uint32_t coefficient = 0;
#if defined(CONFIG_TEST_XFL_HISTORICAL_DIVIDE_MUTANT)
        while (numerator > denominator) {
#else
        while (numerator >= denominator) {
#endif
            if (totalSubtractions == maxDivideSubtractions)
                return {{}, XFLArithmeticIssue::invalid};
            numerator -= denominator;
            ++coefficient;
            ++totalSubtractions;
        }
        if (coefficient > 18 ||
            quotient > (maxDivideQuotient - coefficient) / 10)
            return {{}, XFLArithmeticIssue::invalid};
        quotient = quotient * 10 + coefficient;
        denominator /= 10;
        if (denominator == 0)
            break;
        --quotientExponent;
    }
    if (denominator != 0)
        return {{}, XFLArithmeticIssue::invalid};

#if defined(CONFIG_TEST_XFL_NEAREST_MUTANT)
    if (quotient != 0)
        ++quotient;
#endif

    return normalizeLive(
        quotient,
        quotientExponent,
        left.is_negative() != right.is_negative());
}

}  // namespace hook
