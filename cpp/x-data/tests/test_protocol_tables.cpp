#include <catl/xdata/protocol.h>

#include <gtest/gtest.h>

#include <string>

using catl::xdata::Protocol;
using catl::xdata::ProtocolOptions;

namespace {

void
expect_same(Protocol const& file, Protocol const& tables, char const* tag)
{
    ASSERT_EQ(file.fields().size(), tables.fields().size()) << tag << ": field count";
    for (size_t i = 0; i < file.fields().size(); ++i) {
        auto const& a = file.fields()[i];
        auto const& b = tables.fields()[i];
        EXPECT_EQ(a.name, b.name) << tag << " field " << i;
        EXPECT_EQ(a.code, b.code) << tag << " " << a.name;
        EXPECT_EQ(a.header_size, b.header_size) << tag << " " << a.name;
        EXPECT_EQ(a.meta.nth, b.meta.nth) << tag << " " << a.name;
        EXPECT_EQ(a.meta.is_serialized, b.meta.is_serialized) << tag << " " << a.name;
        EXPECT_EQ(a.meta.is_signing_field, b.meta.is_signing_field)
            << tag << " " << a.name;
        EXPECT_EQ(a.meta.is_vl_encoded, b.meta.is_vl_encoded) << tag << " " << a.name;
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
    for (auto const& [name, code] : file.types()) {
        EXPECT_EQ(file.get_type_name(code), tables.get_type_name(code)) << tag;
        EXPECT_EQ(file.is_inferred_vl_type(code), tables.is_inferred_vl_type(code))
            << tag;
        (void)name;
    }
}

void
compare_network(
    char const* json_path,
    Protocol (*embed)(ProtocolOptions const&),
    char const* tag,
    ProtocolOptions opts)
{
    expect_same(Protocol::load_from_file(json_path, opts), embed(opts), tag);
}

}  // namespace

TEST(ProtocolTables, EmbeddedMatchesCleanedJson)
{
    ProtocolOptions def{};
    ProtocolOptions xrpl_test{};
    xrpl_test.network_id = 1;
    ProtocolOptions xahau_test{};
    xahau_test.network_id = 21338;

    compare_network(
        JSHOOKZ_XAHAU_CLEANED_JSON,
        Protocol::load_embedded_xahau_protocol,
        "xahau/default",
        def);
    compare_network(
        JSHOOKZ_XAHAU_CLEANED_JSON,
        Protocol::load_embedded_xahau_protocol,
        "xahau/testnet",
        xahau_test);
    compare_network(
        JSHOOKZ_XRPL_CLEANED_JSON,
        Protocol::load_embedded_xrpl_protocol,
        "xrpl/default",
        def);
    compare_network(
        JSHOOKZ_XRPL_CLEANED_JSON,
        Protocol::load_embedded_xrpl_protocol,
        "xrpl/testnet",
        xrpl_test);
}
