#include "recursive_fuel_once.h"

#include "catl/xdata/canonical_serializer.h"
#include "catl/xdata/recursive_index.h"
#include "catl/xdata/static_protocol.h"

#include <cstdlib>
#include <cstring>

namespace {

volatile std::uint64_t g_recursive_fuel_escape = 0;

#if defined(CATL_XDATA_RECURSIVE_FUEL_COUNTS)
RecursiveFuelCounts g_recursive_fuel_counts{};
#define CATL_RECURSIVE_COUNT(member, amount)                                   \
  (g_recursive_fuel_counts.member += static_cast<std::uint64_t>(amount))
#else
#define CATL_RECURSIVE_COUNT(member, amount) ((void)0)
#endif

void mix_byte(std::uint64_t &value, std::uint8_t byte) noexcept {
  value ^= byte;
  value *= 1099511628211ull;
}

void mix_word(std::uint64_t &value, std::uint32_t word) noexcept {
  mix_byte(value, static_cast<std::uint8_t>(word));
  mix_byte(value, static_cast<std::uint8_t>(word >> 8));
  mix_byte(value, static_cast<std::uint8_t>(word >> 16));
  mix_byte(value, static_cast<std::uint8_t>(word >> 24));
}

void count_scan(catl::xdata::RecursiveScanCounters const &counts) noexcept {
#if defined(CATL_XDATA_RECURSIVE_FUEL_COUNTS)
  CATL_RECURSIVE_COUNT(wire_passes, counts.wire_passes);
  CATL_RECURSIVE_COUNT(scope_entries, counts.scope_entries);
  CATL_RECURSIVE_COUNT(field_headers, counts.field_headers);
  CATL_RECURSIVE_COUNT(material_fields, counts.material_fields);
  CATL_RECURSIVE_COUNT(leaf_routes, counts.leaf_routes);
#else
  (void)counts;
#endif
}

void *counted_reallocate(void *, void *pointer, std::size_t size) noexcept {
  CATL_RECURSIVE_COUNT(allocator_calls, 1);
  return std::realloc(pointer, size);
}

void counted_free(void *, void *pointer) noexcept {
  if (pointer != nullptr) {
    CATL_RECURSIVE_COUNT(allocator_frees, 1);
    std::free(pointer);
  }
}

catl::xdata::RecursiveScanOptions
scan_options(catl::xdata::RecursiveScanCounters &counts) noexcept {
  catl::xdata::RecursiveScanOptions options;
  options.protocol = &catl::xdata::xahau_static_protocol();
  options.counters = &counts;
  return options;
}

std::uint64_t scan_checksum(catl::xdata::RecursiveScanCounters const &counts,
                            std::uint32_t size) noexcept {
  std::uint64_t value = 1469598103934665603ull;
  mix_word(value, size);
  mix_word(value, static_cast<std::uint32_t>(counts.wire_passes));
  mix_word(value, static_cast<std::uint32_t>(counts.scope_entries));
  mix_word(value, static_cast<std::uint32_t>(counts.field_headers));
  mix_word(value, static_cast<std::uint32_t>(counts.material_fields));
  mix_word(value, static_cast<std::uint32_t>(counts.leaf_routes));
  return value == 0 ? 1 : value;
}

} // namespace

