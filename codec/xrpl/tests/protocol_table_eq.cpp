// Host-side equality: load_from_file vs load_embedded_* (issue 0064).
#include "catl/xdata/protocol.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

using catl::xdata::Protocol;
using catl::xdata::ProtocolOptions;

static int
fail(char const* msg)
{
    std::cerr << msg << "\n";
    return 1;
}

static int
compare_protocols(Protocol const& file, Protocol const& tables, char const* tag)
{
    if (file.fields().size() != tables.fields().size())
        return fail((std::string(tag) + ": field count").c_str());

    for (size_t i = 0; i < file.fields().size(); ++i)
    {
        auto const& a = file.fields()[i];
        auto const& b = tables.fields()[i];
        if (a.name != b.name || a.code != b.code || a.header_size != b.header_size
            || a.meta.nth != b.meta.nth
            || a.meta.is_serialized != b.meta.is_serialized
            || a.meta.is_signing_field != b.meta.is_signing_field
            || a.meta.is_vl_encoded != b.meta.is_vl_encoded
            || a.meta.type.code != b.meta.type.code
            || a.meta.type.name != b.meta.type.name)
        {
            std::cerr << tag << ": field mismatch at " << i << " " << a.name
                      << " vs " << b.name << "\n";
            return 1;
        }
    }

    if (file.types() != tables.types())
        return fail((std::string(tag) + ": TYPES").c_str());
    if (file.ledgerEntryTypes() != tables.ledgerEntryTypes())
        return fail((std::string(tag) + ": LEDGER_ENTRY_TYPES").c_str());
    if (file.transactionTypes() != tables.transactionTypes())
        return fail((std::string(tag) + ": TRANSACTION_TYPES").c_str());
    if (file.transactionResults() != tables.transactionResults())
        return fail((std::string(tag) + ": TRANSACTION_RESULTS").c_str());
    if (file.permissions() != tables.permissions())
        return fail((std::string(tag) + ": PERMISSIONS").c_str());
    if (file.should_expand_xaddresses() != tables.should_expand_xaddresses())
        return fail((std::string(tag) + ": expand_xaddresses").c_str());

    for (auto const& [name, code] : file.types())
    {
        if (file.get_type_name(code) != tables.get_type_name(code))
            return fail((std::string(tag) + ": get_type_name").c_str());
        if (file.is_inferred_vl_type(code) != tables.is_inferred_vl_type(code))
            return fail((std::string(tag) + ": inferred_vl").c_str());
    }
    return 0;
}

static int
compare_network(
    char const* json_path,
    Protocol (*embed)(ProtocolOptions const&),
    char const* tag,
    ProtocolOptions opts)
{
    auto file = Protocol::load_from_file(json_path, opts);
    auto tables = embed(opts);
    return compare_protocols(file, tables, tag);
}

int
main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::cerr << "usage: protocol_table_eq xahau.json xrpl.json\n";
        return 2;
    }

    ProtocolOptions def{};
    ProtocolOptions xrpl_test{};
    xrpl_test.network_id = 1;
    ProtocolOptions xahau_test{};
    xahau_test.network_id = 21338;

    if (compare_network(
            argv[1],
            Protocol::load_embedded_xahau_protocol,
            "xahau/default",
            def))
        return 1;
    if (compare_network(
            argv[1],
            Protocol::load_embedded_xahau_protocol,
            "xahau/testnet",
            xahau_test))
        return 1;
    if (compare_network(
            argv[2],
            Protocol::load_embedded_xrpl_protocol,
            "xrpl/default",
            def))
        return 1;
    if (compare_network(
            argv[2],
            Protocol::load_embedded_xrpl_protocol,
            "xrpl/testnet",
            xrpl_test))
        return 1;
    return 0;
}
