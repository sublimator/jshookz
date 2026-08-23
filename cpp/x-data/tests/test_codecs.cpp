// Behavioural tests for the codec headers.
//
// Two jobs. The obvious one is asserting the encoders. The other is that
// including codecs.h here makes *something* in the build compile these
// headers: before this file existed, jshookz_xdata compiled them out via
// CATL_XDATA_NO_BOOST_JSON and jshookz_xdata_json listed no TU that included
// them, so the whole codec surface type-checked nowhere.
//
// The property under test is that encoded_size() and encode() agree. They are
// separate walks over the same JSON — STObjectCodec reserves and writes VL
// prefixes from the first and then calls the second — so any disagreement
// desyncs every byte that follows.

#include <catl/xdata/codecs/codecs.h>
#include <catl/xdata/json-visitor.h>
#include <catl/xdata/parser.h>

#include <gtest/gtest.h>

#include <boost/json.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

using namespace catl::xdata;

namespace {

constexpr std::string_view ACCOUNT = "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh";
constexpr std::string_view ZERO = codecs::AccountIDCodec::ZERO_ACCOUNT_B58;

template <class Fn>
std::vector<std::uint8_t>
emit(Fn&& fn)
{
    std::vector<std::uint8_t> buf;
    VectorSink sink(buf);
    Serializer<VectorSink> s(sink);
    fn(s);
    return buf;
}

boost::json::value
json(std::string_view text)
{
    return boost::json::parse(text);
}

}  // namespace

// -- AccountID: the two frames -----------------------------------------------

TEST(AccountIDCodec, RawSlotIsAlwaysTwentyBytes)
{
    auto const real =
        emit([](auto& s) { codecs::AccountIDCodec::encode_raw(s, ACCOUNT); });
    EXPECT_EQ(real.size(), 20u);

    // The zero account is not special in a fixed-width slot: there is no VL
    // to shorten, so it occupies its 20 bytes like any other value.
    auto const zero =
        emit([](auto& s) { codecs::AccountIDCodec::encode_raw(s, ZERO); });
    ASSERT_EQ(zero.size(), 20u);
    EXPECT_TRUE(
        codecs::AccountIDCodec::is_zero_account(zero.data(), zero.size()));
}

TEST(AccountIDCodec, VlPayloadElidesOnlyTheDefaultAccount)
{
    // rippled STAccount::add — size = isDefault() ? 0 : uint160::bytes
    auto const zero = emit(
        [](auto& s) { codecs::AccountIDCodec::encode_vl_payload(s, ZERO); });
    EXPECT_EQ(zero.size(), 0u);

    auto const real = emit(
        [](auto& s) { codecs::AccountIDCodec::encode_vl_payload(s, ACCOUNT); });
    EXPECT_EQ(real.size(), 20u);
}

TEST(AccountIDCodec, ZeroAccountPredicatesAgree)
{
    auto const bytes =
        emit([](auto& s) { codecs::AccountIDCodec::encode_raw(s, ZERO); });
    EXPECT_TRUE(codecs::AccountIDCodec::is_zero_account(ZERO));
    EXPECT_TRUE(
        codecs::AccountIDCodec::is_zero_account(bytes.data(), bytes.size()));

    auto const real =
        emit([](auto& s) { codecs::AccountIDCodec::encode_raw(s, ACCOUNT); });
    EXPECT_FALSE(codecs::AccountIDCodec::is_zero_account(ACCOUNT));
    EXPECT_FALSE(
        codecs::AccountIDCodec::is_zero_account(real.data(), real.size()));
}

TEST(AccountIDCodec, RejectsInvalidBase58)
{
    std::vector<std::uint8_t> buf;
    VectorSink sink(buf);
    Serializer<VectorSink> s(sink);
    EXPECT_FALSE(codecs::AccountIDCodec::encode_raw_expected(s, "notanaddress"));
    EXPECT_FALSE(
        codecs::AccountIDCodec::encode_vl_payload_expected(s, "notanaddress"));
}

// -- encoded_size() must equal what encode() writes ---------------------------
//
// Each case is run twice: once with a real issuer/account and once with the
// default account, because the default account is where the two walks used to
// disagree — encoded_size counted a fixed slot while encode elided it.

TEST(CodecSizeAgreement, AmountIou)
{
    for (auto issuer : {ACCOUNT, ZERO})
    {
        auto const v = json(
            R"({"value":"10","currency":"USD","issuer":")" +
            std::string(issuer) + R"("})");
        auto const bytes =
            emit([&](auto& s) { codecs::AmountCodec::encode(s, v); });
        EXPECT_EQ(bytes.size(), codecs::AmountCodec::encoded_size(v))
            << "issuer=" << issuer;
        EXPECT_EQ(bytes.size(), 48u) << "issuer=" << issuer;
    }
}

