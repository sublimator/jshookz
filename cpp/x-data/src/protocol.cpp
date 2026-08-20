#include "catl/xdata/protocol.h"
#include "catl/xdata/exception_policy.h"
#include <cstring>  // for std::memset
#include <iostream>  // for logging
#include <stdexcept>

namespace catl::xdata {

void
Protocol::apply_load_options(const ProtocolOptions& opts)
{
    network_id_ = opts.network_id.has_value()
        ? std::optional<uint32_t>(
              Networks::find_base_network_id(opts.network_id.value()))
        : std::nullopt;
    expand_xaddresses_ = opts.expand_xaddresses;
}

void
Protocol::add_table_field(
    std::string name,
    std::string_view type_name,
    std::int32_t nth,
    bool is_serialized,
    bool is_signing_field,
    bool is_vl_encoded)
{
    FieldDef def;
    def.name = std::move(name);
    def.meta.is_serialized = is_serialized;
    def.meta.is_signing_field = is_signing_field;
    def.meta.is_vl_encoded = is_vl_encoded;
    def.meta.nth = static_cast<uint16_t>(nth);

    if (auto ft = FieldTypes::from_name(type_name))
    {
        def.meta.type = *ft;
    }
    else
    {
        auto typeCode = get_type_code(std::string(type_name));
        if (typeCode)
        {
            def.meta.type = FieldType{type_name, *typeCode};
        }
        else
        {
            CATL_XDATA_THROW(std::runtime_error(
                "Field references unknown type: " + std::string(type_name) +
                " (not in TYPES mapping)"));
        }
    }

    fieldNameIndex_[def.name] = fields_.size();
    def.code = make_field_code(def.meta.type.code, def.meta.nth);
    def.compute_header_size();
    fieldCodeIndex_.try_emplace(def.code, fields_.size());
    fields_.push_back(std::move(def));
}

void
Protocol::finish_table_load(const ProtocolOptions& opts)
{
    build_fast_lookup();

    ProtocolOptions normalized_opts = opts;
    normalized_opts.network_id = network_id_;
    for (const auto& [code, name] : typeCodeToName_)
    {
        validate_type(code, normalized_opts);
    }

    for (const auto& [name, code] : transactionTypes_)
    {
        permissions_[name] = static_cast<uint32_t>(code) + 1;
    }
}

void
Protocol::validate_type(uint16_t type_code, const ProtocolOptions& opts)
{
    auto known_type = find_known_type(type_code);

    if (!known_type)
    {
        // Unknown type - check if we can safely infer VL encoding
        if (opts.allow_vl_inference && can_infer_vl_type(type_code))
        {
            std::cerr << "[INFO] Inferred type " << type_code
                      << " as VL-encoded based on field metadata\n";
            inferred_vl_types_.insert(type_code);
        }
        else
        {
            CATL_XDATA_THROW(std::runtime_error(
                "Unknown type " + std::to_string(type_code) +
                " - cannot parse safely. All fields of this type must have "
                "is_vl_encoded=true to continue."));
        }
    }
    else if (opts.network_id.has_value())
    {
        // Known type - verify network compatibility
        // Note: network_id is already normalized to base in
        // apply_load_options
        if (!known_type->matches_network(opts.network_id.value()))
        {
            CATL_XDATA_THROW(std::runtime_error(
                "Type " + std::string(known_type->name) + " (code " +
                std::to_string(type_code) + ") not valid for network " +
                std::to_string(opts.network_id.value())));
        }
    }
}

bool
Protocol::can_infer_vl_type(uint16_t type_code) const
{
    // This should only be called AFTER all fields are loaded
    if (fields_.empty())
    {
        CATL_XDATA_THROW(std::logic_error(
            "can_infer_vl_type called before fields are loaded"));
    }

    size_t vl_count = 0, total_count = 0;

    for (const auto& field : fields_)
    {
        if (field.meta.type.code == type_code)
        {
            total_count++;
            if (field.meta.is_vl_encoded)
                vl_count++;
        }
    }

    // Safe ONLY if ALL fields of this type are VL-encoded
    return total_count > 0 && vl_count == total_count;
}

std::optional<FieldType>
Protocol::find_known_type(uint16_t type_code) const
{
    // Search through all known types
    for (const auto& ft : FieldTypes::ALL)
    {
        if (ft.code == type_code)
        {
            return ft;
        }
    }
    return std::nullopt;
}

std::optional<FieldDef>
Protocol::find_field(const std::string& name) const
{
    auto it = fieldNameIndex_.find(name);
    if (it != fieldNameIndex_.end())
    {
        return fields_[it->second];
    }
    return std::nullopt;
}

std::optional<FieldDef>
Protocol::get_field(const std::string& type, uint16_t field_id) const
{
    // Convert type name to FieldType
    auto field_type = FieldTypes::from_name(type);
    if (!field_type)
    {
        return std::nullopt;
    }

    uint32_t field_code = make_field_code(field_type->code, field_id);
    return get_field_by_code_opt(field_code);
}

const FieldDef*
Protocol::get_field_by_code(uint32_t field_code) const
{
    uint16_t type_code = get_field_type_code(field_code);
    uint16_t field_id = get_field_id(field_code);

    if (type_code < Protocol::kFastTypeDim &&
        field_id < Protocol::kFastNthDim)
    {
        return fast_lookup_->data[type_code][field_id];
    }

    // Slow path for rare cases
    auto it = fieldCodeIndex_.find(field_code);
    if (it != fieldCodeIndex_.end())
    {
        return &fields_[it->second];
    }
    return nullptr;
}

std::optional<FieldDef>
Protocol::get_field_by_code_opt(uint32_t field_code) const
{
    const FieldDef* field = get_field_by_code(field_code);
    if (field)
    {
        return *field;
    }
    return std::nullopt;
}

std::optional<uint16_t>
Protocol::get_type_code(const std::string& typeName) const
{
    auto it = types_.find(typeName);
    if (it != types_.end())
    {
        return it->second;
    }
    return std::nullopt;
}

std::optional<std::string>
Protocol::get_type_name(uint16_t typeCode) const
{
    auto it = typeCodeToName_.find(typeCode);
    if (it != typeCodeToName_.end())
    {
        return it->second;
    }
    return std::nullopt;
}

std::optional<std::string>
Protocol::get_transaction_type_name(uint16_t txTypeCode) const
{
    // Reverse lookup in transactionTypes_
    for (const auto& [name, code] : transactionTypes_)
    {
        if (code == txTypeCode)
        {
            return name;
        }
    }
    return std::nullopt;
}

std::optional<std::string>
Protocol::get_ledger_entry_type_name(uint16_t leTypeCode) const
{
    // Reverse lookup in ledgerEntryTypes_
    for (const auto& [name, code] : ledgerEntryTypes_)
    {
        if (code == leTypeCode)
        {
            return name;
        }
    }
    return std::nullopt;
}

void
Protocol::build_fast_lookup()
{
    std::memset(fast_lookup_->data, 0, sizeof(fast_lookup_->data));
    max_serialized_type_code_ = 0;
    max_serialized_nth_ = 0;
    fast_lookup_fallback_count_ = 0;

    for (const auto& field : fields_)
    {
        uint16_t type_code = get_field_type_code(field.code);
        uint16_t field_id = get_field_id(field.code);
        if (type_code > max_serialized_type_code_ && type_code < 10000)
            max_serialized_type_code_ = type_code;
        if (field_id > max_serialized_nth_ && field_id < 10000)
            max_serialized_nth_ = field_id;

        if (type_code < Protocol::kFastTypeDim &&
            field_id < Protocol::kFastNthDim)
        {
            // First writer wins — don't let duplicates (e.g. "hash"
            // nth=1) overwrite real fields (e.g. "LedgerHash" nth=1).
            if (!fast_lookup_->data[type_code][field_id])
                fast_lookup_->data[type_code][field_id] = &field;
        }
        else
        {
            ++fast_lookup_fallback_count_;
        }
    }
}

}  // namespace catl::xdata
