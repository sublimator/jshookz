#include "catl/xdata/recursive_index.h"
#include "catl/xdata/static_protocol.h"

#include "recursive_semantic_comparator_corpus.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {

constexpr char kComparatorAbi[] = "recursive-semantic-comparator-v1";

struct HeapMetrics {
  std::uint64_t calls = 0;
  std::uint64_t frees = 0;
  std::uint64_t requested = 0;
  std::uint64_t live = 0;
  std::uint64_t peak = 0;
};

struct alignas(std::max_align_t) AllocationHeader {
  std::size_t size = 0;
};

static_assert(sizeof(AllocationHeader) % alignof(std::max_align_t) == 0);

void *measured_reallocate(void *opaque, void *pointer,
                          std::size_t size) noexcept {
  auto &metrics = *static_cast<HeapMetrics *>(opaque);
  ++metrics.calls;
  auto *old_header = pointer == nullptr
                         ? nullptr
                         : static_cast<AllocationHeader *>(pointer) - 1;
  std::size_t const old_size = old_header == nullptr ? 0 : old_header->size;
  if (size == 0) {
    if (old_header != nullptr) {
      std::free(old_header);
      ++metrics.frees;
      metrics.live -= old_size;
    }
    return nullptr;
  }
  if (size > std::numeric_limits<std::size_t>::max() - sizeof(AllocationHeader))
    return nullptr;
  auto *replacement = static_cast<AllocationHeader *>(
      std::realloc(old_header, sizeof(AllocationHeader) + size));
  if (replacement == nullptr)
    return nullptr;
  replacement->size = size;
  metrics.requested += size;
  metrics.live = metrics.live - old_size + size;
  if (metrics.live > metrics.peak)
    metrics.peak = metrics.live;
  return replacement + 1;
}

void measured_free(void *opaque, void *pointer) noexcept {
  if (pointer == nullptr)
    return;
  auto &metrics = *static_cast<HeapMetrics *>(opaque);
  auto *header = static_cast<AllocationHeader *>(pointer) - 1;
  ++metrics.frees;
  metrics.live -= header->size;
  std::free(header);
}

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

void mix_bytes(std::uint64_t &value, void const *data,
               std::size_t size) noexcept {
  auto const *bytes = static_cast<std::uint8_t const *>(data);
  for (std::size_t i = 0; i < size; ++i)
    mix_byte(value, bytes[i]);
}

struct RunResult {
  bool ok = true;
  std::uint64_t accepted = 0;
  std::uint64_t checksum = 1469598103934665603ull;
  HeapMetrics heap{};
};

RunResult run_baseline(std::uint32_t iterations) noexcept {
  RunResult result;
  for (std::uint32_t iteration = 0; iteration < iterations; ++iteration) {
    for (auto const &item : comparator_valid_cases) {
      mix_word(result.checksum, item.size);
      mix_bytes(result.checksum, item.bytes, item.size);
      ++result.accepted;
    }
  }
  return result;
}

RunResult run_certify(std::uint32_t iterations) noexcept {
  RunResult result;
  catl::xdata::RecursiveScanOptions options;
  options.protocol = &catl::xdata::xahau_static_protocol();
  for (std::uint32_t iteration = 0; iteration < iterations; ++iteration) {
    for (auto const &item : comparator_valid_cases) {
      auto const status = catl::xdata::guest_exact_validate_object(
          Slice{item.bytes, item.size}, options);
      if (!status.ok()) {
        result.ok = false;
        mix_word(result.checksum, status.message_id);
        continue;
      }
      ++result.accepted;
      mix_word(result.checksum, item.size);
      mix_bytes(result.checksum, item.bytes, item.size);
    }
  }
  return result;
}

RunResult run_index(std::uint32_t iterations) noexcept {
  RunResult result;
  catl::xdata::RecursiveScanOptions options;
  options.protocol = &catl::xdata::xahau_static_protocol();
  catl::xdata::ScanAllocator const allocator{&result.heap, measured_reallocate,
                                             measured_free};
  for (std::uint32_t iteration = 0; iteration < iterations; ++iteration) {
    for (auto const &item : comparator_valid_cases) {
      auto indexed = catl::xdata::guest_exact_object_index(
          Slice{item.bytes, item.size}, options, allocator);
      if (!indexed.ok()) {
        result.ok = false;
        mix_word(result.checksum, indexed.status.message_id);
        continue;
      }
      catl::xdata::RecursiveIndexView const view{indexed.index,
                                                 indexed.index_size, item.size};
      if (!view.structurally_valid())
        result.ok = false;
      ++result.accepted;
      mix_word(result.checksum, indexed.consumed);
      mix_word(result.checksum, indexed.index_size);
      mix_bytes(result.checksum, indexed.index, indexed.index_size);
      allocator.free(allocator.opaque, indexed.index);
    }
  }
  if (result.heap.live != 0)
    result.ok = false;
  return result;
}

RunResult run_control() noexcept {
  RunResult result;
  catl::xdata::RecursiveScanOptions options;
  options.protocol = &catl::xdata::xahau_static_protocol();
  for (auto const &item : comparator_semantic_controls) {
    auto const status = catl::xdata::guest_exact_validate_object(
        Slice{item.bytes, item.size}, options);
    mix_word(result.checksum, status.issue);
    mix_word(result.checksum, status.message_id);
    if (status.ok())
      ++result.accepted;
  }
  return result;
}

} // namespace

int main(int argc, char **argv) {
  char const *mode = argc > 1 ? argv[1] : "unknown";
  unsigned long parsed = argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 1;
  if (parsed > std::numeric_limits<std::uint32_t>::max())
    parsed = 0;
  auto const iterations = static_cast<std::uint32_t>(parsed);

  RunResult result;
  if (std::strcmp(mode, "baseline") == 0)
    result = run_baseline(iterations);
  else if (std::strcmp(mode, "certify") == 0)
    result = run_certify(iterations);
  else if (std::strcmp(mode, "index") == 0)
    result = run_index(iterations);
  else if (std::strcmp(mode, "control") == 0)
    result = run_control();
  else {
    std::fprintf(stderr, "unknown comparator mode: %s\n", mode);
    return 2;
  }

  std::printf("comparator_abi %s\n", kComparatorAbi);
  std::printf(
      "mode %s iterations=%u cases=%zu accepted=%llu checksum=%016llx\n", mode,
      iterations,
      std::strcmp(mode, "control") == 0 ? comparator_semantic_controls.size()
                                        : comparator_valid_cases.size(),
      static_cast<unsigned long long>(result.accepted),
      static_cast<unsigned long long>(result.checksum));
  std::printf("heap calls=%llu frees=%llu requested=%llu peak=%llu live=%llu\n",
              static_cast<unsigned long long>(result.heap.calls),
              static_cast<unsigned long long>(result.heap.frees),
              static_cast<unsigned long long>(result.heap.requested),
              static_cast<unsigned long long>(result.heap.peak),
              static_cast<unsigned long long>(result.heap.live));
  return result.ok ? 0 : 1;
}
