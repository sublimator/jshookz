// Embed load from generated Protocol tables (issue 0064).
#include "catl/xdata/protocol.h"
#include "catl/xdata/exception_policy.h"
#include "catl/xdata/protocol_tables.h"
#include "embedded_xahau_definitions.h"
#include "embedded_xrpl_definitions.h"

#include <cstdint>

namespace catl::xdata {

Protocol
Protocol::load_embedded_xahau_protocol(const ProtocolOptions& opts)
{
    if (xahau::FIELDS.empty())
    {
        CATL_XDATA_THROW(std::runtime_error(
            "Protocol tables must contain FIELDS"));
    }

    Protocol protocol;
    protocol.apply_load_options(opts);

    for (auto const& t : xahau::TYPES)
    {
        auto const code = static_cast<uint16_t>(t.code);
        protocol.types_[t.name] = code;
        protocol.typeCodeToName_[code] = t.name;
    }
    for (auto const& f : xahau::FIELDS)
    {
        protocol.add_table_field(
            f.name,
            f.type_name,
            f.nth,
            f.is_serialized,
            f.is_signing_field,
            f.is_vl_encoded);
    }
    for (auto const& t : xahau::LEDGER_ENTRY_TYPES)
        protocol.ledgerEntryTypes_[t.name] = static_cast<uint16_t>(t.code);
    for (auto const& t : xahau::TRANSACTION_TYPES)
        protocol.transactionTypes_[t.name] = static_cast<uint16_t>(t.code);
    for (auto const& t : xahau::TRANSACTION_RESULTS)
        protocol.transactionResults_[t.name] = static_cast<int32_t>(t.code);
    for (auto const& t : xahau::PERMISSIONS)
        protocol.permissions_[t.name] = static_cast<uint32_t>(t.code);

    protocol.finish_table_load(opts);
    return protocol;
}

Protocol
Protocol::load_embedded_xrpl_protocol(const ProtocolOptions& opts)
{
    if (xrpl::FIELDS.empty())
    {
        CATL_XDATA_THROW(std::runtime_error(
            "Protocol tables must contain FIELDS"));
    }

    Protocol protocol;
    protocol.apply_load_options(opts);

    for (auto const& t : xrpl::TYPES)
    {
        auto const code = static_cast<uint16_t>(t.code);
        protocol.types_[t.name] = code;
        protocol.typeCodeToName_[code] = t.name;
    }
    for (auto const& f : xrpl::FIELDS)
    {
        protocol.add_table_field(
            f.name,
            f.type_name,
            f.nth,
            f.is_serialized,
            f.is_signing_field,
            f.is_vl_encoded);
    }
    for (auto const& t : xrpl::LEDGER_ENTRY_TYPES)
        protocol.ledgerEntryTypes_[t.name] = static_cast<uint16_t>(t.code);
    for (auto const& t : xrpl::TRANSACTION_TYPES)
        protocol.transactionTypes_[t.name] = static_cast<uint16_t>(t.code);
    for (auto const& t : xrpl::TRANSACTION_RESULTS)
        protocol.transactionResults_[t.name] = static_cast<int32_t>(t.code);
    for (auto const& t : xrpl::PERMISSIONS)
        protocol.permissions_[t.name] = static_cast<uint32_t>(t.code);

    protocol.finish_table_load(opts);
    return protocol;
}

}  // namespace catl::xdata
