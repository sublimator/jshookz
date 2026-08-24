#pragma once

#include <cstddef>
#include <cstdint>

struct RecursiveFuelCounts {
  std::uint64_t helper_baseline = 0;
  std::uint64_t helper_certify = 0;
  std::uint64_t helper_index = 0;
  std::uint64_t helper_serialize = 0;
  std::uint64_t wire_passes = 0;
  std::uint64_t scope_entries = 0;
  std::uint64_t field_headers = 0;
  std::uint64_t material_fields = 0;
  std::uint64_t leaf_routes = 0;
  std::uint64_t allocator_calls = 0;
  std::uint64_t allocator_frees = 0;
  std::uint64_t index_bytes = 0;
  std::uint64_t object_lookups = 0;
  std::uint64_t array_resolutions = 0;
  std::uint64_t serializer_sizes = 0;
  std::uint64_t serializer_writes = 0;
  std::uint64_t serializer_bytes = 0;
};

extern "C" {

std::uint64_t recursive_baseline_once_c(std::uint8_t const *bytes,
                                        std::size_t size);
std::uint64_t recursive_certify_once_c(std::uint8_t const *bytes,
                                       std::size_t size);
std::uint64_t recursive_index_once_c(std::uint8_t const *bytes,
                                     std::size_t size);
std::uint64_t recursive_serialize_once_c(std::uint8_t const *bytes,
                                         std::size_t size, void const *index,
                                         std::uint32_t index_size,
                                         std::uint8_t *output,
                                         std::uint32_t capacity);

#if defined(CATL_XDATA_RECURSIVE_FUEL_COUNTS)
void recursive_fuel_counts_reset_c();
void recursive_fuel_counts_read_c(RecursiveFuelCounts *output);
#endif
}
