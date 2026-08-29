#pragma once

#include "catl/core/types.h"
#include "catl/xdata/recursive_index.h"

#include <cstdint>

namespace catl::xdata {

class CanonicalWriter;

struct CanonicalSizeResult {
  ScanStatus status{};
  std::uint32_t size = 0;

  [[nodiscard]] constexpr bool ok() const noexcept { return status.ok(); }
};

struct CanonicalWriteResult {
  ScanStatus status{};
  std::uint32_t written = 0;

  [[nodiscard]] constexpr bool ok() const noexcept { return status.ok(); }
};

// Measure/write either indexed scope kind. A root scope omits its close
// marker; a nested value includes the canonical kind-specific close marker.
// This is the zero-temporary seam used when replacement receives an already
// certified STObject or STArray value.
[[nodiscard]] CanonicalSizeResult canonical_scope_size(Slice wire,
                                                       RecursiveIndexView index,
                                                       std::uint32_t scope_id,
                                                       bool root) noexcept;

[[nodiscard]] CanonicalWriteResult
canonical_scope_write(Slice wire, RecursiveIndexView index,
                      std::uint32_t scope_id, bool root, std::uint8_t *output,
                      std::uint32_t capacity) noexcept;

// Emit through an existing count-or-write sink. This is the allocation-free
// composition seam for canonical replacement and typed transaction builders.
[[nodiscard]] ScanStatus canonical_scope_emit(Slice wire,
                                              RecursiveIndexView index,
                                              std::uint32_t scope_id, bool root,
                                              CanonicalWriter &writer) noexcept;

// Measure/write a canonical serialized object scope. The selected scope must
// be an object. A root object omits its close marker; a nested value includes
// the canonical ObjectEnd marker.
[[nodiscard]] CanonicalSizeResult
canonical_object_size(Slice wire, RecursiveIndexView index,
                      std::uint32_t scope_id, bool root = true) noexcept;

[[nodiscard]] CanonicalWriteResult
canonical_object_write(Slice wire, RecursiveIndexView index,
                       std::uint32_t scope_id, bool root, std::uint8_t *output,
                       std::uint32_t capacity) noexcept;

// Measure/write the canonical value payload for one indexed field. Field
// headers and VL length prefixes are excluded. Nested container closes are
// included, matching STObject.fieldBytes().
[[nodiscard]] CanonicalSizeResult
canonical_field_value_size(Slice wire, RecursiveIndexView index,
                           FieldRecord const &field) noexcept;

[[nodiscard]] CanonicalWriteResult
canonical_field_value_write(Slice wire, RecursiveIndexView index,
                            FieldRecord const &field, std::uint8_t *output,
                            std::uint32_t capacity) noexcept;

[[nodiscard]] ScanStatus
canonical_field_value_emit(Slice wire, RecursiveIndexView index,
                           FieldRecord const &field,
                           CanonicalWriter &writer) noexcept;

} // namespace catl::xdata
