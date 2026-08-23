#include <catl/xdata/protocol.h>
#include <catl/xdata/static_protocol.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

using catl::xdata::Protocol;
using catl::xdata::ProtocolOptions;
using catl::xdata::xahau_static_protocol;

namespace {

using catl::xdata::MaterializerKind;

struct ExpectedType {
  std::string_view name;
  std::uint16_t code;
  std::uint16_t width;
  MaterializerKind materializer;
};

constexpr std::array<ExpectedType, 19> EXPECTED_TYPES{{
    {"UInt16", 1, 2, MaterializerKind::uint16},
    {"UInt32", 2, 4, MaterializerKind::uint32},
    {"UInt64", 3, 8, MaterializerKind::uint64},
    {"Hash128", 4, 16, MaterializerKind::hash128},
    {"Hash256", 5, 32, MaterializerKind::hash256},
    {"Amount", 6, 0, MaterializerKind::amount},
    {"Blob", 7, 0, MaterializerKind::blob},
    {"AccountID", 8, 0, MaterializerKind::account_id},
    {"Number", 9, 12, MaterializerKind::number},
    {"STObject", 14, 0, MaterializerKind::st_object},
    {"STArray", 15, 0, MaterializerKind::st_array},
    {"UInt8", 16, 1, MaterializerKind::uint8},
    {"Hash160", 17, 20, MaterializerKind::hash160},
    {"PathSet", 18, 0, MaterializerKind::path_set},
    {"Vector256", 19, 0, MaterializerKind::vector256},
    {"Hash192", 21, 24, MaterializerKind::hash192},
    {"Issue", 24, 0, MaterializerKind::issue},
    {"XChainBridge", 25, 0, MaterializerKind::xchain_bridge},
    {"Currency", 26, 20, MaterializerKind::currency},
}};

MaterializerKind materializer_for(std::string_view field,
                                  std::string_view type) {
  if (field == "TransactionType")
    return MaterializerKind::transaction_type;
  if (field == "TransactionResult")
    return MaterializerKind::transaction_result;
  auto const match = std::find_if(
      EXPECTED_TYPES.begin(), EXPECTED_TYPES.end(),
      [type](ExpectedType const &candidate) { return candidate.name == type; });
  return match == EXPECTED_TYPES.end() ? MaterializerKind::invalid
                                       : match->materializer;
}

bool fast_table_matches(Protocol const &dynamic,
                        catl::xdata::ProtocolView const &view) {
  std::uint16_t expected[32][128]{};
  std::uint16_t ordinal = 0;
  for (auto const &field : dynamic.fields()) {
    if (!field.meta.is_serialized)
      continue;
    auto const type_code = static_cast<std::uint16_t>(field.code >> 16);
    auto const nth = static_cast<std::uint16_t>(field.code);
    if (type_code < 32 && nth < 128)
      expected[type_code][nth] = static_cast<std::uint16_t>(ordinal + 1);
    ++ordinal;
  }
  for (std::uint16_t type_code = 0; type_code < 32; ++type_code) {
    for (std::uint16_t nth = 0; nth < 128; ++nth) {
      if (view.fast_ordinals[type_code][nth] != expected[type_code][nth])
        return false;
    }
  }
  return true;
}

bool materializer_rows_match(Protocol const &dynamic,
                             catl::xdata::ProtocolView const &view) {
  std::uint16_t field_ordinal = 0;
  std::uint16_t material_ordinal = 0;
  for (auto const &field : dynamic.fields()) {
    if (!field.meta.is_serialized)
      continue;
    if (field_ordinal >= view.field_count)
      return false;
    auto const &actual = view.fields[field_ordinal];
    bool const sentinel =
        field.name == "ObjectEndMarker" || field.name == "ArrayEndMarker";
    auto const expected =
        sentinel ? MaterializerKind::invalid
                 : materializer_for(field.name, field.meta.type.name);
    if (actual.materializer != expected)
      return false;
    if (sentinel) {
      if (actual.material_ordinal != catl::xdata::ProtocolView::no_ordinal)
        return false;
    } else {
      if (actual.material_ordinal != material_ordinal ||
          material_ordinal >= view.material_field_count)
        return false;
      auto const &material = view.material_fields[material_ordinal];
      if (material.admission_ordinal != field_ordinal ||
          material.field_code != field.code ||
          material.materializer != expected)
        return false;
      ++material_ordinal;
    }
    ++field_ordinal;
  }
  return field_ordinal == view.field_count &&
         material_ordinal == view.material_field_count;
}

void expect_same(Protocol const &file, Protocol const &tables,
                 char const *tag) {
  ASSERT_EQ(file.fields().size(), tables.fields().size())
      << tag << ": field count";
  for (size_t i = 0; i < file.fields().size(); ++i) {
    auto const &a = file.fields()[i];
    auto const &b = tables.fields()[i];
    EXPECT_EQ(a.name, b.name) << tag << " field " << i;
    EXPECT_EQ(a.code, b.code) << tag << " " << a.name;
    EXPECT_EQ(a.header_size, b.header_size) << tag << " " << a.name;
    EXPECT_EQ(a.meta.nth, b.meta.nth) << tag << " " << a.name;
    EXPECT_EQ(a.meta.is_serialized, b.meta.is_serialized)
        << tag << " " << a.name;
    EXPECT_EQ(a.meta.is_signing_field, b.meta.is_signing_field)
        << tag << " " << a.name;
    EXPECT_EQ(a.meta.is_vl_encoded, b.meta.is_vl_encoded)
        << tag << " " << a.name;
    EXPECT_EQ(a.meta.type.code, b.meta.type.code) << tag << " " << a.name;
    EXPECT_EQ(a.meta.type.name, b.meta.type.name) << tag << " " << a.name;
  }
  EXPECT_EQ(file.types(), tables.types()) << tag;
  EXPECT_EQ(file.ledgerEntryTypes(), tables.ledgerEntryTypes()) << tag;
  EXPECT_EQ(file.transactionTypes(), tables.transactionTypes()) << tag;
  EXPECT_EQ(file.transactionResults(), tables.transactionResults()) << tag;
  EXPECT_EQ(file.permissions(), tables.permissions()) << tag;
  EXPECT_EQ(file.should_expand_xaddresses(), tables.should_expand_xaddresses())
      << tag;
  for (auto const &[name, code] : file.types()) {
    EXPECT_EQ(file.get_type_name(code), tables.get_type_name(code)) << tag;
    EXPECT_EQ(file.is_inferred_vl_type(code), tables.is_inferred_vl_type(code))
        << tag;
    (void)name;
  }
}

void compare_network(char const *json_path,
                     Protocol (*embed)(ProtocolOptions const &),
                     char const *tag, ProtocolOptions opts) {
  expect_same(Protocol::load_from_file(json_path, opts), embed(opts), tag);
}

} // namespace

