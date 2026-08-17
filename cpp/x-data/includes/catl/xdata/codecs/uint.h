#pragma once

#include "catl/xdata/codec-error.h"
#include "catl/xdata/serializer.h"
#ifndef CATL_XDATA_NO_BOOST_JSON
#include <boost/json.hpp>
#endif
#include <string>

namespace catl::xdata::codecs {

struct UInt8Codec
{
    static constexpr size_t fixed_size = 1;

#ifndef CATL_XDATA_NO_BOOST_JSON
    static size_t
    encoded_size(boost::json::value const&)
    {
        return 1;
    }
#endif

    template <ByteSink Sink>
    static void
    encode(Serializer<Sink>& s, uint8_t v)
    {
        s.add_u8(v);
    }

    static uint8_t
    decode_raw(Slice const& data)
    {
        return data.data()[0];
    }

#ifndef CATL_XDATA_NO_BOOST_JSON
    template <ByteSink Sink>
    static void
    encode(Serializer<Sink>& s, boost::json::value const& v)
    {
        s.add_u8(static_cast<uint8_t>(
            v.is_uint64() ? v.as_uint64()
                          : static_cast<uint64_t>(v.as_int64())));
    }

    static boost::json::value
    decode(Slice const& data)
    {
        return static_cast<std::uint64_t>(decode_raw(data));
    }
#endif
};

struct UInt16Codec
{
    static constexpr size_t fixed_size = 2;

#ifndef CATL_XDATA_NO_BOOST_JSON
    static size_t
    encoded_size(boost::json::value const&)
    {
        return 2;
    }
#endif

    template <ByteSink Sink>
    static void
    encode(Serializer<Sink>& s, uint16_t v)
    {
        s.add_u16(v);
    }

#ifndef CATL_XDATA_NO_BOOST_JSON
    template <ByteSink Sink>
    static void
    encode(Serializer<Sink>& s, boost::json::value const& v)
    {
        s.add_u16(static_cast<uint16_t>(
            v.is_uint64() ? v.as_uint64()
                          : static_cast<uint64_t>(v.as_int64())));
    }
#endif

    // Decode to raw uint16. Enum resolution (TransactionType etc.) is
    // handled by the dispatch layer which has Protocol access.
    static uint16_t
    decode_raw(Slice const& data)
    {
        return (static_cast<uint16_t>(data.data()[0]) << 8) |
            static_cast<uint16_t>(data.data()[1]);
    }

#ifndef CATL_XDATA_NO_BOOST_JSON
    static boost::json::value
    decode(Slice const& data)
    {
        return static_cast<std::uint64_t>(decode_raw(data));
    }
#endif
};

struct UInt32Codec
{
    static constexpr size_t fixed_size = 4;

#ifndef CATL_XDATA_NO_BOOST_JSON
    static size_t
    encoded_size(boost::json::value const&)
    {
        return 4;
    }
#endif

    template <ByteSink Sink>
    static void
    encode(Serializer<Sink>& s, uint32_t v)
    {
        s.add_u32(v);
    }

    static uint32_t
    decode_raw(Slice const& data)
    {
        return (static_cast<uint32_t>(data.data()[0]) << 24) |
            (static_cast<uint32_t>(data.data()[1]) << 16) |
            (static_cast<uint32_t>(data.data()[2]) << 8) |
            static_cast<uint32_t>(data.data()[3]);
    }

#ifndef CATL_XDATA_NO_BOOST_JSON
    template <ByteSink Sink>
    static void
    encode(Serializer<Sink>& s, boost::json::value const& v)
    {
        s.add_u32(static_cast<uint32_t>(
            v.is_uint64() ? v.as_uint64()
                          : static_cast<uint64_t>(v.as_int64())));
    }

    static boost::json::value
    decode(Slice const& data)
    {
        return static_cast<std::uint64_t>(decode_raw(data));
    }
#endif
};

struct UInt64Codec
{
    static constexpr size_t fixed_size = 8;

#ifndef CATL_XDATA_NO_BOOST_JSON
    static size_t
    encoded_size(boost::json::value const&)
    {
        return 8;
    }
#endif

    template <ByteSink Sink>
    static void
    encode(Serializer<Sink>& s, uint64_t v)
    {
        s.add_u64(v);
    }

    // JSON UInt64 is a hex string (e.g. "5003BAF82D03A000")
    //
    // Delegates to the errors-as-values path rather than parsing here, so the
    // boost::json (CLI/diagnostic) encoder and the JS encoder cannot disagree.
    // They used to: the tolerant parse_hex_uint64 checked only from_chars'
    // error code and never that the whole string was consumed, so "1234GG"
    // silently encoded as 0x1234 and a 17-char "00000000000000001" as 0x1,
    // while the JS path rejected both (issue 0010).
#ifndef CATL_XDATA_NO_BOOST_JSON
    template <ByteSink Sink>
    static void
    encode(Serializer<Sink>& s, boost::json::value const& v)
    {
        encode_hex(s, std::string_view(v.as_string()));
    }
#endif

    // From hex string (errors-as-values core)
    template <ByteSink Sink>
    static std::expected<void, CodecErrorValue>
    encode_hex_expected(Serializer<Sink>& s, std::string_view hex)
    {
        auto v = try_parse_hex_uint64(hex, "UInt64");
        if (!v)
            return std::unexpected(std::move(v.error()));
        s.add_u64(*v);
        return {};
    }

    // From hex string (throwing facade)
    template <ByteSink Sink>
    static void
    encode_hex(Serializer<Sink>& s, std::string_view hex)
    {
        encode_or_throw(encode_hex_expected(s, hex));
    }

    // From decimal string (for SPECIAL_FIELDS like MaximumAmount)
    template <ByteSink Sink>
    static void
    encode_decimal(Serializer<Sink>& s, std::string_view decimal)
    {
        s.add_u64(parse_uint64(decimal, "UInt64"));
    }

    // Decode to hex string (matching xrpl-py/xrpl.js)
#ifndef CATL_XDATA_NO_BOOST_JSON
    static boost::json::value
    decode(Slice const& data)
    {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
        {
            v = (v << 8) | data.data()[i];
        }
        char buf[17];
        std::snprintf(
            buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(v));
        return boost::json::string(buf);
    }
#endif
};

}  // namespace catl::xdata::codecs
