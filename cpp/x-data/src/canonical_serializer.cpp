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

[[nodiscard]] constexpr ScanStatus
failure(ScanMessage message, std::uint32_t offset = 0,
        std::uint32_t field_code = 0,
        std::uint32_t aux = 0) noexcept {
  return {static_cast<std::uint16_t>(ScanIssue::internal_error),
          static_cast<std::uint16_t>(message), offset, field_code, aux};
}

[[nodiscard]] constexpr bool
checked_add(std::uint32_t &total, std::uint32_t part) noexcept {
  if (part > std::numeric_limits<std::uint32_t>::max() - total)
    return false;
  total += part;
  return true;
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
    if (size > capacity_ - position_ || (size != 0 && data == nullptr) ||
        (size != 0 && output_ == nullptr))
      return false;
    if (size != 0)
      std::memcpy(output_ + position_, data, size);
    position_ += size;
    return true;
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

  [[nodiscard]] std::uint32_t position() const noexcept { return position_; }

private:
  std::uint8_t *output_ = nullptr;
  std::uint32_t capacity_ = 0;
  std::uint32_t position_ = 0;
};

struct Serializer {
  Slice wire;
  RecursiveIndexView index;
  ProtocolView const &protocol = xahau_static_protocol();

  [[nodiscard]] bool valid_field(FieldRecord const &field,
                                 StaticFieldDescriptor const *&descriptor,
                                 std::uint32_t &payload_size) const noexcept {
    descriptor = protocol.field_by_code(field.field_code);
    if (descriptor == nullptr ||
        descriptor->material_ordinal == ProtocolView::no_ordinal ||
        field.payload_begin > field.wire_end || field.wire_end > wire.size() ||
        field.header_begin >= field.payload_begin)
      return false;
    payload_size = field.wire_end - field.payload_begin;
    return true;
  }

  [[nodiscard]] CanonicalSizeResult
  value_size(FieldRecord const &field) const noexcept {
    StaticFieldDescriptor const *descriptor = nullptr;
    std::uint32_t payload_size = 0;
    if (!valid_field(field, descriptor, payload_size))
      return {failure(ScanMessage::invalid_index, field.header_begin,
                      field.field_code), 0};
    if (field.child_scope != FieldRecord::no_child) {
      auto const *scope = index.scope(field.child_scope);
      if (scope == nullptr)
        return {failure(ScanMessage::invalid_index, field.header_begin,
                        field.field_code), 0};
      return scope_size(field.child_scope, false);
    }
    if (descriptor->materializer == MaterializerKind::number)
      return {ScanStatus::success(), 12};
    return {ScanStatus::success(), payload_size};
  }

  [[nodiscard]] CanonicalSizeResult
  scope_size(std::uint32_t scope_id, bool root) const noexcept {
    auto const *scope = index.scope(scope_id);
    auto const *header = index.header();
    if (scope == nullptr || header == nullptr ||
        scope->first_field > header->field_count ||
        scope->field_count() > header->field_count - scope->first_field)
      return {failure(ScanMessage::invalid_index), 0};

    std::uint32_t total = 0;
    for (std::uint32_t i = 0; i < scope->field_count(); ++i) {
      auto const *field = index.field(scope->first_field + i);
      if (field == nullptr)
        return {failure(ScanMessage::invalid_index), 0};
      auto const *descriptor = protocol.field_by_code(field->field_code);
      auto const value = value_size(*field);
      if (descriptor == nullptr || !value.ok())
        return {value.ok() ? failure(ScanMessage::invalid_index,
                                    field->header_begin, field->field_code)
                           : value.status,
                0};
      std::uint32_t part = descriptor->header_size;
      if ((descriptor->flags & field_vl_encoded) != 0) {
        auto const prefix = vl_prefix_size(value.size);
        if (prefix == 0 || !checked_add(part, prefix))
          return {failure(ScanMessage::index_size_overflow,
                          field->header_begin, field->field_code,
                          value.size),
                  0};
      }
      if (!checked_add(part, value.size) || !checked_add(total, part))
        return {failure(ScanMessage::index_size_overflow,
                        field->header_begin, field->field_code),
                0};
    }
    if (!root && !checked_add(total, 1))
      return {failure(ScanMessage::index_size_overflow), 0};
    return {ScanStatus::success(), total};
  }

  [[nodiscard]] ScanStatus write_value(FieldRecord const &field,
                                       Writer &writer) const noexcept {
    StaticFieldDescriptor const *descriptor = nullptr;
    std::uint32_t payload_size = 0;
    if (!valid_field(field, descriptor, payload_size))
      return failure(ScanMessage::invalid_index, field.header_begin,
                     field.field_code);
    if (field.child_scope != FieldRecord::no_child)
      return write_scope(field.child_scope, false, writer);
    auto const *payload = wire.data() + field.payload_begin;
    if (descriptor->materializer == MaterializerKind::number) {
      NormalizedNumber normalized;
      if (!NumberRules::normalize(Slice{payload, payload_size}, normalized))
        return failure(ScanMessage::invalid_number, field.payload_begin,
                       field.field_code);
      if (!writer.be64(static_cast<std::uint64_t>(normalized.mantissa)) ||
          !writer.be32(static_cast<std::uint32_t>(normalized.exponent)))
        return failure(ScanMessage::index_size_overflow, field.payload_begin,
                       field.field_code);
      return ScanStatus::success();
    }
    if (!writer.bytes(payload, payload_size))
      return failure(ScanMessage::index_size_overflow, field.payload_begin,
                     field.field_code);
    return ScanStatus::success();
  }