TEST(ProtocolTables, EmbeddedMatchesCleanedJson) {
  ProtocolOptions def{};
  ProtocolOptions xrpl_test{};
  xrpl_test.network_id = 1;
  ProtocolOptions xahau_test{};
  xahau_test.network_id = 21338;

  compare_network(JSHOOKZ_XAHAU_CLEANED_JSON,
                  Protocol::load_embedded_xahau_protocol, "xahau/default", def);
  compare_network(JSHOOKZ_XAHAU_CLEANED_JSON,
                  Protocol::load_embedded_xahau_protocol, "xahau/testnet",
                  xahau_test);
  compare_network(JSHOOKZ_XRPL_CLEANED_JSON,
                  Protocol::load_embedded_xrpl_protocol, "xrpl/default", def);
  compare_network(JSHOOKZ_XRPL_CLEANED_JSON,
                  Protocol::load_embedded_xrpl_protocol, "xrpl/testnet",
                  xrpl_test);
}

TEST(ProtocolTables, ProviderStaticXahauMatchesDynamicAuthority) {
  auto const dynamic = Protocol::load_embedded_xahau_protocol();
  auto const &view = xahau_static_protocol();
  EXPECT_EQ(view.field_name_count, 337);
  EXPECT_EQ(view.field_count, 327);
  EXPECT_EQ(view.material_field_count, 325);
  EXPECT_EQ(view.type_count, 19);
  EXPECT_EQ(view.fallback_count, 0);
  EXPECT_EQ(view.inferred_vl_count, 0);
  EXPECT_EQ(view.duplicate_word_count, 6);
  EXPECT_TRUE(fast_table_matches(dynamic, view));
  EXPECT_TRUE(materializer_rows_match(dynamic, view));
  ASSERT_NE(view.definitions_sha256, nullptr);
  ASSERT_NE(view.materializer_policy_sha256, nullptr);
  ASSERT_NE(view.identity, nullptr);
  EXPECT_STREQ(view.definitions_sha256, JSHOOKZ_XAHAU_DEFINITIONS_SHA256);
  EXPECT_STREQ(view.materializer_policy_sha256,
               JSHOOKZ_XAHAU_PROVIDER_POLICY_SHA256);
  auto const expected_identity = std::string("xahau:") +
                                 JSHOOKZ_XAHAU_DEFINITIONS_SHA256 + ":" +
                                 JSHOOKZ_XAHAU_PROVIDER_POLICY_SHA256;
  EXPECT_EQ(view.identity, expected_identity);

  ASSERT_EQ(dynamic.fields().size(), view.field_name_count);
  std::uint32_t expected_name_offset = 0;
  for (std::uint16_t name_ordinal = 0; name_ordinal < view.field_name_count;
       ++name_ordinal) {
    auto const &expected = dynamic.fields()[name_ordinal];
    auto const &actual = view.field_names[name_ordinal];
    auto const name = view.field_name(name_ordinal);
    ASSERT_NE(name.data, nullptr);
    EXPECT_EQ(actual.offset, expected_name_offset) << expected.name;
    EXPECT_EQ(name.size, expected.name.size()) << expected.name;
    EXPECT_EQ(std::string_view(name.data, name.size), expected.name);
    EXPECT_EQ(name.data[name.size], '\0') << expected.name;
    EXPECT_EQ(actual.code, expected.code) << expected.name;
    std::uint8_t expected_flags = 0;
    if (expected.meta.is_serialized)
      expected_flags |= catl::xdata::field_name_serialized;
    if (expected.meta.is_signing_field)
      expected_flags |= catl::xdata::field_name_signing;
    if (expected.meta.is_vl_encoded)
      expected_flags |= catl::xdata::field_name_vl_encoded;
    EXPECT_EQ(actual.flags, expected_flags) << expected.name;
    expected_name_offset +=
        static_cast<std::uint32_t>(expected.name.size() + 1);
  }
  EXPECT_EQ(view.field_name_bytes_size, expected_name_offset);

  std::uint16_t ordinal = 0;
  std::uint16_t material_ordinal = 0;
  std::uint16_t expected_fast[32][128]{};
  struct ExpectedFallback {
    std::uint32_t code;
    std::uint16_t ordinal;
    std::uint16_t flags;
  };
  std::vector<ExpectedFallback> expected_fallback;
  for (auto const &field : dynamic.fields()) {
    if (!field.meta.is_serialized)
      continue;
    ASSERT_LT(ordinal, view.field_count);
    auto const &actual = view.fields[ordinal];
    auto const name = view.field_name(actual.name_ordinal);
    ASSERT_NE(name.data, nullptr);
    EXPECT_EQ(std::string_view(name.data, name.size), field.name);
    EXPECT_EQ(actual.code, field.code) << field.name;
    EXPECT_EQ(actual.header_size, field.header_size) << field.name;
    EXPECT_EQ(actual.wire_type, field.meta.type.code) << field.name;
    EXPECT_EQ(actual.fixed_size, field.meta.type.fixed_size) << field.name;
    EXPECT_EQ(bool(actual.flags & catl::xdata::field_signing),
              field.meta.is_signing_field)
        << field.name;
    EXPECT_EQ(bool(actual.flags & catl::xdata::field_vl_encoded),
              field.meta.is_vl_encoded)
        << field.name;
    auto const object_end = field.name == "ObjectEndMarker";
    auto const array_end = field.name == "ArrayEndMarker";
    EXPECT_EQ(bool(actual.flags & catl::xdata::field_object_end), object_end)
        << field.name;
    EXPECT_EQ(bool(actual.flags & catl::xdata::field_array_end), array_end)
        << field.name;
    auto const expected_materializer =
        object_end || array_end
            ? MaterializerKind::invalid
            : materializer_for(field.name, field.meta.type.name);
    EXPECT_EQ(actual.materializer, expected_materializer) << field.name;
    if (object_end || array_end) {
      EXPECT_EQ(actual.material_ordinal, catl::xdata::ProtocolView::no_ordinal)
          << field.name;
    } else {
      EXPECT_EQ(actual.material_ordinal, material_ordinal) << field.name;
      auto const *material = view.material_field(material_ordinal);
      ASSERT_NE(material, nullptr);
      EXPECT_EQ(material->field_code, field.code) << field.name;
      EXPECT_EQ(material->admission_ordinal, ordinal) << field.name;
      EXPECT_EQ(material->materializer, expected_materializer) << field.name;
      ++material_ordinal;
    }
    EXPECT_EQ(view.field_by_code(field.code), &actual) << field.name;
    auto const type_code = static_cast<std::uint16_t>(field.code >> 16);
    auto const nth = static_cast<std::uint16_t>(field.code);
    if (type_code < 32 && nth < 128) {
      ASSERT_EQ(expected_fast[type_code][nth], 0) << field.name;
      expected_fast[type_code][nth] = static_cast<std::uint16_t>(ordinal + 1);
    } else {
      expected_fallback.push_back({field.code, ordinal, actual.flags});
    }
    ++ordinal;
  }
  EXPECT_EQ(ordinal, view.field_count);
  EXPECT_EQ(material_ordinal, view.material_field_count);
  EXPECT_EQ(view.field_by_code(0xffffffffu), nullptr);

  for (std::uint16_t type_code = 0; type_code < 32; ++type_code) {
    for (std::uint16_t nth = 0; nth < 128; ++nth) {
      EXPECT_EQ(view.fast_ordinals[type_code][nth],
                expected_fast[type_code][nth])
          << "fast cell " << type_code << "/" << nth;
    }
  }
  std::sort(expected_fallback.begin(), expected_fallback.end(),
            [](auto const &left, auto const &right) {
              return left.code < right.code;
            });
  ASSERT_EQ(view.fallback_count, expected_fallback.size());
  for (std::uint16_t index = 0; index < view.fallback_count; ++index) {
    EXPECT_EQ(view.fallback_fields[index].field_code,
              expected_fallback[index].code);
    EXPECT_EQ(view.fallback_fields[index].admission_ordinal,
              expected_fallback[index].ordinal);
    EXPECT_EQ(view.fallback_fields[index].flags,
              expected_fallback[index].flags);
  }

  ASSERT_EQ(view.type_count, EXPECTED_TYPES.size());
  for (std::uint16_t index = 0; index < view.type_count; ++index) {
    auto const &actual = view.types[index];
    auto const &expected = EXPECTED_TYPES[index];
    auto const dynamic_code = dynamic.get_type_code(std::string(expected.name));
    ASSERT_TRUE(dynamic_code.has_value()) << expected.name;
    EXPECT_EQ(actual.code, *dynamic_code) << expected.name;
    EXPECT_EQ(actual.code, expected.code) << expected.name;
    EXPECT_EQ(actual.fixed_size, expected.width) << expected.name;
    EXPECT_EQ(actual.materializer, expected.materializer) << expected.name;
    EXPECT_EQ(std::string_view(view.type_name_bytes + actual.name_offset,
                               actual.name_size),
              expected.name);
  }
}

