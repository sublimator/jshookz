#include "catl/xdata/parser-context.h"
#include "catl/xdata/protocol.h"
#include "catl/xdata/scan.h"

#include <gtest/gtest.h>

using namespace catl::xdata;

TEST(ParserContext, FirstErrorWinsAndLaterReadsAreNoops)
{
    std::uint8_t const bytes[] = {0x01};
    ParserContext ctx{Slice{bytes, sizeof(bytes)}};
    uint8_t b = 0;
    ASSERT_TRUE(ctx.read_u8(b));
    EXPECT_EQ(b, 0x01);
    EXPECT_FALSE(ctx.failed());
    size_t const pos_at_fail = ctx.pos();
    EXPECT_FALSE(ctx.read_u8(b));
    EXPECT_TRUE(ctx.failed());
    EXPECT_EQ(ctx.fail_offset(), pos_at_fail);
    EXPECT_EQ(ctx.pos(), pos_at_fail);
    ctx.fail("second error");
    EXPECT_NE(std::string(ctx.as_error().message).find("read_u8"), std::string::npos);
    EXPECT_FALSE(ctx.read_u8(b));
    EXPECT_EQ(ctx.pos(), pos_at_fail);
    EXPECT_FALSE(ctx.advance(1));
    EXPECT_EQ(ctx.pos(), pos_at_fail);
}

TEST(ParserContext, FailedReadDoesNotAdvance)
{
    std::uint8_t const bytes[] = {0x81};
    ParserContext ctx{Slice{bytes, sizeof(bytes)}};
    EXPECT_EQ(ctx.pos(), 0u);
    EXPECT_FALSE(ctx.advance(4));
    EXPECT_TRUE(ctx.failed());
    EXPECT_EQ(ctx.pos(), 0u);
    EXPECT_EQ(ctx.fail_offset(), 0u);
}

TEST(ParserContext, ResetClearsFailureForNextScan)
{
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    std::uint8_t const bad[] = {0xff};
    std::uint8_t const good[] = {0x81, 0x00};
    ParserContext ctx{Slice{bad, sizeof(bad)}};
    NullSink sink;
    scan_detail::scan_object<ScanMode::CertifyWire>(
        ctx, protocol, sink, 0, true);
    EXPECT_TRUE(ctx.failed());
    ctx.reset(Slice{good, sizeof(good)});
    EXPECT_FALSE(ctx.failed());
    scan_detail::scan_object<ScanMode::CertifyWire>(
        ctx, protocol, sink, 0, true);
    EXPECT_FALSE(ctx.failed()) << ctx.as_error().message;
}

TEST(ParserContext, MalformedThenValidPublicAdapter)
{
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    std::uint8_t const bad[] = {0xff};
    std::uint8_t const good[] = {0x81, 0x00};
    NullSink sink;
    auto r1 = scan_scope<ScanMode::CertifyWire>(
        Slice{bad, sizeof(bad)}, 0, protocol, sink);
    EXPECT_FALSE(r1.has_value());
    auto r2 = scan_scope<ScanMode::CertifyWire>(
        Slice{good, sizeof(good)}, 0, protocol, sink);
    EXPECT_TRUE(r2.has_value()) << (r1 ? "" : r1.error().message);
}
