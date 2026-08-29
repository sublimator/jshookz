#include "catl/xdata/canonical_replacement.h"

#include "catl/xdata/canonical_serializer.h"
#include "catl/xdata/canonical_writer.h"
#include "catl/xdata/static_protocol.h"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace catl::xdata {
namespace {

enum class Operation : std::uint8_t {
  replace,
  remove,
};

[[nodiscard]] constexpr ScanStatus failure(ScanIssue issue, ScanMessage message,
                                           std::uint32_t offset = 0,
                                           std::uint32_t field_code = 0,
                                           std::uint32_t aux = 0) noexcept {
  return {static_cast<std::uint16_t>(issue),
          static_cast<std::uint16_t>(message), offset, field_code, aux};
}

[[nodiscard]] constexpr ScanStatus
invalid_index(std::uint32_t offset = 0, std::uint32_t field_code = 0,
              std::uint32_t aux = 0) noexcept {
  return failure(ScanIssue::internal_error, ScanMessage::invalid_index, offset,
                 field_code, aux);
}

[[nodiscard]] constexpr ScanStatus overflow(std::uint32_t offset = 0,
                                            std::uint32_t field_code = 0,
                                            std::uint32_t aux = 0) noexcept {
  return failure(ScanIssue::internal_error, ScanMessage::index_size_overflow,
                 offset, field_code, aux);
}

struct ReplacementValue {
  Slice payload{};
  Slice wire{};
  RecursiveIndexView index{};
  std::uint32_t scope_id = 0;
  bool indexed = false;
};

struct ReplacementSerializer {
  Slice wire;
  RecursiveIndexView index;
  std::uint32_t scope_id;
  std::uint32_t field_code;
  ReplacementValue value;
  Operation operation;
  ProtocolView const &protocol = xahau_static_protocol();

  [[nodiscard]] ScanStatus
  selected_scope(ScopeRecord const *&scope,
                 IndexHeader const *&header) const noexcept {
    scope = nullptr;
    header = index.header();
    if (wire.size() > std::numeric_limits<std::uint32_t>::max() ||
        (wire.size() != 0 && wire.data() == nullptr) || header == nullptr ||
        header->format_version != 1 || header->root_scope != 0 ||
        header->scope_count == 0) {
      return invalid_index(0, 0, scope_id);
    }
    scope = index.scope(scope_id);
    if (scope == nullptr || scope->kind() != ScopeKind::object ||
        scope->close_kind() == ScopeCloseKind::invalid ||
        scope->content_begin > scope->content_end ||
        scope->content_end > wire.size() ||
        scope->first_field > header->field_count ||
        scope->field_count() > header->field_count - scope->first_field) {
      return invalid_index(0, 0, scope_id);
    }
    return ScanStatus::success();
  }

  [[nodiscard]] ScanStatus
  replacement_descriptor(StaticFieldDescriptor const *&descriptor,
                         std::uint32_t &payload_size) const noexcept {
    descriptor = protocol.field_by_code(field_code);
    payload_size = 0;
    if (descriptor == nullptr ||
        descriptor->material_ordinal == ProtocolView::no_ordinal) {
      return failure(ScanIssue::malformed_data, ScanMessage::unknown_field, 0,
                     field_code);
    }
    auto header = CanonicalWriter::measuring();
    if (!header.field_header(field_code) ||
        header.position() != descriptor->header_size) {
      return invalid_index(0, field_code, descriptor->header_size);
    }
    if (operation == Operation::remove)
      return ScanStatus::success();
    if (value.indexed) {
      auto const *scope = value.index.scope(value.scope_id);
      auto const expected_kind =
          descriptor->wire_type == 14 ? ScopeKind::object : ScopeKind::array;
      if ((descriptor->wire_type != 14 && descriptor->wire_type != 15) ||
          (descriptor->flags & field_vl_encoded) != 0 || scope == nullptr ||
          scope->kind() != expected_kind) {
        return failure(ScanIssue::malformed_data,
                       ScanMessage::noncanonical_payload, 0, field_code,
                       value.scope_id);
      }
    } else if (value.payload.size() >
               std::numeric_limits<std::uint32_t>::max()) {
      return overflow(0, field_code, std::numeric_limits<std::uint32_t>::max());
    } else {
      payload_size = static_cast<std::uint32_t>(value.payload.size());
    }
    if (!value.indexed && payload_size != 0 &&
        value.payload.data() == nullptr) {
      return failure(ScanIssue::malformed_data, ScanMessage::truncated_field, 0,
                     field_code, payload_size);
    }
    if (!value.indexed && descriptor->fixed_size != 0 &&
        payload_size != descriptor->fixed_size) {
      return failure(ScanIssue::malformed_data, ScanMessage::truncated_field, 0,
                     field_code, descriptor->fixed_size);
    }
    if (!value.indexed && (descriptor->flags & field_vl_encoded) != 0) {
      auto prefix = CanonicalWriter::measuring();
      if (!prefix.vl_prefix(payload_size))
        return failure(ScanIssue::malformed_data, ScanMessage::invalid_vl, 0,
                       field_code, payload_size);
    }
    return ScanStatus::success();
  }

