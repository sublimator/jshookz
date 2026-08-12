#pragma once

#include "catl/xdata/types.h"
#include <string>
#include <cstdint>

namespace catl::xdata {

// Forward declaration
class TypeParser;

// Field metadata matching TypeScript interface
struct FieldMeta {
    bool is_serialized;
    bool is_signing_field;
    bool is_vl_encoded;
    uint16_t nth;  // field ID within its type
    FieldType type;  // The actual type (was string)
};

// Field definition with name and metadata
struct FieldDef {
    std::string name;
    FieldMeta meta;
    uint32_t code;      // Combined type code (upper 16 bits) and field ID (lower 16 bits)
    uint8_t header_size; // 1, 2, or 3 — precomputed from type_code and field_id

    // Compute header_size from the code (call after code is set)
    void compute_header_size() {
        uint16_t tc = static_cast<uint16_t>(code >> 16);
        uint16_t fc = static_cast<uint16_t>(code & 0xFFFF);
        if (tc < 16 && fc < 16) header_size = 1;
        else if (tc >= 16 && fc >= 16) header_size = 3;
        else header_size = 2;
    }
};

// Helper to calculate field code from type and field ID
inline uint32_t make_field_code(uint16_t typeCode, uint16_t fieldId) {
    return (static_cast<uint32_t>(typeCode) << 16) | fieldId;
}

// Helper to extract type code from field code
inline uint16_t get_field_type_code(uint32_t fieldCode) {
    return static_cast<uint16_t>(fieldCode >> 16);
}

// Helper to extract field ID from field code
inline uint16_t get_field_id(uint32_t fieldCode) {
    return static_cast<uint16_t>(fieldCode & 0xFFFF);
}

} // namespace catl::xdata
