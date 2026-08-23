#include <catl/xdata/protocol.h>
#include <catl/xdata/static_protocol.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>

using catl::xdata::Protocol;
using catl::xdata::ProtocolOptions;
using catl::xdata::xahau_static_protocol;

namespace {

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

  std::uint16_t ordinal = 0;
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
    EXPECT_EQ(view.field_by_code(field.code), &actual) << field.name;
    ++ordinal;
  }
  EXPECT_EQ(ordinal, view.field_count);
  EXPECT_EQ(view.field_by_code(0xffffffffu), nullptr);

  for (std::uint16_t material_ordinal = 0;
       material_ordinal < view.material_field_count; ++material_ordinal) {
    auto const *material = view.material_field(material_ordinal);
    ASSERT_NE(material, nullptr);
    auto const *field = view.field_by_ordinal(material->admission_ordinal);
    ASSERT_NE(field, nullptr);
    EXPECT_EQ(field->code, material->field_code);
    EXPECT_EQ(field->material_ordinal, material_ordinal);
    EXPECT_EQ(field->materializer, material->materializer);
    EXPECT_NE(material->materializer, catl::xdata::MaterializerKind::invalid);
  }
}
