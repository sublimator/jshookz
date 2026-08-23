#include "catl/xdata/canonical_replacement.h"

#include "catl/xdata/canonical_serializer.h"
#include "catl/xdata/number-rules.h"
#include "catl/xdata/static_protocol.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace catl::xdata {
namespace {

constexpr std::uint32_t kMaximumVl = 918'744;

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

[[nodiscard]] constexpr bool checked_add(std::uint32_t &total,
                                         std::uint32_t part) noexcept {
  if (part > std::numeric_limits<std::uint32_t>::max() - total)
    return false;
  total += part;
  return true;
}

[[nodiscard]] constexpr std::uint32_t
field_header_size(std::uint32_t field_code) noexcept {
  auto const type = static_cast<std::uint16_t>(field_code >> 16);
  auto const nth = static_cast<std::uint16_t>(field_code);
  if (type == 0 || nth == 0 || type > 255 || nth > 255)
    return 0;
  return 1u + (type >= 16 ? 1u : 0u) + (nth >= 16 ? 1u : 0u);
}

[[nodiscard]] constexpr std::uint32_t
vl_prefix_size(std::uint32_t size) noexcept {
  return size <= 192 ? 1 : size <= 12'480 ? 2 : size <= kMaximumVl ? 3 : 0;
}

class Writer {
public:
  Writer(std::uint8_t *output, std::uint32_t capacity) noexcept
      : output_(output), capacity_(capacity) {}

  [[nodiscard]] bool byte(std::uint8_t value) noexcept {
    if (position_ == capacity_ || output_ == nullptr)
      return false;
    output_[position_++] = value;
    return true;
  }

  [[nodiscard]] bool bytes(std::uint8_t const *data,
                           std::uint32_t size) noexcept {
    if (size > remaining() || (size != 0 && data == nullptr) ||
        (size != 0 && output_ == nullptr))
      return false;
    if (size != 0)
      std::memcpy(output_ + position_, data, size);
    position_ += size;
    return true;
  }

  [[nodiscard]] bool field_header(std::uint32_t field_code) noexcept {
    auto const type = static_cast<std::uint16_t>(field_code >> 16);
    auto const nth = static_cast<std::uint16_t>(field_code);
    if (type == 0 || nth == 0 || type > 255 || nth > 255)
      return false;
    if (type < 16 && nth < 16)
      return byte(static_cast<std::uint8_t>((type << 4) | nth));
    if (type >= 16 && nth < 16)
      return byte(static_cast<std::uint8_t>(nth)) &&
             byte(static_cast<std::uint8_t>(type));
    if (type < 16)
      return byte(static_cast<std::uint8_t>(type << 4)) &&
             byte(static_cast<std::uint8_t>(nth));
    return byte(0) && byte(static_cast<std::uint8_t>(type)) &&
           byte(static_cast<std::uint8_t>(nth));
  }

  [[nodiscard]] bool vl_prefix(std::uint32_t size) noexcept {
    if (size <= 192)
      return byte(static_cast<std::uint8_t>(size));
    if (size <= 12'480) {
      auto const adjusted = size - 193;
      return byte(static_cast<std::uint8_t>(193 + (adjusted >> 8))) &&
             byte(static_cast<std::uint8_t>(adjusted));
    }
    if (size > kMaximumVl)
      return false;
    auto const adjusted = size - 12'481;
    return byte(static_cast<std::uint8_t>(241 + (adjusted >> 16))) &&
           byte(static_cast<std::uint8_t>(adjusted >> 8)) &&
           byte(static_cast<std::uint8_t>(adjusted));
  }

  [[nodiscard]] bool be32(std::uint32_t value) noexcept {
    return byte(static_cast<std::uint8_t>(value >> 24)) &&
           byte(static_cast<std::uint8_t>(value >> 16)) &&
           byte(static_cast<std::uint8_t>(value >> 8)) &&
           byte(static_cast<std::uint8_t>(value));
  }

  [[nodiscard]] bool be64(std::uint64_t value) noexcept {
    for (std::uint32_t shift = 56;; shift -= 8) {
      if (!byte(static_cast<std::uint8_t>(value >> shift)))
        return false;
      if (shift == 0)
        return true;
    }
  }

  [[nodiscard]] std::uint32_t remaining() const noexcept {
    return capacity_ - position_;
  }

