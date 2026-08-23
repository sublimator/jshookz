#pragma once

#include "catl/core/types.h"

#include <cstddef>
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

  // Allocation-free decimal parser for the public Number string value. It
  // retains sixteen significant digits and applies the same ties-to-even law
  // as normalize(), then emits the already-normalized pair. Nonzero exponent
  // underflow, overflow, and malformed text reject; decimal zero stays zero.
  [[nodiscard]] static bool parse_decimal(char const *text, std::size_t size,
                                          NormalizedNumber &output) noexcept {
    output = {};
    if (text == nullptr || size == 0)
      return false;
    std::size_t position = 0;
    bool negative = false;
    if (text[position] == '-' || text[position] == '+') {
      negative = text[position] == '-';
      if (++position == size)
        return false;
    }

    bool seen_dot = false;
    bool saw_digit = false;
    std::size_t integer_digits = 0;
    char first_integer_digit = '\0';
    bool significant = false;
    std::uint64_t mantissa = 0;
    std::size_t significant_digits = 0;
    std::size_t fractional_digits = 0;
    std::uint8_t first_discarded = 0;
    bool discarded_nonzero = false;
    for (; position < size; ++position) {
      char const current = text[position];
      if (current == 'e' || current == 'E')
        break;
      if (current == '.') {
        if (seen_dot)
          return false;
        seen_dot = true;
        continue;
      }
      if (current < '0' || current > '9')
        return false;
      saw_digit = true;
      if (seen_dot) {
        ++fractional_digits;
      } else {
        if (integer_digits == 0)
          first_integer_digit = current;
        ++integer_digits;
      }
      auto const digit = static_cast<std::uint8_t>(current - '0');
      if (!significant && digit == 0)
        continue;
      significant = true;
      ++significant_digits;
      if (significant_digits <= 16) {
        mantissa = mantissa * 10 + digit;
      } else if (significant_digits == 17) {
        first_discarded = digit;
      } else if (digit != 0) {
        discarded_nonzero = true;
      }
    }
    if (!saw_digit || integer_digits == 0 ||
        (integer_digits > 1 && first_integer_digit == '0') ||
        (seen_dot && fractional_digits == 0))
      return false;

    std::int64_t explicit_exponent = 0;
    if (position < size) {
      ++position;
      if (position == size)
        return false;
      bool exponent_negative = false;
      if (text[position] == '-' || text[position] == '+') {
        exponent_negative = text[position] == '-';
        if (++position == size)
          return false;
      }
      bool exponent_digit = false;
      std::int64_t magnitude = 0;
      for (; position < size; ++position) {
        char const current = text[position];
        if (current < '0' || current > '9')
          return false;
        exponent_digit = true;
        if (magnitude <= 1'000'000)
          magnitude = magnitude * 10 + (current - '0');
      }
      if (!exponent_digit)
        return false;
      explicit_exponent = exponent_negative ? -magnitude : magnitude;
    }

    if (!significant) {
      output = {};
      return true;
    }
    auto exponent =
        explicit_exponent - static_cast<std::int64_t>(fractional_digits);
    if (significant_digits > 16)
      exponent += static_cast<std::int64_t>(significant_digits - 16);
    while (significant_digits < 16) {
      mantissa *= 10;
      --exponent;
      ++significant_digits;
    }

    if (first_discarded > 5 ||
        (first_discarded == 5 && (discarded_nonzero || (mantissa & 1) != 0))) {
      ++mantissa;
      if (mantissa > max_mantissa) {
        mantissa /= 10;
        ++exponent;
      }
    }
    if (exponent < min_exponent || exponent > max_exponent)
      return false;
    output.mantissa = negative ? -static_cast<std::int64_t>(mantissa)
                               : static_cast<std::int64_t>(mantissa);
    output.exponent = static_cast<std::int32_t>(exponent);
    return true;
  }
};

} // namespace catl::xdata