TEST(CodecSizeAgreement, IssueIou)
{
    for (auto issuer : {ACCOUNT, ZERO})
    {
        auto const v = json(
            R"({"currency":"USD","issuer":")" + std::string(issuer) +
            R"("})");
        auto const bytes =
            emit([&](auto& s) { codecs::IssueCodec::encode(s, v); });
        EXPECT_EQ(bytes.size(), codecs::IssueCodec::encoded_size(v))
            << "issuer=" << issuer;
        EXPECT_EQ(bytes.size(), 40u) << "issuer=" << issuer;
    }
}

TEST(CodecSizeAgreement, PathSetHops)
{
    // Both AccountID positions in a hop, not just `account`: the `issuer`
    // encoder is a separate call site and was reachable with no test at all,
    // so flipping it back to the eliding encoder stayed green.
    for (auto account : {ACCOUNT, ZERO})
    {
        for (auto issuer : {ACCOUNT, ZERO})
        {
            auto const v = json(
                R"([[{"account":")" + std::string(account) +
                R"(","currency":"USD","issuer":")" + std::string(issuer) +
                R"("}]])");
            auto const bytes =
                emit([&](auto& s) { codecs::PathSetCodec::encode(s, v); });
            EXPECT_EQ(bytes.size(), codecs::PathSetCodec::encoded_size(v))
                << "account=" << account << " issuer=" << issuer;
            // type byte + account(20) + currency(20) + issuer(20) + END_BYTE
            EXPECT_EQ(bytes.size(), 62u)
                << "account=" << account << " issuer=" << issuer;
        }
    }
}

TEST(CodecSizeAgreement, XChainBridgeDoorsKeepTheirVlElision)
{
    // Doors are STAccount — VL-framed — so here the default account really
    // does shorten the payload, and encoded_size has to agree that it does.
    for (auto door : {ACCOUNT, ZERO})
    {
        auto const v = json(
            R"({"LockingChainDoor":")" + std::string(door) +
            R"(","LockingChainIssue":"XRP","IssuingChainDoor":")" +
            std::string(door) + R"(","IssuingChainIssue":"XRP"})");
        auto const bytes =
            emit([&](auto& s) { codecs::XChainBridgeCodec::encode(s, v); });
        EXPECT_EQ(bytes.size(), codecs::XChainBridgeCodec::encoded_size(v))
            << "door=" << door;
        EXPECT_EQ(bytes.size(), door == ZERO ? 42u : 82u) << "door=" << door;
    }
}

// -- round trips --------------------------------------------------------------

TEST(CodecRoundTrip, AmountIouThroughDecode)
{
    auto const v = json(
        R"({"value":"10","currency":"USD","issuer":")" + std::string(ACCOUNT) +
        R"("})");
    auto const bytes =
        emit([&](auto& s) { codecs::AmountCodec::encode(s, v); });
    auto const back =
        codecs::AmountCodec::decode(Slice(bytes.data(), bytes.size()));
    ASSERT_TRUE(back.is_object());
    EXPECT_EQ(back.as_object().at("currency").as_string(), "USD");
    EXPECT_EQ(back.as_object().at("issuer").as_string(), ACCOUNT);
    EXPECT_EQ(back.as_object().at("value").as_string(), "10");
}

TEST(CodecRoundTrip, AccountIdThroughDecode)
{
    auto const bytes =
        emit([](auto& s) { codecs::AccountIDCodec::encode_raw(s, ACCOUNT); });
    auto const back =
        codecs::AccountIDCodec::decode(Slice(bytes.data(), bytes.size()));
    EXPECT_EQ(back.as_string(), ACCOUNT);
}

// -- input bounds (upstream d5d11ba, 2f08afe) --------------------------------

TEST(ParserDepth, NestingBeyondTheCapIsRejected)
{
    // A blob of nothing but STObject field headers: each one opens another
    // level and never closes it, which is the shape that drove unbounded
    // recursion into a stack-exhaustion SIGSEGV before the cap.
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    auto const field = protocol.find_field("Memo");
    ASSERT_TRUE(field);
    ASSERT_EQ(field->meta.type.code, FieldTypes::STObject.code);

    std::vector<std::uint8_t> blob;
    VectorSink sink(blob);
    Serializer<VectorSink> s(sink);
    for (int i = 0; i < kMaxParseDepth + 8; ++i)
        s.add_field_header(*field);

    ParserContext ctx{Slice(blob.data(), blob.size())};
    JsonVisitor visitor(protocol);
    EXPECT_THROW(parse_with_visitor(ctx, protocol, visitor), ParserError);
}

TEST(ParserDepth, NestingWithinTheCapStillParses)
{
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    auto const field = protocol.find_field("Memo");
    ASSERT_TRUE(field);

    std::vector<std::uint8_t> blob;
    VectorSink sink(blob);
    Serializer<VectorSink> s(sink);
    constexpr int depth = 8;
    for (int i = 0; i < depth; ++i)
        s.add_field_header(*field);
    for (int i = 0; i < depth; ++i)
        s.add_object_end_marker();

    ParserContext ctx{Slice(blob.data(), blob.size())};
    JsonVisitor visitor(protocol);
    EXPECT_NO_THROW(parse_with_visitor(ctx, protocol, visitor));
}

