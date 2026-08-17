#pragma once

#include "catl/xdata/codec-error.h"
#include "catl/xdata/hex.h"
#include "catl/xdata/serializer.h"
#ifndef CATL_XDATA_NO_BOOST_JSON
#include <boost/json.hpp>
#endif
#include <cstring>

namespace catl::xdata::codecs {

struct CurrencyCodec
{
    static constexpr size_t fixed_size = 20;

#ifndef CATL_XDATA_NO_BOOST_JSON
    static size_t
    encoded_size(boost::json::value const&)
    {
        return 20;
    }
#endif

    // From raw 20 bytes
    template <ByteSink Sink>
    static void
    encode(Serializer<Sink>& s, std::span<const uint8_t, 20> data)
    {
        s.add_raw(std::span<const uint8_t>{data.data(), 20});
    }

    // From string — errors-as-values core.
    template <ByteSink Sink>
    static std::expected<void, CodecErrorValue>
    encode_expected(
        Serializer<Sink>& s,
        std::string_view currency,
        std::string const& path = {})
    {
        uint8_t buf[20] = {};

        if (currency == "XRP" || currency == "XAH" || currency.empty())
        {
            s.add_raw(std::span<const uint8_t>{buf, 20});
        }
        else if (currency.size() == 40)
        {
            if (auto r = try_require_hex_length(currency, 40, "Currency", path);
                !r)
                return std::unexpected(std::move(r.error()));
            s.add_hex(currency);
        }
        else if (currency.size() == 3)
        {
            buf[12] = static_cast<uint8_t>(currency[0]);
            buf[13] = static_cast<uint8_t>(currency[1]);
            buf[14] = static_cast<uint8_t>(currency[2]);
            s.add_raw(std::span<const uint8_t>{buf, 20});
        }
        else
        {
            return encode_error(
                CodecErrorCode::invalid_value,
                "Currency",
                "invalid currency: " + std::string(currency),
                path);
        }
        return {};
    }

    // Throwing facade for native / existing callers.
    template <ByteSink Sink>
    static void
    encode(
        Serializer<Sink>& s,
        std::string_view currency,
        std::string const& path = {})
    {
        encode_or_throw(encode_expected(s, currency, path));
    }

#ifndef CATL_XDATA_NO_BOOST_JSON
    template <ByteSink Sink>
    static void
    encode(
        Serializer<Sink>& s,
        boost::json::value const& v,
        std::string const& path = {})
    {
        if (!v.is_string())
        {
            CATL_XDATA_THROW(EncodeError(
                CodecErrorCode::invalid_value,
                "Currency",
                "expected string",
                path));
        }
        encode(s, std::string_view(v.as_string()), path);
    }
#endif

    static std::string
    decode_string(Slice const& data)
    {
        if (data.size() != 20)
            return hex_encode(data);

        bool all_zeros = true;
        for (size_t i = 0; i < 20; ++i)
        {
            if (data.data()[i] != 0)
            {
                all_zeros = false;
                break;
            }
        }
        if (all_zeros)
            return "XRP";

        bool standard = true;
        for (size_t i = 0; i < 12; ++i)
        {
            if (data.data()[i] != 0)
            {
                standard = false;
                break;
            }
        }
        if (standard)
        {
            for (size_t i = 15; i < 20; ++i)
            {
                if (data.data()[i] != 0)
                {
                    standard = false;
                    break;
                }
            }
        }
        if (standard)
        {
            std::string code;
            for (size_t i = 12; i < 15; ++i)
            {
                if (data.data()[i] != 0)
                    code += static_cast<char>(data.data()[i]);
            }
            return code;
        }

        return hex_encode(data);
    }

#ifndef CATL_XDATA_NO_BOOST_JSON
    static boost::json::value
    decode(Slice const& data)
    {
        if (data.size() != 20)
            return boost::json::string(hex_encode(data));

        // All zeros → native
        bool all_zeros = true;
        for (size_t i = 0; i < 20; ++i)
        {
            if (data.data()[i] != 0)
            {
                all_zeros = false;
                break;
            }
        }
        if (all_zeros)
            return boost::json::string("XRP");

        // Standard 3-char code: 12 zeros + 3 chars + 5 zeros
        bool standard = true;
        for (size_t i = 0; i < 12; ++i)
        {
            if (data.data()[i] != 0)
            {
                standard = false;
                break;
            }
        }
        if (standard)
        {
            for (size_t i = 15; i < 20; ++i)
            {
                if (data.data()[i] != 0)
                {
                    standard = false;
                    break;
                }
            }
        }
        if (standard)
        {
            std::string code;
            for (size_t i = 12; i < 15; ++i)
            {
                if (data.data()[i] != 0)
                    code += static_cast<char>(data.data()[i]);
            }
            return boost::json::string(code);
        }

        return boost::json::string(hex_encode(data));
    }
#endif
};

}  // namespace catl::xdata::codecs
