#pragma once

#include "catl/core/types.h"
#include "catl/xdata/static_protocol.h"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace catl::xdata {

enum class ScanIssue : std::uint16_t {
  none = 0,
  malformed_data,
  resource_limit,
  out_of_memory,
  internal_error,
};

enum class ScanMessage : std::uint16_t {
  none = 0,
  begin_past_end,
  input_too_large,
  truncated_field_header,
  noncanonical_type_code,
  noncanonical_field_code,
  unknown_field,
  illegal_terminator,
  non_object_array_element,
  too_many_nops,
  nesting_too_deep,
  too_many_fields,
  too_many_scopes,
  truncated_vl,
  invalid_vl,
  truncated_field,
  invalid_account_id,
  invalid_amount,
  invalid_number,
  invalid_pathset,
  invalid_vector256,
  invalid_issue,
  invalid_xchain_bridge,
  duplicate_field,
  trailing_bytes,
  allocation_failed,
  index_size_overflow,
  invalid_index,
};

struct ScanStatus {
  std::uint16_t issue;
  std::uint16_t message_id;
  std::uint32_t offset;
  std::uint32_t field_code;
  std::uint32_t aux;

  [[nodiscard]] constexpr bool ok() const noexcept {
    return issue == static_cast<std::uint16_t>(ScanIssue::none);
  }

  [[nodiscard]] static constexpr ScanStatus success() noexcept { return {}; }
};

static_assert(sizeof(ScanStatus) == 16);

[[nodiscard]] char const *
scan_message_literal(std::uint16_t message_id) noexcept;

enum class ScopeKind : std::uint8_t {
  object = 0,
  array = 1,
};

enum class ScopeCloseKind : std::uint8_t {
  eof = 0,
  object_end = 1,
  array_end = 2,
  invalid = 3,
};

// ScanStatus::aux meaning for ScanMessage::illegal_terminator. This preserves
// the current scope expectation without retaining a parser frame or pointer.
enum class ExpectedTerminator : std::uint32_t {
  object_end = 1,
  array_end = 2,
  root_eof = 3,
};

struct IndexHeader {
  std::uint32_t format_version;
  std::uint32_t root_scope;
  std::uint32_t scope_count;
  std::uint32_t field_count;
};

struct ScopeRecord {
  static constexpr std::uint32_t array_mask = 1u << 31;
  static constexpr std::uint32_t close_shift = 29;
  static constexpr std::uint32_t close_mask = 3u << close_shift;
  static constexpr std::uint32_t count_mask = (1u << close_shift) - 1;

  std::uint32_t content_begin;
  std::uint32_t content_end;
  std::uint32_t first_field;
  std::uint32_t count_kind_close;

  [[nodiscard]] constexpr std::uint32_t field_count() const noexcept {
    return count_kind_close & count_mask;
  }

  [[nodiscard]] constexpr ScopeKind kind() const noexcept {
    return (count_kind_close & array_mask) != 0 ? ScopeKind::array
                                                : ScopeKind::object;
  }

  [[nodiscard]] constexpr ScopeCloseKind close_kind() const noexcept {
    return static_cast<ScopeCloseKind>((count_kind_close & close_mask) >>
                                       close_shift);
  }

  [[nodiscard]] static constexpr std::uint32_t
  pack(std::uint32_t count, ScopeKind kind, ScopeCloseKind close) noexcept {
    return count | (kind == ScopeKind::array ? array_mask : std::uint32_t{0}) |
           (static_cast<std::uint32_t>(close) << close_shift);
  }
};

struct FieldRecord {
  static constexpr std::uint32_t no_child =
      std::numeric_limits<std::uint32_t>::max();

  std::uint32_t field_code;
  std::uint32_t header_begin;
  std::uint32_t payload_begin;
  std::uint32_t wire_end;
  std::uint32_t child_scope;
};

static_assert(sizeof(IndexHeader) == 16);
static_assert(sizeof(ScopeRecord) == 16);
static_assert(sizeof(FieldRecord) == 20);

