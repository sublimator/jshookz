#include <catl/xdata/codec-error.h>
// Compiles the codec headers in the CATL_XDATA_NO_BOOST_JSON configuration —
// the one the wasm library actually ships. test_codecs covers the boost side.
#include <catl/xdata/codecs/codecs.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <string_view>
#include <vector>

using catl::xdata::EncodeError;
using catl::xdata::parse_hex_uint64;
using catl::xdata::parse_int64;
using catl::xdata::parse_uint64;
using catl::xdata::try_parse_hex_uint64;
using catl::xdata::try_parse_int64;

TEST(ParseHelpers, RejectsTrailingGarbage)
{
    EXPECT_THROW(parse_int64("1234GG", "Int64"), EncodeError);
    EXPECT_THROW(parse_uint64("1234GG", "UInt64"), EncodeError);
    EXPECT_THROW(parse_hex_uint64("1234GG", "UInt64"), EncodeError);
    EXPECT_FALSE(try_parse_int64("1234GG", "Int64"));
    EXPECT_FALSE(try_parse_hex_uint64("1234GG", "UInt64"));
}

TEST(ParseHelpers, HexUint64RejectsOverLengthEvenWhenValueFits)
{
    EXPECT_THROW(parse_hex_uint64("00000000000000001", "UInt64"), EncodeError);
    EXPECT_FALSE(try_parse_hex_uint64("00000000000000001", "UInt64"));
}

TEST(ParseHelpers, HexUint64AcceptsCanonicalAndFullWidth)
{
    EXPECT_EQ(parse_hex_uint64("1", "UInt64"), 1u);
    EXPECT_EQ(parse_hex_uint64("0000000000000001", "UInt64"), 1u);
    EXPECT_EQ(*try_parse_hex_uint64("1", "UInt64"), 1u);
    EXPECT_EQ(*try_parse_hex_uint64("0000000000000001", "UInt64"), 1u);
}

// The raw/VL AccountID split, in the no-boost configuration. Same assertions
// as test_codecs, but reached through the headers as the wasm build sees them.
TEST(NoBoostCodecs, AccountIdRawAndVlFramesDiffer)
{
    using catl::xdata::codecs::AccountIDCodec;

    auto emit = [](auto&& fn) {
        std::vector<std::uint8_t> buf;
        catl::xdata::VectorSink sink(buf);
        catl::xdata::Serializer<catl::xdata::VectorSink> s(sink);
        fn(s);
        return buf;
    };

    constexpr std::string_view account = "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh";

    EXPECT_EQ(
        emit([&](auto& s) {
            AccountIDCodec::encode_raw(s, AccountIDCodec::ZERO_ACCOUNT_B58);
        }).size(),
        20u);
    EXPECT_EQ(
        emit([&](auto& s) {
            AccountIDCodec::encode_vl_payload(
                s, AccountIDCodec::ZERO_ACCOUNT_B58);
        }).size(),
        0u);
    EXPECT_EQ(
        emit([&](auto& s) { AccountIDCodec::encode_raw(s, account); }).size(),
        20u);
    EXPECT_EQ(
        emit([&](auto& s) {
            AccountIDCodec::encode_vl_payload(s, account);
        }).size(),
        20u);
}
