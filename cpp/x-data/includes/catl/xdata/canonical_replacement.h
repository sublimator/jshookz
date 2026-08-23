#pragma once

#include "catl/core/types.h"
#include "catl/xdata/recursive_index.h"

#include <cstdint>

namespace catl::xdata {

// Replacement always emits a new canonical root object.  Removal alone may
// report no_op when the selected field is absent; callers can then preserve
// wrapper identity without allocating an output buffer.
enum class CanonicalReplacementDisposition : std::uint32_t {
  emitted = 0,
  no_op = 1,
};

struct CanonicalReplacementSizeResult {
  ScanStatus status{};
  std::uint32_t size = 0;
  CanonicalReplacementDisposition disposition =
      CanonicalReplacementDisposition::emitted;

  [[nodiscard]] constexpr bool ok() const noexcept { return status.ok(); }
  [[nodiscard]] constexpr bool no_op() const noexcept {
    return ok() && disposition == CanonicalReplacementDisposition::no_op;
  }
};

struct CanonicalReplacementWriteResult {
  ScanStatus status{};
  std::uint32_t written = 0;
  CanonicalReplacementDisposition disposition =
      CanonicalReplacementDisposition::emitted;

  [[nodiscard]] constexpr bool ok() const noexcept { return status.ok(); }
  [[nodiscard]] constexpr bool no_op() const noexcept {
    return ok() && disposition == CanonicalReplacementDisposition::no_op;
  }
};

static_assert(sizeof(CanonicalReplacementSizeResult) == 24);
static_assert(sizeof(CanonicalReplacementWriteResult) == 24);

// Measure/write a canonical root object in which field_code is inserted or
// replaced. value_payload is an already-certified canonical field value: it
// excludes the field header and VL prefix, while nested container closes are
// included. Even a byte-identical replacement has disposition emitted.
[[nodiscard]] CanonicalReplacementSizeResult canonical_object_with_field_size(
    Slice wire, RecursiveIndexView index, std::uint32_t scope_id,
    std::uint32_t field_code, Slice value_payload) noexcept;

[[nodiscard]] CanonicalReplacementWriteResult canonical_object_with_field_write(
    Slice wire, RecursiveIndexView index, std::uint32_t scope_id,
    std::uint32_t field_code, Slice value_payload, std::uint8_t *output,
    std::uint32_t capacity) noexcept;

// Measure/write a canonical root object with field_code removed. An absent
// field returns success/no_op, size or written zero, and never touches output.
[[nodiscard]] CanonicalReplacementSizeResult
canonical_object_without_field_size(Slice wire, RecursiveIndexView index,
                                    std::uint32_t scope_id,
                                    std::uint32_t field_code) noexcept;

[[nodiscard]] CanonicalReplacementWriteResult
canonical_object_without_field_write(Slice wire, RecursiveIndexView index,
                                     std::uint32_t scope_id,
                                     std::uint32_t field_code,
                                     std::uint8_t *output,
                                     std::uint32_t capacity) noexcept;

} // namespace catl::xdata
