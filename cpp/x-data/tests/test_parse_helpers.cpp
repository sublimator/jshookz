#include <catl/xdata/codec-error.h>

#include <gtest/gtest.h>

#include <string_view>

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