  [[nodiscard]] std::uint32_t position() const noexcept { return position_; }

private:
  std::uint8_t *output_ = nullptr;
  std::uint32_t capacity_ = 0;
  std::uint32_t position_ = 0;
};

struct ReplacementSerializer {
  Slice wire;
  RecursiveIndexView index;
  std::uint32_t scope_id;
  std::uint32_t field_code;
  Slice payload;
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
    auto const encoded_header_size = field_header_size(field_code);
    if (encoded_header_size == 0 ||
        encoded_header_size != descriptor->header_size) {
      return invalid_index(0, field_code, descriptor->header_size);
    }
    if (operation == Operation::remove)
      return ScanStatus::success();
    if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
      return overflow(0, field_code, std::numeric_limits<std::uint32_t>::max());
    }
    payload_size = static_cast<std::uint32_t>(payload.size());
    if (payload_size != 0 && payload.data() == nullptr) {
      return failure(ScanIssue::malformed_data, ScanMessage::truncated_field, 0,
                     field_code, payload_size);
    }
    if (descriptor->fixed_size != 0 && payload_size != descriptor->fixed_size) {
      return failure(ScanIssue::malformed_data, ScanMessage::truncated_field, 0,
                     field_code, descriptor->fixed_size);
    }
    if ((descriptor->flags & field_vl_encoded) != 0 &&
        vl_prefix_size(payload_size) == 0) {
      return failure(ScanIssue::malformed_data, ScanMessage::invalid_vl, 0,
                     field_code, payload_size);
    }
    return ScanStatus::success();
  }

  [[nodiscard]] ScanStatus
  existing_part(FieldRecord const &field,
                std::uint32_t &part_size) const noexcept {
    part_size = 0;
    auto const *descriptor = protocol.field_by_code(field.field_code);
    auto const encoded_header_size = field_header_size(field.field_code);
    if (descriptor == nullptr ||
        descriptor->material_ordinal == ProtocolView::no_ordinal ||
        encoded_header_size == 0 ||
        encoded_header_size != descriptor->header_size) {
      return invalid_index(field.header_begin, field.field_code);
    }
    auto const value = canonical_field_value_size(wire, index, field);
    if (!value.ok())
      return value.status;
    if (descriptor->materializer == MaterializerKind::number) {
      if (field.payload_begin > field.wire_end || field.wire_end > wire.size())
        return invalid_index(field.header_begin, field.field_code);
      NormalizedNumber normalized;
      if (!NumberRules::normalize(Slice{wire.data() + field.payload_begin,
                                        field.wire_end - field.payload_begin},
                                  normalized)) {
        return failure(ScanIssue::malformed_data, ScanMessage::invalid_number,
                       field.payload_begin, field.field_code);
      }
    }
    part_size = encoded_header_size;
    if ((descriptor->flags & field_vl_encoded) != 0) {
      auto const prefix = vl_prefix_size(value.size);
      if (prefix == 0 || !checked_add(part_size, prefix))
        return overflow(field.payload_begin, field.field_code, value.size);
    }
    if (!checked_add(part_size, value.size))
      return overflow(field.payload_begin, field.field_code, value.size);
    return ScanStatus::success();
  }

  [[nodiscard]] ScanStatus
  indexed_field(FieldRecord const &field,
                StaticFieldDescriptor const *&descriptor,
                std::uint32_t &payload_size) const noexcept {
    descriptor = protocol.field_by_code(field.field_code);
    payload_size = 0;
    auto const encoded_header_size = field_header_size(field.field_code);
    if (descriptor == nullptr ||
        descriptor->material_ordinal == ProtocolView::no_ordinal ||
        encoded_header_size == 0 ||
        encoded_header_size != descriptor->header_size ||
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
  replacement_part(StaticFieldDescriptor const &descriptor,
                   std::uint32_t payload_size,
                   std::uint32_t &part_size) const noexcept {
    part_size = field_header_size(field_code);
    if ((descriptor.flags & field_vl_encoded) != 0) {
      auto const prefix = vl_prefix_size(payload_size);
      if (prefix == 0 || !checked_add(part_size, prefix))
        return overflow(0, field_code, payload_size);
    }
    if (!checked_add(part_size, payload_size))
      return overflow(0, field_code, payload_size);
    return ScanStatus::success();
  }

  [[nodiscard]] CanonicalReplacementSizeResult measure() const noexcept {
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

    std::uint32_t replacement_size = 0;
    if (operation == Operation::replace) {
      status = replacement_part(*replacement, payload_size, replacement_size);
      if (!status.ok())
        return {status, 0, CanonicalReplacementDisposition::emitted};
    }

    std::uint32_t total = 0;
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
        if (!checked_add(total, replacement_size))
          return {overflow(0, field_code, replacement_size), 0,
                  CanonicalReplacementDisposition::emitted};
        inserted = true;
      }
      if (field->field_code == field_code)
        continue;

      std::uint32_t part = 0;
      status = existing_part(*field, part);
      if (!status.ok())
        return {status, 0, CanonicalReplacementDisposition::emitted};
      if (!checked_add(total, part))
        return {overflow(field->header_begin, field->field_code, part), 0,
                CanonicalReplacementDisposition::emitted};
    }
    if (operation == Operation::replace && !inserted &&
        !checked_add(total, replacement_size)) {
      return {overflow(0, field_code, replacement_size), 0,
              CanonicalReplacementDisposition::emitted};
    }
    return {ScanStatus::success(), total,
            CanonicalReplacementDisposition::emitted};
  }

  [[nodiscard]] ScanStatus
  write_replacement(StaticFieldDescriptor const &descriptor,
                    std::uint32_t payload_size, Writer &writer) const noexcept {
    if (!writer.field_header(field_code))
      return invalid_index(0, field_code);
    if ((descriptor.flags & field_vl_encoded) != 0 &&
        !writer.vl_prefix(payload_size))
      return overflow(0, field_code, payload_size);
    if (!writer.bytes(payload.data(), payload_size))
      return overflow(0, field_code, payload_size);
    return ScanStatus::success();
  }

  [[nodiscard]] ScanStatus write_indexed_scope(std::uint32_t selected_scope_id,
                                               Writer &writer) const noexcept {
    auto const *scope = index.scope(selected_scope_id);
    auto const *header = index.header();
    if (scope == nullptr || header == nullptr ||
        scope->first_field > header->field_count ||
        scope->field_count() > header->field_count - scope->first_field)
      return invalid_index(0, 0, selected_scope_id);
    for (std::uint32_t i = 0; i < scope->field_count(); ++i) {
      auto const *field = index.field(scope->first_field + i);
      if (field == nullptr)
        return invalid_index(0, 0, selected_scope_id);
      StaticFieldDescriptor const *descriptor = nullptr;
      std::uint32_t payload_size = 0;
      auto status = indexed_field(*field, descriptor, payload_size);
      if (!status.ok())
        return status;
      if (!writer.field_header(field->field_code))
        return invalid_index(field->header_begin, field->field_code);
      if ((descriptor->flags & field_vl_encoded) != 0 &&
          !writer.vl_prefix(payload_size))
        return overflow(field->payload_begin, field->field_code, payload_size);
      status = write_indexed_value(*field, *descriptor, payload_size, writer);
      if (!status.ok())
        return status;
    }
    auto const close = scope->kind() == ScopeKind::array ? 0xf1 : 0xe1;
    if (!writer.byte(close))
      return overflow(scope->content_end, 0, close);
    return ScanStatus::success();
  }

  [[nodiscard]] ScanStatus write_indexed_value(
      FieldRecord const &field, StaticFieldDescriptor const &descriptor,
      std::uint32_t payload_size, Writer &writer) const noexcept {
    if (field.child_scope != FieldRecord::no_child)
      return write_indexed_scope(field.child_scope, writer);
    auto const *bytes = wire.data() + field.payload_begin;
    if (descriptor.materializer == MaterializerKind::number) {
      NormalizedNumber normalized;
      if (!NumberRules::normalize(Slice{bytes, payload_size}, normalized))
        return failure(ScanIssue::malformed_data, ScanMessage::invalid_number,
                       field.payload_begin, field.field_code);
      if (!writer.be64(static_cast<std::uint64_t>(normalized.mantissa)) ||
          !writer.be32(static_cast<std::uint32_t>(normalized.exponent)))
        return overflow(field.payload_begin, field.field_code, 12);
      return ScanStatus::success();
    }
    if (!writer.bytes(bytes, payload_size))
      return overflow(field.payload_begin, field.field_code, payload_size);
    return ScanStatus::success();
  }

  [[nodiscard]] ScanStatus write_existing(FieldRecord const &field,
                                          Writer &writer) const noexcept {
    StaticFieldDescriptor const *descriptor = nullptr;
    std::uint32_t payload_size = 0;
    auto status = indexed_field(field, descriptor, payload_size);
    if (!status.ok())
      return status;
    if (!writer.field_header(field.field_code))
      return invalid_index(field.header_begin, field.field_code);
    if ((descriptor->flags & field_vl_encoded) != 0 &&
        !writer.vl_prefix(payload_size))
      return overflow(field.payload_begin, field.field_code, payload_size);
    return write_indexed_value(field, *descriptor, payload_size, writer);
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

    Writer writer(output, capacity);
    bool inserted = false;
    for (std::uint32_t i = 0; i < scope->field_count(); ++i) {
      auto const *field = index.field(scope->first_field + i);
      if (field == nullptr)
        return {invalid_index(), 0, CanonicalReplacementDisposition::emitted};
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
    if (writer.position() != measured.size)
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
  return ReplacementSerializer{wire,       index,         scope_id,
                               field_code, value_payload, Operation::replace}
      .measure();
}

CanonicalReplacementWriteResult canonical_object_with_field_write(
    Slice wire, RecursiveIndexView index, std::uint32_t scope_id,
    std::uint32_t field_code, Slice value_payload, std::uint8_t *output,
    std::uint32_t capacity) noexcept {
  return ReplacementSerializer{wire,       index,         scope_id,
                               field_code, value_payload, Operation::replace}
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
