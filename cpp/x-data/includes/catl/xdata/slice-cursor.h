#pragma once

#include "catl/core/types.h"  // For Slice
#include "catl/xdata/codec-error.h"
#include <cstdint>
#include <expected>
#include <stdexcept>
#include <string>
#include <utility>  // For std::pair

namespace catl::xdata {

// Custom exception for SliceCursor errors
class SliceCursorError : public std::runtime_error
{
public:
    explicit SliceCursorError(const std::string& msg)
        : std::runtime_error("SliceCursor: " + msg)
    {
    }
};

inline CodecErrorValue
slice_cursor_error(std::string msg)
{
    return {CodecErrorCode::malformed_data, std::move(msg)};
}

// Cursor for tracking position in a Slice
struct SliceCursor
{
    Slice data;
    size_t pos = 0;

    bool
    empty() const
    {
        return pos >= data.size();
    }
    size_t
    remaining_size() const
    {
        return pos < data.size() ? data.size() - pos : 0;
    }
    Slice
    remaining() const
    {
        if (pos >= data.size())
        {
            return {};
        }
        return data.subslice(pos);
    }

    std::expected<uint8_t, CodecErrorValue>
    try_peek_u8() const
    {
        if (pos >= data.size())
        {
            return std::unexpected(
                slice_cursor_error("peek past end of data"));
        }
        return data.data()[pos];
    }

#ifndef CATL_XDATA_NO_THROWING_CURSOR
    uint8_t
    peek_u8() const
    {
        auto result = try_peek_u8();
        if (!result)
        {
            CATL_XDATA_THROW(SliceCursorError(result.error().message));
        }
        return *result;
    }
#endif

    std::expected<uint8_t, CodecErrorValue>
    try_read_u8()
    {
        if (pos >= data.size())
        {
            return std::unexpected(
                slice_cursor_error("read_u8 past end of data"));
        }
        return data.data()[pos++];
    }

#ifndef CATL_XDATA_NO_THROWING_CURSOR
    uint8_t
    read_u8()
    {
        auto result = try_read_u8();
        if (!result)
        {
            CATL_XDATA_THROW(SliceCursorError(result.error().message));
        }
        return *result;
    }
#endif

    std::expected<uint16_t, CodecErrorValue>
    try_read_uint16_be()
    {
        if (pos > data.size() || 2 > data.size() - pos)
        {
            return std::unexpected(
                slice_cursor_error("read_uint16_be past end of data"));
        }
        uint16_t result = (static_cast<uint16_t>(data.data()[pos]) << 8) |
            data.data()[pos + 1];
        pos += 2;
        return result;
    }

#ifndef CATL_XDATA_NO_THROWING_CURSOR
    uint16_t
    read_uint16_be()
    {
        auto result = try_read_uint16_be();
        if (!result)
        {
            CATL_XDATA_THROW(SliceCursorError(result.error().message));
        }
        return *result;
    }
#endif

    std::expected<uint32_t, CodecErrorValue>
    try_read_uint32_be()
    {
        if (pos > data.size() || 4 > data.size() - pos)
        {
            return std::unexpected(
                slice_cursor_error("read_uint32_be past end of data"));
        }
        uint32_t result = (static_cast<uint32_t>(data.data()[pos]) << 24) |
            (static_cast<uint32_t>(data.data()[pos + 1]) << 16) |
            (static_cast<uint32_t>(data.data()[pos + 2]) << 8) |
            data.data()[pos + 3];
        pos += 4;
        return result;
    }

#ifndef CATL_XDATA_NO_THROWING_CURSOR
    uint32_t
    read_uint32_be()
    {
        auto result = try_read_uint32_be();
        if (!result)
        {
            CATL_XDATA_THROW(SliceCursorError(result.error().message));
        }
        return *result;
    }
#endif

    std::expected<uint64_t, CodecErrorValue>
    try_read_uint64_be()
    {
        if (pos > data.size() || 8 > data.size() - pos)
        {
            return std::unexpected(
                slice_cursor_error("read_uint64_be past end of data"));
        }
        uint64_t result = 0;
        for (int i = 0; i < 8; ++i)
        {
            result = (result << 8) | data.data()[pos + i];
        }
        pos += 8;
        return result;
    }

#ifndef CATL_XDATA_NO_THROWING_CURSOR
    uint64_t
    read_uint64_be()
    {
        auto result = try_read_uint64_be();
        if (!result)
        {
            CATL_XDATA_THROW(SliceCursorError(result.error().message));
        }
        return *result;
    }
#endif

    std::expected<void, CodecErrorValue>
    try_advance(size_t n)
    {
        if (pos > data.size() || n > data.size() - pos)
        {
            return std::unexpected(slice_cursor_error(
                "attempted to advance " + std::to_string(n) +
                " bytes, only " + std::to_string(remaining_size()) +
                " available"));
        }
        pos += n;
        return {};
    }

#ifndef CATL_XDATA_NO_THROWING_CURSOR
    void
    advance(size_t n)
    {
        auto result = try_advance(n);
        if (!result)
        {
            CATL_XDATA_THROW(SliceCursorError(result.error().message));
        }
    }
#endif

