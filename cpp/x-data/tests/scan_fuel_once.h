#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {

uint64_t view_once_c(void const* idx, size_t ord, int32_t* exp, uint8_t* tag);
uint64_t raw_once_c(uint8_t const* bytes, size_t n, int32_t* exp, uint8_t* tag);
uint64_t mask_once_c(
    uint8_t const* bytes, size_t n, int32_t* exp, uint8_t* tag);
uint64_t parts_once_c(void const* view, int32_t* exp, uint8_t* tag);
uint64_t retained_parts_c(void const* parts, int32_t* exp, uint8_t* tag);

#if defined(CATL_XDATA_HELPER_CALL_COUNTS)
void helper_counts_reset_c();
void helper_counts_read_c(uint32_t* out);
#endif
}
