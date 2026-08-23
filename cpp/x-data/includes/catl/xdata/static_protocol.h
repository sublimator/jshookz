#pragma once

#include <cstddef>
#include <cstdint>

namespace catl::xdata {

// Allocation-free protocol metadata used by the sealed provider.  This is
// deliberately separate from Protocol: Protocol remains the convenient,
// dynamic native/tooling representation while ProtocolView is immutable and
// safe to reach from -fno-exceptions provider code.
enum class MaterializerKind : std::uint8_t {
  invalid = 0,
  uint8,
  uint16,
  uint32,
  uint64,
  hash128,
  hash160,
  hash192,
  hash256,
  blob,
  account_id,
  amount,
  currency,
  issue,
  number,
  path_set,
  vector256,
  xchain_bridge,
  st_object,
  st_array,
  transaction_type,
  transaction_result,
};

enum StaticFieldFlags : std::uint8_t {
  field_signing = 1u << 0,
  field_vl_encoded = 1u << 1,
  field_object_end = 1u << 2,
  field_array_end = 1u << 3,
};

enum StaticFieldNameFlags : std::uint8_t {
  field_name_serialized = 1u << 0,
  field_name_signing = 1u << 1,
  field_name_vl_encoded = 1u << 2,
};

struct StaticFieldName {
  std::uint32_t code;
  std::uint16_t offset;
  std::uint8_t size;
  std::uint8_t flags;
};

struct StaticFieldDescriptor {
  std::uint32_t code;
  std::uint16_t name_ordinal;
  std::uint16_t material_ordinal;
  std::uint16_t fixed_size;
  std::uint8_t header_size;
  std::uint8_t wire_type;
  std::uint8_t flags;
  MaterializerKind materializer;
  std::uint16_t reserved;
};

struct StaticFallbackField {
  std::uint32_t field_code;
  std::uint16_t admission_ordinal;
  std::uint16_t flags;
};

struct StaticMaterialField {
  std::uint32_t field_code;
  std::uint16_t admission_ordinal;
  MaterializerKind materializer;
  std::uint8_t reserved;
};

struct StaticTypeDescriptor {
  std::uint16_t code;
  std::uint16_t fixed_size;
  std::uint16_t name_offset;
  std::uint8_t name_size;
  MaterializerKind materializer;
};

static_assert(sizeof(StaticFieldName) == 8);
static_assert(sizeof(StaticFieldDescriptor) == 16);
static_assert(sizeof(StaticFallbackField) == 8);
static_assert(sizeof(StaticMaterialField) == 8);
static_assert(sizeof(StaticTypeDescriptor) == 8);

struct StaticNameView {
  char const *data;
  std::uint16_t size;
};

struct ProtocolView {
  static constexpr std::uint16_t fast_type_count = 32;
  static constexpr std::uint16_t fast_nth_count = 128;
  static constexpr std::uint16_t no_ordinal = 0xffff;

  StaticFieldDescriptor const *fields;
  StaticFieldName const *field_names;
  StaticFallbackField const *fallback_fields;
  StaticMaterialField const *material_fields;
  StaticTypeDescriptor const *types;
  std::uint16_t const (*fast_ordinals)[fast_nth_count];
  char const *field_name_bytes;
  char const *type_name_bytes;
  char const *definitions_sha256;
  char const *materializer_policy_sha256;
  char const *identity;
  std::uint32_t field_name_bytes_size;
  std::uint16_t field_count;
  std::uint16_t field_name_count;
  std::uint16_t fallback_count;
  std::uint16_t material_field_count;
  std::uint16_t type_count;
  std::uint16_t max_type_code;
  std::uint16_t max_nth;
  std::uint16_t duplicate_word_count;
  std::uint16_t inferred_vl_count;

  [[nodiscard]] StaticFieldDescriptor const *
  field_by_code(std::uint32_t field_code) const noexcept;

  [[nodiscard]] StaticFieldDescriptor const *
  field_by_ordinal(std::uint16_t ordinal) const noexcept {
    return ordinal < field_count ? fields + ordinal : nullptr;
  }

  [[nodiscard]] StaticMaterialField const *
  material_field(std::uint16_t ordinal) const noexcept {
    return ordinal < material_field_count ? material_fields + ordinal : nullptr;
  }

  [[nodiscard]] StaticNameView
  field_name(std::uint16_t ordinal) const noexcept {
    if (ordinal >= field_name_count)
      return {nullptr, 0};
    auto const &name = field_names[ordinal];
    return {field_name_bytes + name.offset, name.size};
  }
};

static_assert(sizeof(std::uint16_t) * ProtocolView::fast_type_count *
                  ProtocolView::fast_nth_count ==
              8192);

// The only protocol selected by the sealed Xahau provider profile.
[[nodiscard]] ProtocolView const &xahau_static_protocol() noexcept;

} // namespace catl::xdata
