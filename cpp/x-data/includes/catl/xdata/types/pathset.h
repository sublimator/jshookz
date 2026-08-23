#pragma once

#include <cstdint>

namespace catl::xdata {

// PathSet protocol constants
namespace PathSet {
constexpr uint8_t END_BYTE = 0x00;
constexpr uint8_t PATH_SEPARATOR = 0xFF;
constexpr uint8_t TYPE_ACCOUNT = 0x01;
constexpr uint8_t TYPE_CURRENCY = 0x10;
constexpr uint8_t TYPE_ISSUER = 0x20;
}  // namespace PathSet

}  // namespace catl::xdata
