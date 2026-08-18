#include <jshookz/qjs.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using jshookz::qjs::HexCase;
using jshookz::qjs::hexDecode;
using jshookz::qjs::hexEncode;

TEST(Hex, EncodeUpper)
{
    std::vector<std::uint8_t> const bytes{0xde, 0xad, 0x00, 0xff};
    EXPECT_EQ(hexEncode(bytes, HexCase::Upper), "DEAD00FF");
}

TEST(Hex, EncodeLower)
{
    std::vector<std::uint8_t> const bytes{0xde, 0xad, 0x00, 0xff};
    EXPECT_EQ(hexEncode(bytes, HexCase::Lower), "dead00ff");
}

TEST(Hex, DecodeMixedCase)
{
    std::vector<std::uint8_t> const bytes{0xde, 0xad, 0x00, 0xff};
    std::vector<std::uint8_t> out;
    ASSERT_TRUE(hexDecode("DeAd00Ff", out));
    EXPECT_EQ(out, bytes);
}

TEST(Hex, DecodeRejectsOddLengthAndNonHex)
{
    std::vector<std::uint8_t> out;
    EXPECT_FALSE(hexDecode("f", out));
    EXPECT_FALSE(hexDecode("zz", out));
}