  [[nodiscard]] ScanStatus
  indexed_field(FieldRecord const &field,
                StaticFieldDescriptor const *&descriptor,
                std::uint32_t &payload_size) const noexcept {
    descriptor = protocol.field_by_code(field.field_code);
    payload_size = 0;
    auto encoded_header = CanonicalWriter::measuring();
    if (descriptor == nullptr ||
        descriptor->material_ordinal == ProtocolView::no_ordinal ||
        !encoded_header.field_header(field.field_code) ||
        encoded_header.position() != descriptor->header_size ||
        field.payload_begin > field.wire_end || field.wire_end > wire.size() ||
        field.header_begin >= field.payload_begin) {
      return invalid_index(field.header_begin, field.field_code);
    }
    payload_size = field.wire_end - field.payload_begin;
    bool const container =
        descriptor->wire_type == 14 || descriptor->wire_type == 15;
    if (field.child_scope == FieldRecord::no_child) {
      if (container)
        return invalid_index(field.header_begin, field.field_code);
      return ScanStatus::success();
    }
    auto const *child = index.scope(field.child_scope);
    if (!container || child == nullptr ||
        (descriptor->flags & field_vl_encoded) != 0 ||
        child->content_begin != field.payload_begin ||
        child->content_end > field.wire_end ||
        child->kind() != (descriptor->wire_type == 14 ? ScopeKind::object
                                                      : ScopeKind::array)) {
      return invalid_index(field.header_begin, field.field_code,
                           field.child_scope);
    }
    if (child->close_kind() == ScopeCloseKind::eof) {
      if (child->content_end != field.wire_end || field.wire_end != wire.size())
        return invalid_index(field.header_begin, field.field_code,
                             field.child_scope);
    } else if (child->content_end ==
                   std::numeric_limits<std::uint32_t>::max() ||
               child->content_end + 1 != field.wire_end) {
      return invalid_index(field.header_begin, field.field_code,
                           field.child_scope);
    }
    return ScanStatus::success();
  }

  [[nodiscard]] ScanStatus
  write_replacement(StaticFieldDescriptor const &descriptor,
                    std::uint32_t payload_size,
                    CanonicalWriter &writer) const noexcept {
    if (!writer.field_header(field_code))
      return invalid_index(0, field_code);
    if ((descriptor.flags & field_vl_encoded) != 0 &&
        !writer.vl_prefix(payload_size))
      return overflow(0, field_code, payload_size);
    if (value.indexed) {
      auto const status = canonical_scope_emit(value.wire, value.index,
                                               value.scope_id, false, writer);
      if (!status.ok())
        return status;
    } else if (!writer.bytes(value.payload.data(), payload_size)) {
      return overflow(0, field_code, payload_size);
    }
    return ScanStatus::success();
  }

  [[nodiscard]] ScanStatus
  write_existing(FieldRecord const &field,
                 CanonicalWriter &writer) const noexcept {
    StaticFieldDescriptor const *descriptor = nullptr;
    std::uint32_t payload_size = 0;
    auto status = indexed_field(field, descriptor, payload_size);
    if (!status.ok())
      return status;
    if (!writer.field_header(field.field_code))
      return invalid_index(field.header_begin, field.field_code);
    if ((descriptor->flags & field_vl_encoded) != 0) {
      auto const value_size =
          descriptor->materializer == MaterializerKind::number ? 12
                                                               : payload_size;
      if (!writer.vl_prefix(value_size))
        return overflow(field.payload_begin, field.field_code, value_size);
    }
    return canonical_field_value_emit(wire, index, field, writer);
  }

  [[nodiscard]] CanonicalReplacementWriteResult
  emit(CanonicalWriter &writer) const noexcept {
    StaticFieldDescriptor const *replacement = nullptr;
    std::uint32_t payload_size = 0;
    auto status = replacement_descriptor(replacement, payload_size);
    if (!status.ok())
      return {status, 0, CanonicalReplacementDisposition::emitted};

    ScopeRecord const *scope = nullptr;
    IndexHeader const *header = nullptr;
    status = selected_scope(scope, header);
    if (!status.ok())
      return {status, 0, CanonicalReplacementDisposition::emitted};

    if (operation == Operation::remove &&
        index.find_object_field(scope_id, field_code) == nullptr) {
      return {ScanStatus::success(), 0, CanonicalReplacementDisposition::no_op};
    }

    std::uint32_t previous_code = 0;
    bool inserted = false;
    for (std::uint32_t i = 0; i < scope->field_count(); ++i) {
      auto const *field = index.field(scope->first_field + i);
      if (field == nullptr || (i != 0 && field->field_code <= previous_code)) {
        return {invalid_index(field == nullptr ? 0 : field->header_begin,
                              field == nullptr ? 0 : field->field_code,
                              scope_id),
                0, CanonicalReplacementDisposition::emitted};
      }
      previous_code = field->field_code;

      if (operation == Operation::replace && !inserted &&
          field->field_code >= field_code) {
        status = write_replacement(*replacement, payload_size, writer);
        if (!status.ok())
          return {status, 0, CanonicalReplacementDisposition::emitted};
        inserted = true;
      }
      if (field->field_code == field_code)
        continue;
      status = write_existing(*field, writer);
      if (!status.ok())
        return {status, 0, CanonicalReplacementDisposition::emitted};
    }
    if (operation == Operation::replace && !inserted) {
      status = write_replacement(*replacement, payload_size, writer);
      if (!status.ok())
        return {status, 0, CanonicalReplacementDisposition::emitted};
    }
    return {ScanStatus::success(), writer.position(),
            CanonicalReplacementDisposition::emitted};
  }

