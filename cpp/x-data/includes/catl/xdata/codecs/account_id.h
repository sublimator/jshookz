#pragma once

#include "catl/base58/base58.h"
#include "catl/xdata/codec-error.h"
#include "catl/xdata/serializer.h"
#ifndef CATL_XDATA_NO_BOOST_JSON
#include <boost/json.hpp>
#endif
#include <span>
#include <vector>

namespace catl::xdata::codecs {

struct AccountIDCodec
{
    static constexpr size_t fixed_size = 20;

    static constexpr std::string_view ZERO_ACCOUNT_B58 =
        "rrrrrrrrrrrrrrrrrrrrrhoLvTp";

    // rippled's rule, from libxrpl/protocol/STAccount.cpp:
    //
    //     int const size = isDefault() ? 0 : uint160::bytes;
    //     s.addVL(value_.data(), size);
    //
    // A default (all-zero) account still gets its field header and its VL
    // prefix; only the payload is empty. That applies where an AccountID is
    // VL-framed — an STAccount field. A fixed-width slot (the IOU issuer
    // inside a 48-byte Amount, a PathSet hop) has no VL to shorten and always
    // carries 20 bytes.
    //
    // Hence two encoders, each named for the frame it writes into, and no
    // default: eliding inside a fixed-width slot silently desyncs the stream
    // for every byte that follows.

    /// Canonical-spelling test. base58check is canonical, so the zero
    /// account has exactly one encoding and this is exact, not heuristic.
    static bool
    is_zero_account(std::string_view base58_addr)
    {
        return base58_addr == ZERO_ACCOUNT_B58;
    }

    /// Value-side test, matching rippled's `isDefault()`.
    static bool
    is_zero_account(const uint8_t* data, size_t size)
    {
        if (size != fixed_size)
            return false;
        for (size_t i = 0; i < fixed_size; ++i)
        {
            if (data[i] != 0)
                return false;
        }
        return true;
    }

    /// Size of the VL *payload* for an STAccount field: zero for the default
    /// account, 20 otherwise. Not the size of a fixed-width slot, which is
    /// always `fixed_size`.
#ifndef CATL_XDATA_NO_BOOST_JSON
    static size_t
    encoded_size(boost::json::value const& v)
    {
        if (v.is_string() && is_zero_account(std::string_view(v.as_string())))
            return 0;
        return fixed_size;
    }
#endif

    // From raw 20 bytes
    template <ByteSink Sink>
    static void
    encode(Serializer<Sink>& s, std::span<const uint8_t, 20> data)
    {
        s.add_raw(std::span<const uint8_t>{data.data(), fixed_size});
    }

    /// Single decode point for every base58 input.
    static std::expected<std::vector<uint8_t>, CodecErrorValue>
    decode_b58_expected(
        std::string_view base58_addr,
        std::string const& path = {})
    {
        auto decoded = base58::decode_account_id(base58_addr);
        if (!decoded || decoded->size() != fixed_size)
        {
            return encode_error(
                CodecErrorCode::invalid_encoding,
                "AccountID",
                "invalid base58 address: " + std::string(base58_addr),
                path);
        }
        return std::move(*decoded);
    }

    // -- Fixed-width slot: always 20 bytes, the zero account included. -------

    template <ByteSink Sink>
    static std::expected<void, CodecErrorValue>
    encode_raw_expected(
        Serializer<Sink>& s,
        std::string_view base58_addr,
        std::string const& path = {})
    {
        auto decoded = decode_b58_expected(base58_addr, path);
        if (!decoded)
            return std::unexpected(std::move(decoded.error()));
        s.add_raw(std::span<const uint8_t>{decoded->data(), decoded->size()});
        return {};
    }

    template <ByteSink Sink>
    static void
    encode_raw(
        Serializer<Sink>& s,
        std::string_view base58_addr,
        std::string const& path = {})
    {
        encode_or_throw(encode_raw_expected(s, base58_addr, path));
    }

    // -- VL-framed STAccount payload: nothing for the default account. -------
    // The caller writes the VL prefix, sized by `encoded_size`.

    template <ByteSink Sink>
    static std::expected<void, CodecErrorValue>
    encode_vl_payload_expected(
        Serializer<Sink>& s,
        std::string_view base58_addr,
        std::string const& path = {})
    {
        if (is_zero_account(base58_addr))
            return {};
        return encode_raw_expected(s, base58_addr, path);
    }

    template <ByteSink Sink>
    static void
    encode_vl_payload(
        Serializer<Sink>& s,
        std::string_view base58_addr,
        std::string const& path = {})
    {
        encode_or_throw(encode_vl_payload_expected(s, base58_addr, path));
    }

#ifndef CATL_XDATA_NO_BOOST_JSON
    template <ByteSink Sink>
    static void
    encode_vl_payload(
        Serializer<Sink>& s,
        boost::json::value const& v,
        std::string const& path = {})
    {
        if (!v.is_string())
        {
            CATL_XDATA_THROW(EncodeError(
                CodecErrorCode::invalid_value,
                "AccountID",
                "expected string",
                path));
        }
        encode_vl_payload(s, std::string_view(v.as_string()), path);
    }
#endif

    static std::expected<std::string, CodecErrorValue>
    decode_string_expected(Slice const& data)
    {
        if (data.empty())
        {
            static const uint8_t zeros[fixed_size] = {};
            return base58::encode_account_id(zeros, fixed_size);
        }
        if (data.size() != fixed_size)
        {
            return encode_error(
                CodecErrorCode::malformed_data,
                "AccountID",
                "expected 20 bytes, got " + std::to_string(data.size()));
        }
        return base58::encode_account_id(data.data(), data.size());
    }

#ifndef CATL_XDATA_NO_BOOST_JSON
    static std::expected<boost::json::value, CodecErrorValue>
    decode_expected(Slice const& data)
    {
        auto decoded = decode_string_expected(data);
        if (!decoded)
            return std::unexpected(std::move(decoded.error()));
        return boost::json::string(*decoded);
    }

    static boost::json::value
    decode(Slice const& data)
    {
        return decode_or_throw(decode_expected(data));
    }
#endif
};

}  // namespace catl::xdata::codecs
