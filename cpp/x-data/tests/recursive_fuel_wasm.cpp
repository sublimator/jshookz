#include "recursive_fuel_once.h"

#include "catl/xdata/recursive_index.h"
#include "catl/xdata/static_protocol.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr char kCounterAbi[] = "recursive-xdata-fuel-v1";
constexpr std::size_t kFixtureCapacity = 4096;
constexpr std::size_t kOutputCapacity = 4096;

struct Fixture {
  char const *name = nullptr;
  std::uint32_t elements = 0;
  std::uint32_t blob_size = 0;
  std::array<std::uint8_t, kFixtureCapacity> bytes{};
  std::uint32_t size = 0;
  void *index = nullptr;
  std::uint32_t index_size = 0;
  std::uint32_t scopes = 0;
  std::uint32_t fields = 0;
  std::uint32_t headers = 0;
  std::uint32_t leaves = 0;

  ~Fixture() { std::free(index); }

  Fixture(Fixture const &) = delete;
  Fixture &operator=(Fixture const &) = delete;
  Fixture() = default;
};

void *plain_reallocate(void *, void *pointer, std::size_t size) noexcept {
  return std::realloc(pointer, size);
}

void plain_free(void *, void *pointer) noexcept { std::free(pointer); }

bool append_byte(Fixture &fixture, std::uint8_t value) noexcept {
  if (fixture.size == fixture.bytes.size())
    return false;
  fixture.bytes[fixture.size++] = value;
  return true;
}

bool append_u32(Fixture &fixture, std::uint32_t value) noexcept {
  return append_byte(fixture, static_cast<std::uint8_t>(value >> 24)) &&
         append_byte(fixture, static_cast<std::uint8_t>(value >> 16)) &&
         append_byte(fixture, static_cast<std::uint8_t>(value >> 8)) &&
         append_byte(fixture, static_cast<std::uint8_t>(value));
}

bool append_vl(Fixture &fixture, std::uint32_t size) noexcept {
  if (size > 192 || !append_byte(fixture, static_cast<std::uint8_t>(size)))
    return false;
  for (std::uint32_t i = 0; i < size; ++i) {
    if (!append_byte(fixture, static_cast<std::uint8_t>(
                                  0x31u + fixture.elements * 7u + i * 13u)))
      return false;
  }
  return true;
}

bool build_fixture(Fixture &fixture, char const *name, std::uint32_t elements,
                   std::uint32_t blob_size) noexcept {
  fixture.name = name;
  fixture.elements = elements;
  fixture.blob_size = blob_size;

  // Deliberately noncanonical object order. Certification retains wire
  // offsets; canonical serialization must sort both root and child objects.
  if (!append_byte(fixture, 0xf9)) // Memos array.
    return false;
  for (std::uint32_t element = 0; element < elements; ++element) {
    if (!append_byte(fixture, 0xea) || // Memo object element.
        !append_byte(fixture, 0x7d) || // MemoData Blob.
        !append_vl(fixture, blob_size) ||
        !append_byte(fixture, 0x24) || // Sequence UInt32.
        !append_u32(fixture, 1000u + element) ||
        !append_byte(fixture, 0x22) || // Flags UInt32.
        !append_u32(fixture, 0xa5000000u + element) ||
        !append_byte(fixture, 0xe1)) // Memo close.
      return false;
  }
  if (!append_byte(fixture, 0xf1) || // Memos close.
      !append_byte(fixture, 0x24) || // Root Sequence.
      !append_u32(fixture, elements) || !append_byte(fixture, 0x22) ||
      !append_u32(fixture, blob_size))
    return false;

  catl::xdata::RecursiveScanCounters counters;
  catl::xdata::RecursiveScanOptions options;
  options.protocol = &catl::xdata::xahau_static_protocol();
  options.counters = &counters;
  auto result = catl::xdata::guest_exact_object_index(
      Slice{fixture.bytes.data(), fixture.size}, options,
      catl::xdata::ScanAllocator{nullptr, plain_reallocate, plain_free});
  if (!result.ok())
    return false;
  fixture.index = result.index;
  fixture.index_size = result.index_size;
  fixture.scopes = static_cast<std::uint32_t>(counters.scope_entries);
  fixture.fields = static_cast<std::uint32_t>(counters.material_fields);
  fixture.headers = static_cast<std::uint32_t>(counters.field_headers);
  fixture.leaves = static_cast<std::uint32_t>(counters.leaf_routes);
  return counters.wire_passes == 1 && result.consumed == fixture.size;
}

void dump_fixture(Fixture const &fixture) {
  std::printf("fixture_hex %s ", fixture.name);
  for (std::uint32_t i = 0; i < fixture.size; ++i)
    std::printf("%02x", fixture.bytes[i]);
  std::putchar('\n');
}

enum class Lane : std::uint8_t { baseline, certify, index, serialize };

