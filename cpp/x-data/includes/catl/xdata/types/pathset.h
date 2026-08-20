#pragma once

#include "catl/xdata/parser-context.h"
#include <cstdint>

namespace catl::xdata {

// PathSet protocol constants
namespace PathSet {
constexpr uint8_t END_BYTE = 0x00;
constexpr uint8_t PATH_SEPARATOR = 0xFF;
constexpr uint8_t TYPE_ACCOUNT = 0x01;
constexpr uint8_t TYPE_CURRENCY = 0x10;
constexpr uint8_t TYPE_ISSUER = 0x20;
}  // namespace PathSet

// Skip a PathSet (has its own termination protocol)
inline void
skip_pathset(ParserContext& ctx)
{
    while (!ctx.failed() && !ctx.empty())
    {
        uint8_t type_byte = 0;
        if (!ctx.read_u8(type_byte))
            return;

        if (type_byte == PathSet::END_BYTE)
            break;

        if (type_byte == PathSet::PATH_SEPARATOR)
            continue;

        if (type_byte & PathSet::TYPE_ACCOUNT)
        {
            if (!ctx.advance(20))
                return;
        }
        if (type_byte & PathSet::TYPE_CURRENCY)
        {
            if (!ctx.advance(20))
                return;
        }
        if (type_byte & PathSet::TYPE_ISSUER)
        {
            if (!ctx.advance(20))
                return;
        }
    }
}

}  // namespace catl::xdata