TEST(ProtocolTables, ProviderStaticOutputMutationControlsTurnRed) {
  auto const dynamic = Protocol::load_embedded_xahau_protocol();
  auto const &clean = xahau_static_protocol();
  ASSERT_TRUE(fast_table_matches(dynamic, clean));
  ASSERT_TRUE(materializer_rows_match(dynamic, clean));

  std::uint16_t poisoned_fast[32][128];
  std::memcpy(poisoned_fast, clean.fast_ordinals, sizeof(poisoned_fast));
  ASSERT_EQ(poisoned_fast[31][127], 0);
  poisoned_fast[31][127] = 1;
  auto fast_poison = clean;
  fast_poison.fast_ordinals = poisoned_fast;
  EXPECT_FALSE(fast_table_matches(dynamic, fast_poison));

  std::vector<catl::xdata::StaticFieldDescriptor> poisoned_fields(
      clean.fields, clean.fields + clean.field_count);
  std::vector<catl::xdata::StaticMaterialField> poisoned_material(
      clean.material_fields,
      clean.material_fields + clean.material_field_count);
  ASSERT_FALSE(poisoned_material.empty());
  auto &material = poisoned_material.front();
  auto &field = poisoned_fields[material.admission_ordinal];
  auto const wrong = material.materializer == MaterializerKind::uint8
                         ? MaterializerKind::uint16
                         : MaterializerKind::uint8;
  ASSERT_NE(wrong, MaterializerKind::invalid);
  field.materializer = wrong;
  material.materializer = wrong;
  auto materializer_poison = clean;
  materializer_poison.fields = poisoned_fields.data();
  materializer_poison.material_fields = poisoned_material.data();
  EXPECT_FALSE(materializer_rows_match(dynamic, materializer_poison));
}
