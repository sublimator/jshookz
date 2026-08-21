#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {

uint64_t view_once_c(void const* idx, size_t ord, int32_t* exp, uint8_t* tag);
uint64_t raw_once_c(uint8_t const* bytes, size_t n, int32_t* exp, uint8_t* tag);
uint64_t mask_once_c(uint8_t const* bytes, size_t n, uint8_t* tag);

}
