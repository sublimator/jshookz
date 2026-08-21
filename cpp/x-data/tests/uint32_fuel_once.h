#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {

uint32_t u32_retained_once_c(uint32_t value);
uint32_t u32_prebound_once_c(void const* view);
uint32_t u32_rebind_once_c(void const* idx, size_t ordinal);
uint32_t u32_raw_once_c(
    uint8_t const* bytes, size_t size, void const* protocol);

#if defined(CATL_XDATA_UINT32_HELPER_CALL_COUNTS)
void u32_helper_counts_reset_c();
void u32_helper_counts_read_c(uint32_t* out);
#endif
}
