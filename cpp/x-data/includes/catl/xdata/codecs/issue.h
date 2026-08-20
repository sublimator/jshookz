#pragma once

#include "catl/xdata/codec-error.h"
#include "catl/xdata/codecs/account_id.h"
#include "catl/xdata/codecs/currency.h"
#include "catl/xdata/hex.h"
#include "catl/xdata/serializer.h"
#include "catl/xdata/types/issue.h"
#ifndef CATL_XDATA_NO_BOOST_JSON
#include <boost/json.hpp>
#endif

namespace catl::xdata::codecs {

// noAccount sentinel: 20 bytes, all zeros except last byte = 1
// This distinguishes MPT from IOU in the wire format.
inline constexpr uint8_t NO_ACCOUNT[20] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};

struct IssueCodec
{
    // Size: XRP=20, IOU=40, MPT=44
#ifndef CATL_XDATA_NO_BOOST_JSON
    static size_t
    encoded_size(boost::json::value const& v)
    {
        if (v.is_string())
            return 20;  // "XRP"
        auto const& obj = v.as_object();
        if (obj.contains("mpt_issuance_id"))
            return 44;  // MPT: issuer(20) + noAccount(20) + seq(4)
        if (!obj.contains("issuer"))
            return 20;  // native
        return 40;      // IOU: currency(20) + issuer(20)
    }
#endif

    template <ByteSink Sink>
    static void
    encode_native(Serializer<Sink>& s)
    {
        s.add_issue_native();
    }

    // IOU: currency + issuer (errors-as-values core)
    template <ByteSink Sink>
    static std::expected<void, CodecErrorValue>
    encode_iou_expected(
        Serializer<Sink>& s,
        std::string_view currency,
        std::string_view issuer,
        std::string const& path = {})
    {
        if (auto r = CurrencyCodec::encode_expected(s, currency, path); !r)
            return std::unexpected(std::move(r.error()));
        if (auto r = AccountIDCodec::encode_raw_expected(s, issuer, path); !r)
            return std::unexpected(std::move(r.error()));
        return {};
    }

    // IOU: currency + issuer (throwing facade)
    template <ByteSink Sink>
    static void
    encode_iou(
        Serializer<Sink>& s,
        std::string_view currency,
        std::string_view issuer,
        std::string const& path = {})
    {
        encode_or_throw(encode_iou_expected(s, currency, issuer, path));
    }

    // MPT: issuer(20) + noAccount(20) + sequence(4) (errors-as-values core)
    template <ByteSink Sink>
    static std::expected<void, CodecErrorValue>
    encode_mpt_expected(
        Serializer<Sink>& s,
        std::string_view mpt_issuance_id,
        std::string const& path = {})
    {
        // MPTID is 24 bytes: sequence(4) + issuer(20)
        // On wire: issuer(20) + noAccount(20) + sequence(4)
        if (auto r = try_require_hex_length(mpt_issuance_id, 48, "Issue", path);
            !r)
            return std::unexpected(std::move(r.error()));

        // Decode the 24-byte MPTID (length guaranteed above, so this cannot
        // throw).
        uint8_t mptid[24];
        hex_decode(mpt_issuance_id, std::span<uint8_t>{mptid, 24});

        // mptid[0..3] = sequence (big-endian)
        // mptid[4..23] = issuer AccountID

        // Write issuer (20 bytes from offset 4)
        s.add_raw(std::span<const uint8_t>{mptid + 4, 20});
        // Write noAccount sentinel
        s.add_raw(std::span<const uint8_t>{NO_ACCOUNT, 20});
        // Write sequence: rippled does memcpy(first 4 MPTID bytes → uint32)
        // then add32. This is a native-endian reinterpret + big-endian write.
        uint32_t sequence;
        std::memcpy(&sequence, mptid, sizeof(sequence));
        s.add_u32(sequence);
        return {};
    }

