#pragma once

#include "catl/xdata/codec-error.h"
#include "catl/xdata/slice-cursor.h"

#include <cstdint>
#include <string>

namespace catl::xdata {

// Sticky first-failure parser state. Allocation-free: diagnostic strings are
// static literals, formatted only by the outer adapter.
struct ParserContext
{
    SliceCursor cursor;
    bool failed_ = false;
    CodecErrorCode code_ = CodecErrorCode::malformed_data;
    uint32_t fail_offset_ = 0;
    char const* lit_ = "";
    uint32_t aux_ = 0;
    bool has_aux_ = false;

    explicit ParserContext(Slice const& data) : cursor{data, 0} {}
    explicit ParserContext(SliceCursor const& c) : cursor(c) {}

    void
    reset(Slice const& data, uint32_t begin = 0)
    {
        cursor = SliceCursor{data, begin};
        failed_ = false;
        code_ = CodecErrorCode::malformed_data;
        fail_offset_ = 0;
        lit_ = "";
        aux_ = 0;
        has_aux_ = false;
    }

    bool
    failed() const noexcept
    {
        return failed_;
    }

    uint32_t
    fail_offset() const noexcept
    {
        return fail_offset_;
    }

    void
    fail(char const* lit, uint32_t aux = 0, bool has_aux = false) noexcept
    {
        if (failed_)
            return;
        failed_ = true;
        code_ = CodecErrorCode::malformed_data;
        fail_offset_ = cursor.pos > 0xffffffffu
            ? 0xffffffffu
            : static_cast<uint32_t>(cursor.pos);
        lit_ = lit ? lit : "";
        aux_ = aux;
        has_aux_ = has_aux;
    }

    CodecErrorValue
    as_error() const
    {
        std::string msg = lit_;
        if (has_aux_)
            msg += std::to_string(aux_);
        return {code_, std::move(msg)};
    }

    bool
    empty() const noexcept
    {
        return failed_ || cursor.empty();
    }

    size_t
    remaining() const noexcept
    {
        return failed_ ? 0 : cursor.remaining_size();
    }

    size_t
    pos() const noexcept
    {
        return cursor.pos;
    }

    uint8_t const*
    at() const noexcept
    {
        return cursor.data.data() + cursor.pos;
    }

    bool
    peek_u8(uint8_t& out) noexcept
    {
        if (failed_)
            return false;
        if (cursor.empty())
        {
            fail("peek past end of data");
            return false;
        }
        out = cursor.data.data()[cursor.pos];
        return true;
    }

    bool
    read_u8(uint8_t& out) noexcept
    {
        if (failed_)
            return false;
        if (cursor.empty())
        {
            fail("read_u8 past end of data");
            return false;
        }
        out = cursor.data.data()[cursor.pos];
        ++cursor.pos;
        return true;
    }

    bool
    advance(size_t n) noexcept
    {
        if (failed_)
            return false;
        if (cursor.pos > cursor.data.size() ||
            n > cursor.data.size() - cursor.pos)
        {
            fail("truncated field");
            return false;
        }
        cursor.pos += n;
        return true;
    }

    // 0 means end-of-input or already failed. Illegal long-form fails sticky.
    uint32_t
    read_field_code() noexcept
    {
        if (failed_)
            return 0;
        if (cursor.empty())
            return 0;
        uint8_t byte1 = 0;
        if (!read_u8(byte1))
            return 0;
        uint32_t type = byte1 >> 4;
        uint32_t field = byte1 & 0x0F;
        // xahaud-vectors:src/libxrpl/protocol/Serializer.cpp:422
        if (type == 0)
        {
            uint8_t t = 0;
            if (!read_u8(t))
            {
                if (!failed_)
                    fail("truncated field type");
                return 0;
            }
            type = t;
            if (type < 16)
            {
                fail("gFID: uncommon type out of range ", type, true);
                return 0;
            }
        }
        // xahaud-vectors:src/libxrpl/protocol/Serializer.cpp:436
        if (field == 0)
        {
            uint8_t f = 0;
            if (!read_u8(f))
            {
                if (!failed_)
                    fail("truncated field id");
                return 0;
            }
            field = f;
            if (field < 16)
            {
                fail("gFID: uncommon name out of range ", field, true);
                return 0;
            }
        }
        return (type << 16) | field;
    }

    bool
    read_vl_length(size_t& out) noexcept
    {
        uint8_t byte1 = 0;
        if (!read_u8(byte1))
            return false;
        if (byte1 <= 192)
        {
            out = byte1;
            return true;
        }
        if (byte1 <= 240)
        {
            uint8_t byte2 = 0;
            if (!read_u8(byte2))
                return false;
            out = 193 + ((static_cast<size_t>(byte1) - 193) * 256) + byte2;
            return true;
        }
        if (byte1 <= 254)
        {
            uint8_t byte2 = 0;
            uint8_t byte3 = 0;
            if (!read_u8(byte2) || !read_u8(byte3))
                return false;
            out = 12481 + ((static_cast<size_t>(byte1) - 241) * 65536) +
                (static_cast<size_t>(byte2) * 256) + byte3;
            return true;
        }
        fail("Invalid VL encoding");
        return false;
    }
};

}  // namespace catl::xdata