std::uint64_t run_once(Fixture const &fixture, Lane lane,
                       std::uint8_t *output) {
  switch (lane) {
  case Lane::baseline:
    return recursive_baseline_once_c(fixture.bytes.data(), fixture.size);
  case Lane::certify:
    return recursive_certify_once_c(fixture.bytes.data(), fixture.size);
  case Lane::index:
    return recursive_index_once_c(fixture.bytes.data(), fixture.size);
  case Lane::serialize:
    return recursive_serialize_once_c(fixture.bytes.data(), fixture.size,
                                      fixture.index, fixture.index_size, output,
                                      kOutputCapacity);
  }
  return 0;
}

bool dispatch(char const *requested, char const *mode, Fixture const &fixture,
              Lane lane, int iterations, std::uint64_t &escape) {
  if (std::strcmp(requested, mode) != 0)
    return false;
  std::array<std::uint8_t, kOutputCapacity> output{};
  auto const expected = run_once(fixture, lane, output.data());
#if defined(CATL_XDATA_RECURSIVE_FUEL_COUNTS)
  recursive_fuel_counts_reset_c();
#endif
  if (expected == 0) {
    std::puts("FAIL expected");
    return true;
  }
  for (int i = 0; i < iterations; ++i) {
    auto const value = run_once(fixture, lane, output.data());
    if (value != expected) {
      std::puts("FAIL result");
      return true;
    }
    escape ^= value + static_cast<std::uint64_t>(i);
  }
  std::printf("coverage fixture=%s elements=%u bytes=%u\n", fixture.name,
              fixture.elements, fixture.size);
  std::puts(mode);
  return true;
}

void print_counts() {
#if defined(CATL_XDATA_RECURSIVE_FUEL_COUNTS)
  RecursiveFuelCounts counts;
  recursive_fuel_counts_read_c(&counts);
  std::printf(
      "helper_counts baseline=%llu certify=%llu index=%llu serialize=%llu\n",
      static_cast<unsigned long long>(counts.helper_baseline),
      static_cast<unsigned long long>(counts.helper_certify),
      static_cast<unsigned long long>(counts.helper_index),
      static_cast<unsigned long long>(counts.helper_serialize));
  std::printf("scan_counts passes=%llu scopes=%llu headers=%llu fields=%llu "
              "leaves=%llu\n",
              static_cast<unsigned long long>(counts.wire_passes),
              static_cast<unsigned long long>(counts.scope_entries),
              static_cast<unsigned long long>(counts.field_headers),
              static_cast<unsigned long long>(counts.material_fields),
              static_cast<unsigned long long>(counts.leaf_routes));
  std::printf("index_counts allocs=%llu frees=%llu bytes=%llu lookups=%llu "
              "array=%llu\n",
              static_cast<unsigned long long>(counts.allocator_calls),
              static_cast<unsigned long long>(counts.allocator_frees),
              static_cast<unsigned long long>(counts.index_bytes),
              static_cast<unsigned long long>(counts.object_lookups),
              static_cast<unsigned long long>(counts.array_resolutions));
  std::printf("serializer_counts sizes=%llu writes=%llu bytes=%llu\n",
              static_cast<unsigned long long>(counts.serializer_sizes),
              static_cast<unsigned long long>(counts.serializer_writes),
              static_cast<unsigned long long>(counts.serializer_bytes));
#endif
}

} // namespace

int main(int argc, char **argv) {
  char const *requested = argc > 1 ? argv[1] : "unknown";
  int iterations = argc > 2 ? std::atoi(argv[2]) : 1024;
  if (iterations < 0)
    iterations = 0;

  Fixture small;
  Fixture large;
  if (!build_fixture(small, "small", 2, 4) ||
      !build_fixture(large, "large", 24, 24)) {
    std::puts("FAIL setup");
    return 0;
  }
  std::printf("counter_abi %s\n", kCounterAbi);
  std::printf("shape small scopes=%u fields=%u headers=%u leaves=%u index=%u\n",
              small.scopes, small.fields, small.headers, small.leaves,
              small.index_size);
  std::printf("shape large scopes=%u fields=%u headers=%u leaves=%u index=%u\n",
              large.scopes, large.fields, large.headers, large.leaves,
              large.index_size);
  if (std::strcmp(requested, "dump_small") == 0) {
    dump_fixture(small);
    return 0;
  }
  if (std::strcmp(requested, "dump_large") == 0) {
    dump_fixture(large);
    return 0;
  }

  std::uint64_t escape = 0;
  bool matched = false;
#define CATL_DISPATCH(bank, operation)                                         \
  matched = dispatch(requested, "recursive_" #bank "_" #operation "_repeat",   \
                     bank, Lane::operation, iterations, escape) ||             \
            matched
  CATL_DISPATCH(small, baseline);
  CATL_DISPATCH(small, certify);
  CATL_DISPATCH(small, index);
  CATL_DISPATCH(small, serialize);
  CATL_DISPATCH(large, baseline);
  CATL_DISPATCH(large, certify);
  CATL_DISPATCH(large, index);
  CATL_DISPATCH(large, serialize);
#undef CATL_DISPATCH
  if (!matched)
    std::puts("FAIL unknown mode");
  print_counts();
  if (escape == 0xffffffffffffffffull)
    std::puts("never");
  return 0;
}
