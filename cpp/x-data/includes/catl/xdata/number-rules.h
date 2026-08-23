#pragma once

#include "catl/core/types.h"

#include <cstdint>
#include <limits>

namespace catl::xdata {

struct NormalizedNumber {
  std::int64_t mantissa = 0;
  std::int32_t exponent = std::numeric_limits<std::int32_t>::min();
};

// Allocation-free, no-throw parity with the pinned Xahau Number constructor.
// STNumber accepts non-normal wire pairs and normalizes them while decoding;
// only overflow is rejected. Underflow and every zero mantissa become the
// canonical zero pair.
struct NumberRules {
  static constexpr std::uint64_t min_mantissa = 1'000'000'000'000'000ULL;
  static constexpr std::uint64_t max_mantissa = 9'999'999'999'999'999ULL;
  static constexpr std::int64_t min_exponent = -32768;
  static constexpr std::int64_t max_exponent = 32768;

  [[nodiscard]] static constexpr std::uint64_t
  read_be64(std::uint8_t const *input) noexcept {
    std::uint64_t value = 0;
    for (std::uint32_t i = 0; i < 8; ++i)
      value = (value << 8) | input[i];
    return value;
  }

  [[nodiscard]] static constexpr std::uint32_t
  read_be32(std::uint8_t const *input) noexcept {
    std::uint32_t value = 0;
    for (std::uint32_t i = 0; i < 4; ++i)
      value = (value << 8) | input[i];
    return value;
  }

  [[nodiscard]] static constexpr bool
  normalize(std::int64_t mantissa, std::int32_t exponent,
            NormalizedNumber &output) noexcept {
    if (mantissa == 0) {
      output = {};
      return true;
    }

    bool const negative = mantissa < 0;
    auto const raw = static_cast<std::uint64_t>(mantissa);
    std::uint64_t magnitude = negative ? std::uint64_t{0} - raw : raw;
    std::int64_t normalized_exponent = exponent;
    while (magnitude < min_mantissa && normalized_exponent > min_exponent) {
      magnitude *= 10;
      --normalized_exponent;
    }

    // Number::Guard stores the most recently discarded decimal digit in
    // its high nibble and remembers any non-zero digit shifted away.
    std::uint64_t guard_digits = 0;
    bool extra_nonzero = false;
    while (magnitude > max_mantissa) {
      if (normalized_exponent >= max_exponent)
        return false;
      extra_nonzero = extra_nonzero || (guard_digits & 0x0f) != 0;
      guard_digits >>= 4;
      guard_digits |= (magnitude % 10) << 60;
      magnitude /= 10;
      ++normalized_exponent;
    }

    if (normalized_exponent < min_exponent || magnitude < min_mantissa) {
      output = {};
      return true;
    }

    constexpr std::uint64_t half = 0x5000'0000'0000'0000ULL;
    bool const round_up =
        guard_digits > half ||
        (guard_digits == half && (extra_nonzero || (magnitude & 1) != 0));
    if (round_up) {
      ++magnitude;
      if (magnitude > max_mantissa) {
        magnitude /= 10;
        ++normalized_exponent;
      }
    }
    if (normalized_exponent > max_exponent)
      return false;

    output.mantissa = negative ? -static_cast<std::int64_t>(magnitude)
                               : static_cast<std::int64_t>(magnitude);
    output.exponent = static_cast<std::int32_t>(normalized_exponent);
    return true;
  }

  [[nodiscard]] static constexpr bool
  normalize(Slice payload, NormalizedNumber &output) noexcept {
    if (payload.size() != 12)
      return false;
    auto const raw_mantissa = read_be64(payload.data());
    std::int64_t mantissa = 0;
    if (raw_mantissa <= std::uint64_t{std::numeric_limits<std::int64_t>::max()})
      mantissa = static_cast<std::int64_t>(raw_mantissa);
    else if (raw_mantissa == (std::uint64_t{1} << 63))
      mantissa = std::numeric_limits<std::int64_t>::min();
    else
      mantissa = -static_cast<std::int64_t>(std::uint64_t{0} - raw_mantissa);
    auto const exponent =
        static_cast<std::int32_t>(read_be32(payload.data() + 8));
    return normalize(mantissa, exponent, output);
  }

  [[nodiscard]] static constexpr bool certify(Slice payload) noexcept {
    NormalizedNumber ignored;
    return normalize(payload, ignored);
  }
};

} // namespace catl::xdata
