#pragma once

#include "catl/xdata/codec-error.h"
#include "catl/xdata/codecs/account_id.h"
#include "catl/xdata/codecs/currency.h"
#include "catl/xdata/serializer.h"
#include "catl/xdata/types/amount.h"
#include "catl/xdata/types/iou-value.h"
#ifndef CATL_XDATA_NO_BOOST_JSON
#include <boost/json.hpp>
#endif

namespace catl::xdata::codecs {

struct AmountCodec
{
    static constexpr size_t native_size = 8;
    static constexpr size_t iou_size = 48;
    static constexpr size_t mpt_size = 33;

#ifndef CATL_XDATA_NO_BOOST_JSON
    static size_t
    encoded_size(boost::json::value const& v)
    {
        if (v.is_string())
            return native_size;
        auto const& obj = v.as_object();
        if (obj.contains("mpt_issuance_id"))
            return mpt_size;
        return iou_size;
    }
#endif

    // -- Native amount from integer drops --
    template <ByteSink Sink>
    static void
    encode_native(Serializer<Sink>& s, int64_t drops)
    {
        s.add_native_amount(drops);
    }

    // -- Native amount from string drops (errors-as-values core) --
    template <ByteSink Sink>
    static std::expected<void, CodecErrorValue>
    encode_native_expected(
        Serializer<Sink>& s,
        std::string_view drops_str,
        std::string const& path = {})
    {
        auto drops = try_parse_int64(drops_str, "Amount", path);
        if (!drops)
            return std::unexpected(std::move(drops.error()));
        s.add_native_amount(*drops);
        return {};
    }

    // -- Native amount from string drops (throwing facade) --
    template <ByteSink Sink>
    static void
    encode_native(
        Serializer<Sink>& s,
        std::string_view drops_str,
        std::string const& path = {})
    {
        encode_or_throw(encode_native_expected(s, drops_str, path));
    }

    // -- IOU amount from string parts (errors-as-values core) --
    template <ByteSink Sink>
    static std::expected<void, CodecErrorValue>
    encode_iou_expected(
        Serializer<Sink>& s,
        std::string_view value,
        std::string_view currency,
        std::string_view issuer,
        std::string const& path = {})
    {
        auto raw = encode_iou_value_expected(value, path);
        if (!raw)
            return std::unexpected(std::move(raw.error()));
        s.add_u64(*raw);
        if (auto r = CurrencyCodec::encode_expected(s, currency, path); !r)
            return std::unexpected(std::move(r.error()));
        if (auto r = AccountIDCodec::encode_raw_expected(s, issuer, path); !r)
            return std::unexpected(std::move(r.error()));
        return {};
    }

    // -- IOU amount from string parts (throwing facade) --
    template <ByteSink Sink>
    static void
    encode_iou(
        Serializer<Sink>& s,
        std::string_view value,
        std::string_view currency,
        std::string_view issuer,
        std::string const& path = {})
    {
        encode_or_throw(encode_iou_expected(s, value, currency, issuer, path));
    }

    // -- MPT amount from string parts (errors-as-values core) --
    template <ByteSink Sink>
    static std::expected<void, CodecErrorValue>
    encode_mpt_expected(
        Serializer<Sink>& s,
        std::string_view value,
        std::string_view mpt_issuance_id,
        std::string const& path = {})
    {
        if (auto r = try_require_hex_length(mpt_issuance_id, 48, "Amount", path);
            !r)
            return std::unexpected(std::move(r.error()));

        int64_t int_val = 0;
        bool is_negative = false;

        if (value.size() > 2 && value[0] == '0' &&
            (value[1] == 'x' || value[1] == 'X'))
        {
            auto u = try_parse_hex_uint64(value.substr(2), "Amount", path);
            if (!u)
                return std::unexpected(std::move(u.error()));
            int_val = static_cast<int64_t>(*u);
        }
        else
        {
            if (!value.empty() && value[0] == '-')
            {
                is_negative = true;
                value.remove_prefix(1);
            }
            auto v = try_parse_int64(value, "Amount", path);
            if (!v)
                return std::unexpected(std::move(v.error()));
            int_val = *v;
        }

        // Zero is always positive
        uint8_t flags = 0x20;  // cMPToken
        if (!is_negative || int_val == 0)
        {
            flags |= 0x40;  // cPositive
        }

        s.add_u8(flags);
        s.add_u64(static_cast<uint64_t>(int_val));
        s.add_hex(mpt_issuance_id);
        return {};
    }

    // -- MPT amount from string parts (throwing facade) --
    template <ByteSink Sink>
    static void
    encode_mpt(
        Serializer<Sink>& s,
        std::string_view value,
        std::string_view mpt_issuance_id,
        std::string const& path = {})
    {
        encode_or_throw(encode_mpt_expected(s, value, mpt_issuance_id, path));
    }

#ifndef CATL_XDATA_NO_BOOST_JSON
    template <ByteSink Sink>
    static void
    encode(
        Serializer<Sink>& s,
        boost::json::value const& v,
        std::string const& path = {})
    {
        if (v.is_string())
        {
            encode_native(s, std::string_view(v.as_string()), path);
        }
        else if (v.is_object())
        {
            auto const& obj = v.as_object();
            if (obj.contains("mpt_issuance_id"))
            {
                if (!obj.contains("value"))
                {
                    CATL_XDATA_THROW(EncodeError(
                        CodecErrorCode::missing_field,
                        "Amount",
                        "MPT missing 'value'",
                        path));
                }
                encode_mpt(
                    s,
                    std::string_view(obj.at("value").as_string()),
                    std::string_view(obj.at("mpt_issuance_id").as_string()),
                    path);
            }
            else
            {
                if (!obj.contains("value") || !obj.contains("currency") ||
                    !obj.contains("issuer"))
                {
                    CATL_XDATA_THROW(EncodeError(
                        CodecErrorCode::missing_field,
                        "Amount",
                        "IOU requires 'value', 'currency', and 'issuer'",
                        path));
                }
                encode_iou(
                    s,
                    std::string_view(obj.at("value").as_string()),
                    std::string_view(obj.at("currency").as_string()),
                    std::string_view(obj.at("issuer").as_string()),
                    path);
            }
        }
        else
        {
            CATL_XDATA_THROW(EncodeError(
                CodecErrorCode::invalid_value,
                "Amount",
                "expected string or object",
                path));
        }
    }