  [[nodiscard]] CanonicalReplacementSizeResult measure() const noexcept {
    auto writer = CanonicalWriter::measuring();
    auto const result = emit(writer);
    return {result.status, result.ok() ? writer.position() : 0,
            result.disposition};
  }

  [[nodiscard]] CanonicalReplacementWriteResult
  write(std::uint8_t *output, std::uint32_t capacity) const noexcept {
    auto const measured = measure();
    if (!measured.ok() || measured.no_op())
      return {measured.status, 0, measured.disposition};
    if (capacity < measured.size || (measured.size != 0 && output == nullptr)) {
      return {overflow(0, field_code, measured.size), 0,
              CanonicalReplacementDisposition::emitted};
    }

    CanonicalWriter writer(output, capacity);
    auto const result = emit(writer);
    if (!result.ok())
      return result;
    if (result.disposition != measured.disposition ||
        writer.position() != measured.size)
      return {invalid_index(0, field_code, writer.position()), 0,
              CanonicalReplacementDisposition::emitted};
    return {ScanStatus::success(), writer.position(),
            CanonicalReplacementDisposition::emitted};
  }
};

} // namespace

CanonicalReplacementSizeResult canonical_object_with_field_size(
    Slice wire, RecursiveIndexView index, std::uint32_t scope_id,
    std::uint32_t field_code, Slice value_payload) noexcept {
  return ReplacementSerializer{
      wire,
      index,
      scope_id,
      field_code,
      ReplacementValue{value_payload, {}, {}, 0, false},
      Operation::replace}
      .measure();
}

CanonicalReplacementWriteResult canonical_object_with_field_write(
    Slice wire, RecursiveIndexView index, std::uint32_t scope_id,
    std::uint32_t field_code, Slice value_payload, std::uint8_t *output,
    std::uint32_t capacity) noexcept {
  return ReplacementSerializer{
      wire,
      index,
      scope_id,
      field_code,
      ReplacementValue{value_payload, {}, {}, 0, false},
      Operation::replace}
      .write(output, capacity);
}

CanonicalReplacementSizeResult canonical_object_with_indexed_field_size(
    Slice wire, RecursiveIndexView index, std::uint32_t scope_id,
    std::uint32_t field_code, Slice value_wire, RecursiveIndexView value_index,
    std::uint32_t value_scope_id) noexcept {
  return ReplacementSerializer{
      wire,
      index,
      scope_id,
      field_code,
      ReplacementValue{{}, value_wire, value_index, value_scope_id, true},
      Operation::replace}
      .measure();
}

CanonicalReplacementWriteResult canonical_object_with_indexed_field_write(
    Slice wire, RecursiveIndexView index, std::uint32_t scope_id,
    std::uint32_t field_code, Slice value_wire, RecursiveIndexView value_index,
    std::uint32_t value_scope_id, std::uint8_t *output,
    std::uint32_t capacity) noexcept {
  return ReplacementSerializer{
      wire,
      index,
      scope_id,
      field_code,
      ReplacementValue{{}, value_wire, value_index, value_scope_id, true},
      Operation::replace}
      .write(output, capacity);
}

CanonicalReplacementSizeResult
canonical_object_without_field_size(Slice wire, RecursiveIndexView index,
                                    std::uint32_t scope_id,
                                    std::uint32_t field_code) noexcept {
  return ReplacementSerializer{wire,       index, scope_id,
                               field_code, {},    Operation::remove}
      .measure();
}

CanonicalReplacementWriteResult canonical_object_without_field_write(
    Slice wire, RecursiveIndexView index, std::uint32_t scope_id,
    std::uint32_t field_code, std::uint8_t *output,
    std::uint32_t capacity) noexcept {
  return ReplacementSerializer{wire,       index, scope_id,
                               field_code, {},    Operation::remove}
      .write(output, capacity);
}

} // namespace catl::xdata
