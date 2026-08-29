#pragma once

#include <cstdint>
#include <cstring>
#include <limits>

namespace catl::xdata {

// The single no-throw XRPL framing primitive used by fixed-memory provider
// paths. A measuring writer counts exactly the bytes a writing writer emits;
// neither owns input, allocates memory, or knows about objects/transactions.
class CanonicalWriter {
public:
  [[nodiscard]] static CanonicalWriter measuring() noexcept {
    return {nullptr, std::numeric_limits<std::uint32_t>::max()};
  }

  CanonicalWriter(std::uint8_t *output, std::uint32_t capacity) noexcept
      : output_(output), capacity_(capacity) {}

  [[nodiscard]] bool byte(std::uint8_t value) noexcept {
    if (remaining() == 0 || (!is_measuring() && output_ == nullptr))
      return false;
    if (!is_measuring())
      output_[position_] = value;
    ++position_;
    return true;
  }

  [[nodiscard]] bool bytes(std::uint8_t const *data,
                           std::uint32_t size) noexcept {
    if (size > remaining() || (size != 0 && data == nullptr) ||
        (size != 0 && !is_measuring() && output_ == nullptr))
      return false;
    if (size != 0 && !is_measuring())
      std::memcpy(output_ + position_, data, size);
    position_ += size;
    return true;
  }

  [[nodiscard]] bool be32(std::uint32_t value) noexcept {
    return byte(static_cast<std::uint8_t>(value >> 24)) &&
           byte(static_cast<std::uint8_t>(value >> 16)) &&
           byte(static_cast<std::uint8_t>(value >> 8)) &&
           byte(static_cast<std::uint8_t>(value));
  }

  [[nodiscard]] bool be64(std::uint64_t value) noexcept {
    for (std::uint32_t shift = 56;; shift -= 8) {
      if (!byte(static_cast<std::uint8_t>(value >> shift)))
        return false;
      if (shift == 0)
        return true;
    }
  }

  [[nodiscard]] bool field_header(std::uint32_t field_code) noexcept {
    auto const type = static_cast<std::uint16_t>(field_code >> 16);
    auto const nth = static_cast<std::uint16_t>(field_code);
    if (type == 0 || nth == 0 || type > 255 || nth > 255)
      return false;
    if (type < 16 && nth < 16)
      return byte(static_cast<std::uint8_t>((type << 4) | nth));
    if (type >= 16 && nth < 16)
      return byte(static_cast<std::uint8_t>(nth)) &&
             byte(static_cast<std::uint8_t>(type));
    if (type < 16)
      return byte(static_cast<std::uint8_t>(type << 4)) &&
             byte(static_cast<std::uint8_t>(nth));
    return byte(0) && byte(static_cast<std::uint8_t>(type)) &&
           byte(static_cast<std::uint8_t>(nth));
  }

  [[nodiscard]] bool vl_prefix(std::uint32_t size) noexcept {
    if (size <= 192)
      return byte(static_cast<std::uint8_t>(size));
    if (size <= 12'480) {
      auto const adjusted = size - 193;
      return byte(static_cast<std::uint8_t>(193 + (adjusted >> 8))) &&
             byte(static_cast<std::uint8_t>(adjusted));
    }
    if (size > 918'744)
      return false;
    auto const adjusted = size - 12'481;
    return byte(static_cast<std::uint8_t>(241 + (adjusted >> 16))) &&
           byte(static_cast<std::uint8_t>(adjusted >> 8)) &&
           byte(static_cast<std::uint8_t>(adjusted));
  }

  [[nodiscard]] std::uint32_t remaining() const noexcept {
    return capacity_ - position_;
  }

  [[nodiscard]] std::uint32_t position() const noexcept { return position_; }

private:
  [[nodiscard]] bool is_measuring() const noexcept {
    return output_ == nullptr &&
           capacity_ == std::numeric_limits<std::uint32_t>::max();
  }

  std::uint8_t *output_ = nullptr;
  std::uint32_t capacity_ = 0;
  std::uint32_t position_ = 0;
};

} // namespace catl::xdata
