#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {

uint32_t account_id_retained_once_c(uint8_t const* normalized);
uint32_t account_id_prebound_once_c(void const* view);
uint32_t account_id_rebind_once_c(void const* idx, size_t ordinal);
uint32_t account_id_raw_once_c(
    uint8_t const* bytes, size_t size, void const* protocol);

#if defined(CATL_XDATA_ACCOUNT_ID_HELPER_CALL_COUNTS)
void account_id_helper_counts_reset_c();
void account_id_helper_counts_read_c(uint32_t* out);
#endif
}
