#include "catl/xdata/protocol.h"
#include "catl/xdata/exception_policy.h"

#include <boost/json.hpp>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace json = boost::json;

namespace catl::xdata {

Protocol
Protocol::load_from_file(const std::string& path, const ProtocolOptions& opts)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        CATL_XDATA_THROW(std::runtime_error(
            "Failed to open protocol file: " + path));
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    boost::system::error_code ec;
    json::value jv = json::parse(content, ec);
    if (ec)
    {
        CATL_XDATA_THROW(std::runtime_error(
            "Failed to parse JSON: " + ec.message()));
    }

    return load_from_json_value(jv, opts);
}

Protocol
Protocol::load_from_json_value(
    const json::value& jv,
    const ProtocolOptions& opts)
{
    json::value json_value = jv;

    if (json_value.is_object() && json_value.as_object().contains("result"))
    {
        json_value = json_value.at("result");
    }

    if (!json_value.is_object())
    {
        CATL_XDATA_THROW(std::runtime_error(
            "Protocol JSON must be an object"));
    }

    Protocol protocol;
    protocol.network_id_ = opts.network_id.has_value()
        ? std::optional<uint32_t>(
              Networks::find_base_network_id(opts.network_id.value()))
        : std::nullopt;
    protocol.expand_xaddresses_ = opts.expand_xaddresses;
    const auto& obj = json_value.as_object();

    if (obj.contains("TYPES"))
    {
        const auto& types = obj.at("TYPES").as_object();
        for (const auto& [key, value] : types)
        {
            uint16_t code = static_cast<uint16_t>(value.as_int64());
            std::string name(key);
            protocol.types_[name] = code;
            protocol.typeCodeToName_[code] = name;
        }
    }

    if (!obj.contains("FIELDS"))
    {
        CATL_XDATA_THROW(std::runtime_error(
            "Protocol JSON must contain FIELDS array"));
    }

    const auto& fields = obj.at("FIELDS").as_array();

    for (const auto& field : fields)
    {
        const auto& fieldArray = field.as_array();
        if (fieldArray.size() != 2)
        {
            CATL_XDATA_THROW(std::runtime_error(
                "Field definition must be a 2-element array"));
        }

        FieldDef def;
        def.name = fieldArray[0].as_string().c_str();

        const auto& metadata = fieldArray[1].as_object();
        def.meta.is_serialized = metadata.at("isSerialized").as_bool();
        def.meta.is_signing_field = metadata.at("isSigningField").as_bool();
        def.meta.is_vl_encoded = metadata.at("isVLEncoded").as_bool();
        def.meta.nth = static_cast<uint16_t>(metadata.at("nth").as_int64());

        auto typeName = metadata.at("type").as_string();
        if (auto ft = FieldTypes::from_name(typeName))
        {
            def.meta.type = *ft;
        }
        else
        {
            auto typeCode = protocol.get_type_code(std::string(typeName));
            if (typeCode)
            {
                def.meta.type = FieldType{typeName, *typeCode};
            }
            else
            {
                CATL_XDATA_THROW(std::runtime_error(
                    "Field references unknown type: " + std::string(typeName) +
                    " (not in TYPES mapping)"));
            }
        }

        protocol.fieldNameIndex_[def.name] = protocol.fields_.size();
        def.code = make_field_code(def.meta.type.code, def.meta.nth);
        def.compute_header_size();
        protocol.fieldCodeIndex_.try_emplace(
            def.code, protocol.fields_.size());
        protocol.fields_.push_back(std::move(def));
    }

    protocol.build_fast_lookup();

    ProtocolOptions normalized_opts = opts;
    normalized_opts.network_id = protocol.network_id_;

    for (const auto& [code, name] : protocol.typeCodeToName_)
    {
        protocol.validate_type(code, normalized_opts);
    }

    if (obj.contains("LEDGER_ENTRY_TYPES"))
    {
        const auto& types = obj.at("LEDGER_ENTRY_TYPES").as_object();
        for (const auto& [key, value] : types)
        {
            protocol.ledgerEntryTypes_[std::string(key)] =
                static_cast<uint16_t>(value.as_int64());
        }
    }

    if (obj.contains("TRANSACTION_TYPES"))
    {
        const auto& types = obj.at("TRANSACTION_TYPES").as_object();
        for (const auto& [key, value] : types)
        {
            protocol.transactionTypes_[std::string(key)] =
                static_cast<uint16_t>(value.as_int64());
        }
    }

    if (obj.contains("TRANSACTION_RESULTS"))
    {
        const auto& results = obj.at("TRANSACTION_RESULTS").as_object();
        for (const auto& [key, value] : results)
        {
            protocol.transactionResults_[std::string(key)] =
                static_cast<int32_t>(value.as_int64());
        }
    }

    if (obj.contains("PERMISSIONS"))
    {
        const auto& perms = obj.at("PERMISSIONS").as_object();
        for (const auto& [key, value] : perms)
        {
            protocol.permissions_[std::string(key)] =
                static_cast<uint32_t>(value.as_int64());
        }
    }

    for (const auto& [name, code] : protocol.transactionTypes_)
    {
        protocol.permissions_[name] = static_cast<uint32_t>(code) + 1;
    }

    return protocol;
}

}  // namespace catl::xdata
