#pragma once

#include "catl/xdata/fields.h"
#include "catl/xdata/types.h"
#ifndef CATL_XDATA_NO_BOOST_JSON
#include <boost/json.hpp>
#endif
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace catl::xdata {

// Protocol loading options
struct ProtocolOptions
{
    std::optional<uint32_t> network_id;  // Which network we're parsing for
    bool allow_vl_inference = true;      // Safe unknown type handling
    bool expand_xaddresses = false;  // Expand X-addresses during serialization
};

// Protocol definitions matching XRPLDefinitions interface
class Protocol
{
public:
#ifndef CATL_XDATA_NO_BOOST_JSON
    // Load definitions from JSON file with options
    static Protocol
    load_from_file(const std::string& path, const ProtocolOptions& opts = {});

    // Load definitions from JSON value with options
    static Protocol
    load_from_json_value(
        const boost::json::value& jv,
        const ProtocolOptions& opts = {});
#endif

    // Load embedded Xahau protocol definitions
    static Protocol
    load_embedded_xahau_protocol(const ProtocolOptions& opts = {});

    // Load embedded XRPL protocol definitions
    static Protocol
    load_embedded_xrpl_protocol(const ProtocolOptions& opts = {});

    // Access field definitions
    const std::vector<FieldDef>&
    fields() const
    {
        return fields_;
    }

    // Find field by name
    std::optional<FieldDef>
    find_field(const std::string& name) const;

    // Get field by type and field ID
    std::optional<FieldDef>
    get_field(const std::string& type, uint16_t fieldId) const;

    // Get field by field code (optimized with lookup table)
    const FieldDef*
    get_field_by_code(uint32_t field_code) const;
    std::optional<FieldDef>
    get_field_by_code_opt(uint32_t field_code) const;

    // Access type mappings
    const std::unordered_map<std::string, uint16_t>&
    types() const
    {
        return types_;
    }

    // Access ledger entry types
    const std::unordered_map<std::string, uint16_t>&
    ledgerEntryTypes() const
    {
        return ledgerEntryTypes_;
    }

    // Access transaction types
    const std::unordered_map<std::string, uint16_t>&
    transactionTypes() const
    {
        return transactionTypes_;
    }

    // Access transaction results
    const std::unordered_map<std::string, int32_t>&
    transactionResults() const
    {
        return transactionResults_;
    }

    // Get type code for type name
    std::optional<uint16_t>
    get_type_code(const std::string& typeName) const;

    // Get type name for type code
    std::optional<std::string>
    get_type_name(uint16_t typeCode) const;

    // Get transaction type name for transaction type code
    std::optional<std::string>
    get_transaction_type_name(uint16_t txTypeCode) const;

    // Get ledger entry type name for ledger entry type code
    std::optional<std::string>
    get_ledger_entry_type_name(uint16_t leTypeCode) const;

    // Access granular permission mappings
    const std::unordered_map<std::string, uint32_t>&
    permissions() const
    {
        return permissions_;
    }

    // Check if a type was inferred as VL-encoded
    bool
    is_inferred_vl_type(uint16_t type_code) const
    {
        return inferred_vl_types_.count(type_code) > 0;
    }

    bool
    should_expand_xaddresses() const
    {
        return expand_xaddresses_;
    }

    void
    set_expand_xaddresses(bool v)
    {
        expand_xaddresses_ = v;
    }

    static constexpr uint16_t kFastTypeDim = 32;
    static constexpr uint16_t kFastNthDim = 128;

    uint16_t
    max_serialized_type_code() const
    {
        return max_serialized_type_code_;
    }

    uint16_t
    max_serialized_nth() const
    {
        return max_serialized_nth_;
    }

    std::size_t
    fast_lookup_fallback_count() const
    {
        return fast_lookup_fallback_count_;
    }

    static constexpr std::size_t
    fast_lookup_bytes()
    {
        return sizeof(const FieldDef*) * kFastTypeDim * kFastNthDim;
    }

private:
    bool expand_xaddresses_ = false;
    // Network this protocol was loaded for (if specified)
    std::optional<uint32_t> network_id_;

    // Field definitions array
    std::vector<FieldDef> fields_;

    // Types that were inferred as VL-encoded during loading
    std::unordered_set<uint16_t> inferred_vl_types_;

    uint16_t max_serialized_type_code_ = 0;
    uint16_t max_serialized_nth_ = 0;
    std::size_t fast_lookup_fallback_count_ = 0;

    //@@start fast-lookup
    // Fast lookup: type < 32, nth < 128. 32*128*sizeof(ptr) = 16KB on wasm32.
    // Was 256*256 = 256KB. Types 10001+ and nth >= 128 use fieldCodeIndex_.
    struct LookupTable {
        const FieldDef* data[kFastTypeDim][kFastNthDim] = {};
    };
    std::unique_ptr<LookupTable> fast_lookup_ = std::make_unique<LookupTable>();
    //@@end fast-lookup

    // Type name to code mappings
    std::unordered_map<std::string, uint16_t> types_;

    // Reverse mapping for type lookup
    std::unordered_map<uint16_t, std::string> typeCodeToName_;

    // Ledger entry type mappings
    std::unordered_map<std::string, uint16_t> ledgerEntryTypes_;

    // Transaction type mappings
    std::unordered_map<std::string, uint16_t> transactionTypes_;

    // Transaction result mappings
    std::unordered_map<std::string, int32_t> transactionResults_;

    // Granular permission mappings
    std::unordered_map<std::string, uint32_t> permissions_;

    // Field lookup indices for performance
    std::unordered_map<std::string, size_t> fieldNameIndex_;
    std::unordered_map<uint32_t, size_t> fieldCodeIndex_;  // key: field code

    void
    apply_load_options(const ProtocolOptions& opts);

    void
    add_table_field(
        std::string name,
        std::string_view type_name,
        std::int32_t nth,
        bool is_serialized,
        bool is_signing_field,
        bool is_vl_encoded);

    void
    finish_table_load(const ProtocolOptions& opts);

    // Build the fast lookup table after loading
    void
    build_fast_lookup();

    // Validate type during loading
    void
    validate_type(uint16_t type_code, const ProtocolOptions& opts);

    // Check if all fields of a type are VL-encoded
    bool
    can_infer_vl_type(uint16_t type_code) const;

    // Find known type in FieldTypes::ALL
    std::optional<FieldType>
    find_known_type(uint16_t type_code) const;
};

}  // namespace catl::xdata
