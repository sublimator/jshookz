#include "catl/xdata/canonical_serializer.h"

#include "catl/xdata/canonical_writer.h"
#include "catl/xdata/number-rules.h"
#include "catl/xdata/static_protocol.h"

#include <cstddef>
#include <cstdint>

namespace catl::xdata {
namespace {

[[nodiscard]] constexpr ScanStatus failure(ScanMessage message,
                                           std::uint32_t offset = 0,
                                           std::uint32_t field_code = 0,
                                           std::uint32_t aux = 0) noexcept {
  return {static_cast<std::uint16_t>(ScanIssue::internal_error),
          static_cast<std::uint16_t>(message), offset, field_code, aux};
}

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

  [[nodiscard]] ScanStatus emit_value(FieldRecord const &field,
                                      CanonicalWriter &writer) const noexcept {
    StaticFieldDescriptor const *descriptor = nullptr;
    std::uint32_t payload_size = 0;
    if (!valid_field(field, descriptor, payload_size))
      return failure(ScanMessage::invalid_index, field.header_begin,
                     field.field_code);
    if (field.child_scope != FieldRecord::no_child)
      return emit_scope(field.child_scope, false, writer);
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

  [[nodiscard]] ScanStatus emit_scope(std::uint32_t scope_id, bool root,
                                      CanonicalWriter &writer) const noexcept {
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
      StaticFieldDescriptor const *descriptor = nullptr;
      std::uint32_t payload_size = 0;
      if (!valid_field(*field, descriptor, payload_size))
        return failure(ScanMessage::invalid_index, field->header_begin,
                       field->field_code);
      if (!writer.field_header(field->field_code))
        return failure(ScanMessage::invalid_index, field->header_begin,
                       field->field_code);
      if ((descriptor->flags & field_vl_encoded) != 0) {
        if (field->child_scope != FieldRecord::no_child)
          return failure(ScanMessage::invalid_index, field->header_begin,
                         field->field_code);
        auto const value_size =
            descriptor->materializer == MaterializerKind::number ? 12
                                                                 : payload_size;
        if (!writer.vl_prefix(value_size))
          return failure(ScanMessage::index_size_overflow, field->payload_begin,
                         field->field_code, value_size);
      }
      auto const status = emit_value(*field, writer);
      if (!status.ok())
        return status;
    }
    if (!root && !writer.byte(scope->kind() == ScopeKind::array ? 0xf1 : 0xe1))
      return failure(ScanMessage::index_size_overflow);
    return ScanStatus::success();
  }
};

[[nodiscard]] CanonicalWriteResult
write_checked(Serializer const &serializer, std::uint32_t scope_id, bool root,
              std::uint8_t *output, std::uint32_t capacity) noexcept {
  auto counter = CanonicalWriter::measuring();
  auto status = serializer.emit_scope(scope_id, root, counter);
  if (!status.ok())
    return {status, 0};
  if (capacity < counter.position() ||
      (counter.position() != 0 && output == nullptr))
    return {failure(ScanMessage::index_size_overflow, 0, 0, counter.position()),
            0};
  CanonicalWriter writer(output, capacity);
  status = serializer.emit_scope(scope_id, root, writer);
  if (!status.ok() || writer.position() != counter.position())
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
  auto writer = CanonicalWriter::measuring();
  auto const status =
      Serializer{wire, index}.emit_scope(scope_id, root, writer);
  return {status, status.ok() ? writer.position() : 0};
}

ScanStatus canonical_scope_emit(Slice wire, RecursiveIndexView index,
                                std::uint32_t scope_id, bool root,
                                CanonicalWriter &writer) noexcept {
  return Serializer{wire, index}.emit_scope(scope_id, root, writer);
}

CanonicalWriteResult canonical_scope_write(Slice wire, RecursiveIndexView index,
                                           std::uint32_t scope_id, bool root,
                                           std::uint8_t *output,
                                           std::uint32_t capacity) noexcept {
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

CanonicalWriteResult canonical_object_write(Slice wire,
                                            RecursiveIndexView index,
                                            std::uint32_t scope_id, bool root,
                                            std::uint8_t *output,
                                            std::uint32_t capacity) noexcept {
  auto const *scope = index.scope(scope_id);
  if (scope == nullptr || scope->kind() != ScopeKind::object)
    return {failure(ScanMessage::invalid_index), 0};
  return canonical_scope_write(wire, index, scope_id, root, output, capacity);
}

CanonicalSizeResult
canonical_field_value_size(Slice wire, RecursiveIndexView index,
                           FieldRecord const &field) noexcept {
  auto writer = CanonicalWriter::measuring();
  auto const status = Serializer{wire, index}.emit_value(field, writer);
  return {status, status.ok() ? writer.position() : 0};
}

ScanStatus canonical_field_value_emit(Slice wire, RecursiveIndexView index,
                                      FieldRecord const &field,
                                      CanonicalWriter &writer) noexcept {
  return Serializer{wire, index}.emit_value(field, writer);
}

CanonicalWriteResult
canonical_field_value_write(Slice wire, RecursiveIndexView index,
                            FieldRecord const &field, std::uint8_t *output,
                            std::uint32_t capacity) noexcept {
  Serializer serializer{wire, index};
  auto counter = CanonicalWriter::measuring();
  auto status = serializer.emit_value(field, counter);
  if (!status.ok())
    return {status, 0};
  if (capacity < counter.position() ||
      (counter.position() != 0 && output == nullptr))
    return {failure(ScanMessage::index_size_overflow, field.payload_begin,
                    field.field_code, counter.position()),
            0};
  CanonicalWriter writer(output, capacity);
  status = serializer.emit_value(field, writer);
  if (!status.ok() || writer.position() != counter.position())
    return {status.ok() ? failure(ScanMessage::invalid_index) : status, 0};
  return {ScanStatus::success(), writer.position()};
}

} // namespace catl::xdata
