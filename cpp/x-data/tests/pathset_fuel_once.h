#pragma once

#include <cstddef>
#include <cstdint>

// Test-only shape of the compact directory retained by the escaping adapter.
// The payload and directory are immutable for every measured cached-access
// lane; no production ownership or facade type is introduced here.
struct PathSetFuelCache
{
    uint8_t const* payload = nullptr;
    size_t payload_size = 0;
    uint32_t const* directory = nullptr;
    uint32_t paths = 0;
    uint32_t hops = 0;
};

extern "C" {

uint32_t pathset_sequential_once_c(void const* view);
uint32_t pathset_measure_fill_once_c(
    uint8_t const* payload,
    size_t payload_size,
    uint32_t* words,
    size_t word_capacity);
uint32_t pathset_cached_length_once_c(void const* cache, size_t path);
uint32_t pathset_cached_at_once_c(
    void const* cache, size_t path, size_t path_hop);
uint32_t pathset_raw_recertify_once_c(
    uint8_t const* payload,
    size_t payload_size,
    void const* protocol);

#if defined(CATL_XDATA_PATHSET_HELPER_CALL_COUNTS)
void pathset_helper_counts_reset_c();
void pathset_helper_counts_read_c(uint32_t* helpers, uint32_t* routes);
#endif
}