  [[nodiscard]] ScanStatus write_scope(std::uint32_t scope_id, bool root,
                                       Writer &writer) const noexcept {
    auto const *scope = index.scope(scope_id);
    auto const *header = index.header();
    if (scope == nullptr || header == nullptr ||
        scope->first_field > header->field_count ||
        scope->field_count() > header->field_count - scope->first_field)
      return failure(ScanMessage::invalid_index);
    for (std::uint32_t i = 0; i < scope->field_count(); ++i) {
      auto const *field = index.field(scope->first_field + i);
      if (field == nullptr)
        return failure(ScanMessage::invalid_index);
      auto const *descriptor = protocol.field_by_code(field->field_code);
      auto const value = value_size(*field);
      if (descriptor == nullptr || !value.ok())
        return value.ok()
                   ? failure(ScanMessage::invalid_index, field->header_begin,
                             field->field_code)
                   : value.status;
      if (!writer.field_header(field->field_code))
        return failure(ScanMessage::invalid_index, field->header_begin,
                       field->field_code);
      if ((descriptor->flags & field_vl_encoded) != 0 &&
          !writer.vl_prefix(value.size))
        return failure(ScanMessage::index_size_overflow, field->payload_begin,
                       field->field_code, value.size);
      auto const status = write_value(*field, writer);
      if (!status.ok())
        return status;
    }
    if (!root &&
        !writer.byte(scope->kind() == ScopeKind::array ? 0xf1 : 0xe1))
      return failure(ScanMessage::index_size_overflow);
    return ScanStatus::success();
  }
};

[[nodiscard]] CanonicalWriteResult
write_checked(Serializer const &serializer, std::uint32_t scope_id, bool root,
              std::uint8_t *output, std::uint32_t capacity) noexcept {
  auto const measured = serializer.scope_size(scope_id, root);
  if (!measured.ok())
    return {measured.status, 0};
  if (capacity < measured.size || (measured.size != 0 && output == nullptr))
    return {failure(ScanMessage::index_size_overflow, 0, 0, measured.size), 0};
  Writer writer(output, capacity);
  auto const status = serializer.write_scope(scope_id, root, writer);
  if (!status.ok() || writer.position() != measured.size)
    return {status.ok() ? failure(ScanMessage::invalid_index) : status, 0};
  return {ScanStatus::success(), writer.position()};
}

} // namespace

CanonicalSizeResult canonical_scope_size(Slice wire, RecursiveIndexView index,
                                          std::uint32_t scope_id,
                                          bool root) noexcept {
  auto const *scope = index.scope(scope_id);
  if (scope == nullptr)
    return {failure(ScanMessage::invalid_index), 0};
  return Serializer{wire, index}.scope_size(scope_id, root);
}

CanonicalWriteResult canonical_scope_write(
    Slice wire, RecursiveIndexView index, std::uint32_t scope_id, bool root,
    std::uint8_t *output, std::uint32_t capacity) noexcept {
  auto const *scope = index.scope(scope_id);
  if (scope == nullptr)
    return {failure(ScanMessage::invalid_index), 0};
  return write_checked(Serializer{wire, index}, scope_id, root, output,
                       capacity);
}

CanonicalSizeResult canonical_object_size(Slice wire, RecursiveIndexView index,
                                           std::uint32_t scope_id,
                                           bool root) noexcept {
  auto const *scope = index.scope(scope_id);
  if (scope == nullptr || scope->kind() != ScopeKind::object)
    return {failure(ScanMessage::invalid_index), 0};
  return canonical_scope_size(wire, index, scope_id, root);
}

CanonicalWriteResult canonical_object_write(
    Slice wire, RecursiveIndexView index, std::uint32_t scope_id, bool root,
    std::uint8_t *output, std::uint32_t capacity) noexcept {
  auto const *scope = index.scope(scope_id);
  if (scope == nullptr || scope->kind() != ScopeKind::object)
    return {failure(ScanMessage::invalid_index), 0};
  return canonical_scope_write(wire, index, scope_id, root, output, capacity);
}

CanonicalSizeResult canonical_field_value_size(
    Slice wire, RecursiveIndexView index, FieldRecord const &field) noexcept {
  return Serializer{wire, index}.value_size(field);
}

CanonicalWriteResult canonical_field_value_write(
    Slice wire, RecursiveIndexView index, FieldRecord const &field,
    std::uint8_t *output, std::uint32_t capacity) noexcept {
  Serializer serializer{wire, index};
  auto const measured = serializer.value_size(field);
  if (!measured.ok())
    return {measured.status, 0};
  if (capacity < measured.size || (measured.size != 0 && output == nullptr))
    return {failure(ScanMessage::index_size_overflow, field.payload_begin,
                    field.field_code, measured.size),
            0};
  Writer writer(output, capacity);
  auto const status = serializer.write_value(field, writer);
  if (!status.ok() || writer.position() != measured.size)
    return {status.ok() ? failure(ScanMessage::invalid_index) : status, 0};
  return {ScanStatus::success(), writer.position()};
}

} // namespace catl::xdata
