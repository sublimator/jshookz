#pragma once

#include "catl/xdata/amount-rules.h"
#include "catl/xdata/codec-error.h"
#include "catl/xdata/codecs/account_id.h"
#include "catl/xdata/codecs/currency.h"
#include "catl/xdata/hex.h"
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

    static boost::json::value
    json_from_parts(AmountRules::Parts const& p, Slice const& payload)
    {
        using Kind = AmountRules::Kind;
        if (p.kind == Kind::Native)
        {
            std::string s = std::to_string(p.magnitude);
            if (p.negative && p.magnitude != 0)
                s.insert(s.begin(), '-');
            return boost::json::string(s);
        }
        if (p.kind == Kind::Mpt)
        {
            boost::json::object obj;
            obj["value"] = boost::json::string(std::to_string(p.magnitude));
            if (p.mpt_id.size() == 24)
            {
                obj["mpt_issuance_id"] = boost::json::string(
                    hex_encode(p.mpt_id.data(), p.mpt_id.size()));
            }
            return obj;
        }
        boost::json::object obj;
        if (p.currency.size() == 20)
            obj["currency"] = CurrencyCodec::decode(p.currency);
        if (payload.size() >= 8)
        {
            IOUValue iou = IOUValue::from_bytes(payload.data());
            obj["value"] = boost::json::string(iou.to_string());
        }
        if (p.issuer.size() == 20)
        {
            obj["issuer"] = boost::json::string(
                base58::encode_account_id(p.issuer.data(), 20));
        }
        return obj;
    }

    static boost::json::object
    oracle_parts(AmountRules::Parts const& p)
    {
        using Kind = AmountRules::Kind;
        boost::json::object o;
        if (p.kind == Kind::Native)
        {
            o["type"] = "native";
            o["drops"] = std::to_string(p.magnitude);
            o["negative"] = p.negative;
            return o;
        }
        if (p.kind == Kind::Mpt)
        {
            o["type"] = "mpt";
            o["value"] = std::to_string(p.magnitude);
            o["negative"] = p.negative;
            if (p.mpt_id.size() == 24)
                o["mpt_id"] = hex_encode(p.mpt_id.data(), p.mpt_id.size());
            return o;
        }
        o["type"] = "iou";
        if (p.currency.size() == 20)
            o["currency"] = CurrencyCodec::decode(p.currency);
        if (p.issuer.size() == 20)
        {
            o["issuer"] =
                base58::encode_account_id(p.issuer.data(), 20);
        }
        if (p.zero)
        {
            o["zero"] = true;
            return o;
        }
        o["negative"] = p.negative;
        o["exponent"] = std::to_string(p.exponent);
        o["mantissa"] = std::to_string(p.magnitude);
        return o;
    }

    // Decode binary amount to JSON via AmountRules, not a second walk.
    static boost::json::value
    decode(Slice const& data)
    {
        if (char const* e = AmountRules::certify(data))
        {
            CATL_XDATA_THROW(std::runtime_error(e));
        }
        return json_from_parts(AmountRules::parts(data), data);
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