extern "C" {

__attribute__((noinline)) std::uint64_t
recursive_baseline_once_c(std::uint8_t const *bytes, std::size_t size) {
  CATL_RECURSIVE_COUNT(helper_baseline, 1);
  if (bytes == nullptr || size == 0) {
    g_recursive_fuel_escape = 0;
    return 0;
  }
  std::uint64_t value = 1469598103934665603ull;
  mix_word(value, static_cast<std::uint32_t>(size));
  mix_byte(value, bytes[0]);
  mix_byte(value, bytes[size / 2]);
  mix_byte(value, bytes[size - 1]);
  g_recursive_fuel_escape = value;
  return value;
}

__attribute__((noinline)) std::uint64_t
recursive_certify_once_c(std::uint8_t const *bytes, std::size_t size) {
  CATL_RECURSIVE_COUNT(helper_certify, 1);
  catl::xdata::RecursiveScanCounters counts;
  auto options = scan_options(counts);
  auto const status =
      catl::xdata::guest_exact_validate_object(Slice{bytes, size}, options);
  count_scan(counts);
  if (!status.ok()) {
    g_recursive_fuel_escape = 0;
    return 0;
  }
#if defined(CATL_XDATA_RECURSIVE_POISON_CERTIFY_RESCAN)
  catl::xdata::RecursiveScanCounters repeated_counts;
  options.counters = &repeated_counts;
  auto const repeated =
      catl::xdata::guest_exact_validate_object(Slice{bytes, size}, options);
  count_scan(repeated_counts);
  if (!repeated.ok()) {
    g_recursive_fuel_escape = 0;
    return 0;
  }
#endif
  auto const value = scan_checksum(counts, static_cast<std::uint32_t>(size));
  g_recursive_fuel_escape = value;
  return value;
}

__attribute__((noinline)) std::uint64_t
recursive_index_once_c(std::uint8_t const *bytes, std::size_t size) {
  CATL_RECURSIVE_COUNT(helper_index, 1);
#if defined(CATL_XDATA_RECURSIVE_POISON_INDEX_RESCAN)
  catl::xdata::RecursiveScanCounters repeated_counts;
  auto repeated_options = scan_options(repeated_counts);
  auto const repeated = catl::xdata::guest_exact_validate_object(
      Slice{bytes, size}, repeated_options);
  count_scan(repeated_counts);
  if (!repeated.ok()) {
    g_recursive_fuel_escape = 0;
    return 0;
  }
#endif
  catl::xdata::RecursiveScanCounters counts;
  auto options = scan_options(counts);
  catl::xdata::ScanAllocator const allocator{nullptr, counted_reallocate,
                                             counted_free};
  auto result = catl::xdata::guest_exact_object_index(Slice{bytes, size},
                                                      options, allocator);
  count_scan(counts);
  if (!result.ok()) {
    g_recursive_fuel_escape = 0;
    return 0;
  }
  CATL_RECURSIVE_COUNT(index_bytes, result.index_size);
  catl::xdata::RecursiveIndexView const index{result.index, result.index_size,
                                              static_cast<std::uint32_t>(size)};
  auto const *header = index.header();
  auto const *flags = index.find_object_field(0, (2u << 16) | 2u);
  CATL_RECURSIVE_COUNT(object_lookups, 1);
  auto const *memos = index.find_object_field(0, (15u << 16) | 9u);
  CATL_RECURSIVE_COUNT(object_lookups, 1);
  auto const *last =
      memos == nullptr
          ? nullptr
          : index.array_element(memos->child_scope,
                                index.scope(memos->child_scope)->field_count() -
                                    1);
  CATL_RECURSIVE_COUNT(array_resolutions, 1);
  std::uint64_t value = scan_checksum(counts, result.index_size);
  mix_word(value, header == nullptr ? 0 : header->scope_count);
  mix_word(value, header == nullptr ? 0 : header->field_count);
  mix_word(value, flags == nullptr ? 0 : flags->header_begin + 1);
  mix_word(value, last == nullptr ? 0 : last->header_begin + 1);
  allocator.free(allocator.opaque, result.index);
  g_recursive_fuel_escape = value;
  return value == 0 ? 1 : value;
}

__attribute__((noinline)) std::uint64_t
recursive_serialize_once_c(std::uint8_t const *bytes, std::size_t size,
                           void const *index_data, std::uint32_t index_size,
                           std::uint8_t *output, std::uint32_t capacity) {
  CATL_RECURSIVE_COUNT(helper_serialize, 1);
  catl::xdata::RecursiveIndexView const index{index_data, index_size,
                                              static_cast<std::uint32_t>(size)};
  auto const measured =
      catl::xdata::canonical_object_size(Slice{bytes, size}, index, 0, true);
  CATL_RECURSIVE_COUNT(serializer_sizes, 1);
  if (!measured.ok() || measured.size > capacity) {
    g_recursive_fuel_escape = 0;
    return 0;
  }
  auto const written = catl::xdata::canonical_object_write(
      Slice{bytes, size}, index, 0, true, output, capacity);
  CATL_RECURSIVE_COUNT(serializer_writes, 1);
  if (!written.ok() || written.written != measured.size) {
    g_recursive_fuel_escape = 0;
    return 0;
  }
  CATL_RECURSIVE_COUNT(serializer_bytes, written.written);
#if defined(CATL_XDATA_RECURSIVE_POISON_SERIALIZER_REPEAT)
  auto const repeated_size =
      catl::xdata::canonical_object_size(Slice{bytes, size}, index, 0, true);
  CATL_RECURSIVE_COUNT(serializer_sizes, 1);
  auto const repeated_write = catl::xdata::canonical_object_write(
      Slice{bytes, size}, index, 0, true, output, capacity);
  CATL_RECURSIVE_COUNT(serializer_writes, 1);
  if (!repeated_size.ok() || !repeated_write.ok() ||
      repeated_size.size != measured.size ||
      repeated_write.written != written.written) {
    g_recursive_fuel_escape = 0;
    return 0;
  }
  CATL_RECURSIVE_COUNT(serializer_bytes, repeated_write.written);
#endif
  std::uint64_t value = 1469598103934665603ull;
  mix_word(value, written.written);
  for (std::uint32_t i = 0; i < written.written; ++i)
    mix_byte(value, output[i]);
  g_recursive_fuel_escape = value;
  return value == 0 ? 1 : value;
}

#if defined(CATL_XDATA_RECURSIVE_FUEL_COUNTS)
void recursive_fuel_counts_reset_c() { g_recursive_fuel_counts = {}; }

void recursive_fuel_counts_read_c(RecursiveFuelCounts *output) {
  if (output != nullptr)
    *output = g_recursive_fuel_counts;
}
#endif

} // extern "C"