    // Decode binary amount to JSON
    static boost::json::value
    decode(Slice const& data)
    {
        if (is_native_amount(data))
        {
            return boost::json::string(parse_native_drops_string(data));
        }
        // IOU: 48 bytes
        if (data.size() == 48)
        {
            IOUValue iou = IOUValue::from_bytes(data.data());
            Slice currency_slice = get_currency_raw(data);
            std::string issuer =
                base58::encode_account_id(data.data() + 28, 20);

            boost::json::object obj;
            obj["currency"] = CurrencyCodec::decode(currency_slice);
            obj["value"] = boost::json::string(iou.to_string());
            obj["issuer"] = boost::json::string(issuer);
            return obj;
        }
        // MPT: 33 bytes (1 flag + 8 value + 24 mptid)
        if (data.size() == 33)
        {
            uint64_t val = 0;
            for (int i = 1; i < 9; ++i)
            {
                val = (val << 8) | data.data()[i];
            }
            boost::json::object obj;
            obj["value"] = boost::json::string(std::to_string(val));
            obj["mpt_issuance_id"] =
                boost::json::string(hex_encode(data.data() + 9, 24));
            return obj;
        }
        // Fallback
        return boost::json::string(hex_encode(data));
    }
#endif

private:
    static constexpr uint64_t IOU_BIT = 0x8000000000000000ULL;
    static constexpr uint64_t POS_BIT = 0x4000000000000000ULL;
    static constexpr uint64_t MANTISSA_MASK = 0x003FFFFFFFFFFFFFULL;
    static constexpr uint64_t MIN_MANTISSA = 1000000000000000ULL;
    static constexpr uint64_t MAX_MANTISSA = 9999999999999999ULL;

    static uint64_t
    encode_iou_value(std::string_view value_str, std::string const& path = {})
    {
        return encode_or_throw(encode_iou_value_expected(value_str, path));
    }

    static std::expected<uint64_t, CodecErrorValue>
    encode_iou_value_expected(
        std::string_view value_str,
        std::string const& path = {})
    {
        if (value_str == "0" || value_str == "0.0" || value_str.empty())
        {
            return IOU_BIT;
        }

        bool negative = false;
        std::string_view sv = value_str;
        if (sv.front() == '-')
        {
            negative = true;
            sv.remove_prefix(1);
        }

        uint64_t mantissa = 0;
        int exponent = 0;
        bool seen_dot = false;
        int decimals = 0;
        bool in_trailing_digits = false;

        // Split on 'e'/'E' for scientific notation (e.g. "9999999999999999e79")
        auto e_pos = sv.find_first_of("eE");
        std::string_view digits_part = (e_pos != std::string_view::npos)
            ? sv.substr(0, e_pos)
            : sv;
        int e_exponent = 0;
        if (e_pos != std::string_view::npos)
        {
            auto exp_str = sv.substr(e_pos + 1);
            bool exp_neg = false;
            if (!exp_str.empty() && exp_str.front() == '-')
            {
                exp_neg = true;
                exp_str.remove_prefix(1);
            }
            else if (!exp_str.empty() && exp_str.front() == '+')
            {
                exp_str.remove_prefix(1);
            }
            for (char c : exp_str)
            {
                if (c < '0' || c > '9')
                    break;
                e_exponent = e_exponent * 10 + (c - '0');
            }
            if (exp_neg)
                e_exponent = -e_exponent;
        }

        for (char c : digits_part)
        {
            if (c == '.')
            {
                seen_dot = true;
                continue;
            }
            if (c < '0' || c > '9')
                break;

            if (in_trailing_digits)
            {
                if (!seen_dot)
                    ++exponent;
                continue;
            }

            uint64_t next = mantissa * 10 + static_cast<uint64_t>(c - '0');
            if (next > MAX_MANTISSA)
            {
                if (c >= '5' && mantissa < MAX_MANTISSA)
                {
                    ++mantissa;
                }
                in_trailing_digits = true;
                if (!seen_dot)
                    ++exponent;
                continue;
            }

            mantissa = next;
            if (seen_dot)
                ++decimals;
        }
        exponent -= decimals;
        exponent += e_exponent;

        if (mantissa == 0)
        {
            return IOU_BIT;
        }

        // Normalize to [1e15, 1e16)
        while (mantissa < MIN_MANTISSA)
        {
            mantissa *= 10;
            --exponent;
        }
        while (mantissa > MAX_MANTISSA)
        {
            mantissa /= 10;
            ++exponent;
        }

        // Validate exponent range (-96 to 80)
        if (exponent < -96 || exponent > 80)
        {
            return encode_error(
                CodecErrorCode::out_of_range,
                "Amount",
                "IOU exponent out of range: " + std::to_string(exponent),
                path);
        }

        uint64_t raw = IOU_BIT;
        if (!negative)
        {
            raw |= POS_BIT;
        }
        uint64_t biased_exp = static_cast<uint64_t>(exponent + 97);
        raw |= (biased_exp << 54);
        raw |= (mantissa & MANTISSA_MASK);
        return raw;
    }
};

}  // namespace catl::xdata::codecs
