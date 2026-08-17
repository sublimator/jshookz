#pragma once

#include <cstdint>

namespace catl::xdata {

// Rows emitted by scripts/generate_definitions.py. Included by the generated
// headers and embedded_protocol.cpp only — not by protocol.h.

struct ProtocolTableField
{
    char const* name;
    char const* type_name;
    std::int32_t nth;
    bool is_serialized;
    bool is_signing_field;
    bool is_vl_encoded;
};

struct ProtocolTableNameCode
{
    char const* name;
    std::int32_t code;
};

}  // namespace catl::xdata