    std::expected<Slice, CodecErrorValue>
    try_read_slice(size_t n)
    {
        if (pos > data.size() || n > data.size() - pos)
        {
            return std::unexpected(slice_cursor_error(
                "attempted to read " + std::to_string(n) + " bytes, only " +
                std::to_string(remaining_size()) + " available"));
        }
        Slice result(data.data() + pos, n);
        pos += n;
        return result;
    }

#ifndef CATL_XDATA_NO_THROWING_CURSOR
    Slice
    read_slice(size_t n)
    {
        auto result = try_read_slice(n);
        if (!result)
        {
            CATL_XDATA_THROW(SliceCursorError(result.error().message));
        }
        return *result;
    }
#endif
};

inline std::expected<std::pair<Slice, uint32_t>, CodecErrorValue>
try_read_field_header(SliceCursor& cursor)
{
    size_t start_pos = cursor.pos;

    if (cursor.empty())
    {
        return std::pair<Slice, uint32_t>{Slice{}, 0};
    }

    // Read the tag byte
    auto byte1_result = cursor.try_read_u8();
    if (!byte1_result)
    {
        return std::unexpected(byte1_result.error());
    }
    uint8_t byte1 = *byte1_result;

    // Extract type bits from upper 4 bits
    uint32_t type = byte1 >> 4;

    // Extract field bits from lower 4 bits
    uint32_t field = byte1 & 0x0F;

    // If type bits are 0, type code is in next byte.
    // Empty cursor before this tag is EOF (field_code 0). A consumed
    // long-form type/name out of range is an error, never 0.
    // xahaud-vectors:src/libxrpl/protocol/Serializer.cpp:422
    if (type == 0)
    {
        auto type_result = cursor.try_read_u8();
        if (!type_result)
        {
            return std::unexpected(
                slice_cursor_error("truncated field type"));
        }
        type = *type_result;
        if (type < 16)
        {
            return std::unexpected(slice_cursor_error(
                "gFID: uncommon type out of range " +
                std::to_string(type)));
        }
    }

    // xahaud-vectors:src/libxrpl/protocol/Serializer.cpp:436
    if (field == 0)
    {
        auto field_result = cursor.try_read_u8();
        if (!field_result)
        {
            return std::unexpected(
                slice_cursor_error("truncated field id"));
        }
        field = *field_result;
        if (field < 16)
        {
            return std::unexpected(slice_cursor_error(
                "gFID: uncommon name out of range " +
                std::to_string(field)));
        }
    }

    // Create slice containing the entire field header
    size_t header_size = cursor.pos - start_pos;
    Slice header_slice(cursor.data.data() + start_pos, header_size);

    // Return header slice and combined field code
    return std::pair<Slice, uint32_t>{header_slice, (type << 16) | field};
}

// Read field header and return field code with the header bytes.
// Returns: pair of (field header slice, field code).
// field_code 0 means end-of-input (empty cursor before a tag), not an
// illegal long-form header — those throw.
#ifndef CATL_XDATA_NO_THROWING_CURSOR
inline std::pair<Slice, uint32_t>
read_field_header(SliceCursor& cursor)
{
    auto result = try_read_field_header(cursor);
    if (!result)
    {
        CATL_XDATA_THROW(SliceCursorError(result.error().message));
    }
    return *result;
}
#endif

// Read variable length prefix
inline std::expected<size_t, CodecErrorValue>
try_read_vl_length(SliceCursor& cursor)
{
    auto byte1_result = cursor.try_read_u8();
    if (!byte1_result)
    {
        return std::unexpected(byte1_result.error());
    }
    uint8_t byte1 = *byte1_result;

    if (byte1 <= 192)
    {
        return byte1;
    }
    else if (byte1 <= 240)
    {
        auto byte2_result = cursor.try_read_u8();
        if (!byte2_result)
        {
            return std::unexpected(byte2_result.error());
        }
        uint8_t byte2 = *byte2_result;
        return 193 + ((byte1 - 193) * 256) + byte2;
    }
    else if (byte1 <= 254)
    {
        auto byte2_result = cursor.try_read_u8();
        if (!byte2_result)
        {
            return std::unexpected(byte2_result.error());
        }
        auto byte3_result = cursor.try_read_u8();
        if (!byte3_result)
        {
            return std::unexpected(byte3_result.error());
        }
        uint8_t byte2 = *byte2_result;
        uint8_t byte3 = *byte3_result;
        return 12481 + ((byte1 - 241) * 65536) + (byte2 * 256) + byte3;
    }

    // Invalid VL encoding
    return std::unexpected(slice_cursor_error(
        "Invalid VL encoding: first byte = " +
        std::to_string(static_cast<int>(byte1))));
}

#ifndef CATL_XDATA_NO_THROWING_CURSOR
inline size_t
read_vl_length(SliceCursor& cursor)
{
    auto result = try_read_vl_length(cursor);
    if (!result)
    {
        CATL_XDATA_THROW(SliceCursorError(result.error().message));
    }
    return *result;
}
#endif

}  // namespace catl::xdata