    // MPT: issuer(20) + noAccount(20) + sequence(4) (throwing facade)
    template <ByteSink Sink>
    static void
    encode_mpt(
        Serializer<Sink>& s,
        std::string_view mpt_issuance_id,
        std::string const& path = {})
    {
        encode_or_throw(encode_mpt_expected(s, mpt_issuance_id, path));
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
            auto sv = std::string_view(v.as_string());
            if (sv == "XRP" || sv == "XAH" || sv.empty())
            {
                encode_native(s);
            }
            else
            {
                CATL_XDATA_THROW(EncodeError(
                    CodecErrorCode::invalid_value,
                    "Issue",
                    "string value must be XRP or XAH, got: " +
                        std::string(sv),
                    path));
            }
        }
        else if (v.is_object())
        {
            auto const& obj = v.as_object();
            if (obj.contains("mpt_issuance_id"))
            {
                encode_mpt(
                    s,
                    std::string_view(obj.at("mpt_issuance_id").as_string()),
                    path);
            }
            else if (!obj.contains("currency"))
            {
                CATL_XDATA_THROW(EncodeError(
                    CodecErrorCode::missing_field,
                    "Issue",
                    "missing 'currency' or 'mpt_issuance_id'",
                    path));
            }
            else if (!obj.contains("issuer"))
            {
                encode_native(s);
            }
            else
            {
                encode_iou(
                    s,
                    std::string_view(obj.at("currency").as_string()),
                    std::string_view(obj.at("issuer").as_string()),
                    path);
            }
        }
        else
        {
            CATL_XDATA_THROW(EncodeError(
                CodecErrorCode::invalid_value,
                "Issue",
                "expected string or object",
                path));
        }
    }

    static boost::json::value
    decode(Slice const& data)
    {
        if (data.size() < 20)
            return boost::json::string(hex_encode(data));

        // First 20 bytes: currency or MPT issuer
        Slice first20(data.data(), 20);

        // Check native (all zeros)
        if (is_xrp_currency(first20))
            return CurrencyCodec::decode(first20);

        if (data.size() >= 40)
        {
            // Second 20 bytes: issuer or noAccount sentinel
            Slice second20(data.data() + 20, 20);

            // Check for MPT: second slot == noAccount
            if (std::memcmp(second20.data(), NO_ACCOUNT, 20) == 0 &&
                data.size() >= 44)
            {
                // MPT: first20=issuer, second20=noAccount, next 4=sequence
                // Reconstruct MPTID: sequence(4) + issuer(20)
                // Reverse the memcpy+add32 from encode: read big-endian
                // uint32, memcpy back to bytes
                uint32_t seq_be = (static_cast<uint32_t>(data.data()[40]) << 24) |
                                  (static_cast<uint32_t>(data.data()[41]) << 16) |
                                  (static_cast<uint32_t>(data.data()[42]) << 8) |
                                   static_cast<uint32_t>(data.data()[43]);
                uint8_t mptid[24];
                std::memcpy(mptid, &seq_be, 4);               // native-endian sequence
                std::memcpy(mptid + 4, data.data(), 20);      // issuer
                boost::json::object obj;
                obj["mpt_issuance_id"] =
                    boost::json::string(hex_encode(mptid, 24));
                return obj;
            }

            // IOU: first20=currency, second20=issuer
            // xahaud-vectors:src/libxrpl/protocol/Issue.cpp:67
            if (is_xrp_currency(first20) != is_xrp_currency(second20))
            {
                CATL_XDATA_THROW(DecodeError(
                    CodecErrorCode::malformed_data,
                    "Issue",
                    "invalid issue: currency and account native mismatch"));
            }
            boost::json::object obj;
            obj["currency"] = CurrencyCodec::decode(first20);
            obj["issuer"] = AccountIDCodec::decode(second20);
            return obj;
        }

        // Just currency, no issuer (native variant)
        return CurrencyCodec::decode(first20);
    }
#endif
};

}  // namespace catl::xdata::codecs