struct RecursiveScanLimits {
  std::uint32_t max_bytes = 1'048'576;
  std::uint32_t max_fields = 32'768;
  std::uint32_t max_scopes = 32'769;
  std::uint32_t max_depth = 10;
};

struct RecursiveScanCounters {
  std::uint64_t wire_passes = 0;
  std::uint64_t scope_entries = 0;
  std::uint64_t field_headers = 0;
  std::uint64_t material_fields = 0;
  std::uint64_t leaf_routes = 0;
};

using ScanRealloc = void *(*)(void *opaque, void *pointer,
                              std::size_t size) noexcept;
using ScanFree = void (*)(void *opaque, void *pointer) noexcept;

struct ScanAllocator {
  void *opaque = nullptr;
  ScanRealloc realloc = nullptr;
  ScanFree free = nullptr;

  [[nodiscard]] constexpr bool valid() const noexcept {
    return realloc != nullptr && free != nullptr;
  }
};

struct RecursiveScanOptions {
  ProtocolView const *protocol = nullptr;
  RecursiveScanLimits limits{};
  RecursiveScanCounters *counters = nullptr;
};

struct ConstructorParityResult {
  ScanStatus status{};
  std::uint32_t consumed = 0;
  std::uint32_t scope_count = 0;
  std::uint32_t field_count = 0;
};

struct IndexBuildResult {
  ScanStatus status{};
  void *index = nullptr;
  std::uint32_t index_size = 0;
  std::uint32_t consumed = 0;

  [[nodiscard]] constexpr bool ok() const noexcept { return status.ok(); }
};

[[nodiscard]] bool recursive_index_size(std::uint32_t scope_count,
                                        std::uint32_t field_count,
                                        std::uint32_t &size) noexcept;

class RecursiveIndexView {
public:
  RecursiveIndexView() = default;
  RecursiveIndexView(void const *data, std::uint32_t size,
                     std::uint32_t wire_size) noexcept
      : data_(static_cast<std::uint8_t const *>(data)), size_(size),
        wire_size_(wire_size) {}

  [[nodiscard]] IndexHeader const *header() const noexcept;

  [[nodiscard]] ScopeRecord const *scope(std::uint32_t scope_id) const noexcept;

  [[nodiscard]] FieldRecord const *
  field(std::uint32_t field_ordinal) const noexcept;

  [[nodiscard]] FieldRecord const *
  find_object_field(std::uint32_t scope_id,
                    std::uint32_t field_code) const noexcept;

  [[nodiscard]] FieldRecord const *
  array_element(std::uint32_t scope_id, std::uint32_t index) const noexcept;

  [[nodiscard]] bool structurally_valid() const noexcept;

private:
  std::uint8_t const *data_ = nullptr;
  std::uint32_t size_ = 0;
  std::uint32_t wire_size_ = 0;
};

// Allocation-free constructor parity. It may stop after a root ObjectEnd and
// report a consumed prefix. Guest-exact validation additionally requires the
// consumed prefix to equal the physical input size.
[[nodiscard]] ConstructorParityResult
constructor_parity_scan(Slice bytes, std::uint32_t begin,
                        RecursiveScanOptions const &options) noexcept;

[[nodiscard]] ScanStatus
guest_exact_validate_object(Slice bytes,
                            RecursiveScanOptions const &options) noexcept;

// Indexed forms allocate scratch through the supplied provider allocator and
// publish one exact 16 + 16*S + 20*F index block only after certification.
[[nodiscard]] IndexBuildResult
constructor_parity_index(Slice bytes, std::uint32_t begin,
                         RecursiveScanOptions const &options,
                         ScanAllocator allocator) noexcept;

[[nodiscard]] IndexBuildResult
guest_exact_object_index(Slice bytes, RecursiveScanOptions const &options,
                         ScanAllocator allocator) noexcept;

} // namespace catl::xdata