TEST(Base58LengthCap, OversizeInputIsRejectedWithoutQuadraticWork)
{
    // NOT 'r'. 'r' is the alphabet's zero character, so a string of them takes
    // the leading-zeros path and does no quadratic work — a cap test built
    // from 'r's passes with the cap removed and proves nothing.
    std::string const huge(2048, 'p');
    EXPECT_FALSE(catl::base58::xrpl_codec.decode(huge));

    // Just under the cap still does the work rather than being rejected.
    std::string const ok(1024, 'p');
    EXPECT_TRUE(catl::base58::xrpl_codec.decode(ok));

    std::vector<std::uint8_t> const big(2048, 0x41);
    EXPECT_TRUE(catl::base58::xrpl_codec.encode(big.data(), big.size()).empty());
    std::vector<std::uint8_t> const fits(1024, 0x41);
    EXPECT_FALSE(
        catl::base58::xrpl_codec.encode(fits.data(), fits.size()).empty());
}

namespace {

/// Declines every object, so the parser takes the skip_object/skip_array
/// path instead of descending. Implementing only one callback is legal since
/// upstream e76de19 made the rest optional.
struct DeclineObjects
{
    bool
    visit_object_start(const FieldPath&, const FieldSlice&)
    {
        return false;
    }
};

}  // namespace

TEST(ParserDepth, SkipPathIsCappedToo)
{
    // The descend path and the skip path have separate depth guards. Only the
    // descend one was covered, so deleting both skip_* guards left the suite
    // green — and the skip path is what selective descent relies on.
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    auto const field = protocol.find_field("Memo");
    ASSERT_TRUE(field);

    std::vector<std::uint8_t> blob;
    VectorSink sink(blob);
    Serializer<VectorSink> s(sink);
    for (int i = 0; i < kMaxParseDepth + 8; ++i)
        s.add_field_header(*field);

    ParserContext ctx{Slice(blob.data(), blob.size())};
    DeclineObjects visitor;
    EXPECT_THROW(parse_with_visitor(ctx, protocol, visitor), ParserError);
}

TEST(TruncatedInput, PathSetAccountHopWithNoAccountThrows)
{
    // Paths header (long type 18, field 1) then hop type Account with none of
    // the 20 account bytes. PathSetRules failure is sticky; the legacy visitor
    // must throw without rewinding or visiting the truncated hop.
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    std::uint8_t const blob[] = {0x01, 0x12, 0x01};
    ParserContext ctx{Slice{blob, sizeof(blob)}};
    struct CountFields
    {
        int fields = 0;
        void
        visit_field(const FieldPath&, const FieldSlice&)
        {
            ++fields;
        }
    };
    CountFields visitor;
    EXPECT_THROW(parse_with_visitor(ctx, protocol, visitor), std::exception);
    EXPECT_EQ(visitor.fields, 0);
    EXPECT_TRUE(ctx.failed());
}

TEST(TruncatedInput, XChainBridgeHeaderWithNoBodyIsRejected)
{
    // A field header and nothing else. wire_size() is called from the leaf
    // dispatch before any length is known, and used to index straight past
    // the end of the buffer — reachable at depth 0, so the recursion cap does
    // nothing for it.
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    auto const field = protocol.find_field("XChainBridge");
    ASSERT_TRUE(field);

    std::vector<std::uint8_t> blob;
    VectorSink sink(blob);
    Serializer<VectorSink> s(sink);
    s.add_field_header(*field);
    ASSERT_LE(blob.size(), 3u);

    ParserContext ctx{Slice(blob.data(), blob.size())};
    JsonVisitor visitor(protocol);
    EXPECT_THROW(parse_with_visitor(ctx, protocol, visitor), std::exception);
}

TEST(TruncatedInput, XChainBridgeDecodeStopsAtTheEnd)
{
    // Same for the decode side: a VL prefix promising more than remains.
    std::vector<std::uint8_t> const truncated{20, 0x01, 0x02};
    EXPECT_THROW(
        codecs::XChainBridgeCodec::decode(
            Slice(truncated.data(), truncated.size())),
        std::exception);
}

TEST(Base58LengthCap, NormalSizedValuesStillRoundTrip)
{
    auto const decoded = catl::base58::decode_account_id(ACCOUNT);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->size(), 20u);
    EXPECT_EQ(catl::base58::encode_account_id(decoded->data(), decoded->size()),
              ACCOUNT);
}

TEST(CodecRoundTrip, EmptyAccountSliceDecodesToTheDefaultAccount)
{
    // The other side of the VL elision: a zero-length STAccount payload reads
    // back as the default account.
    auto const back = codecs::AccountIDCodec::decode(Slice(nullptr, 0));
    EXPECT_EQ(back.as_string(), ZERO);
}
