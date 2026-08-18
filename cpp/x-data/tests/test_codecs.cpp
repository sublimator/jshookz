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
    for (auto account : {ACCOUNT, ZERO})
    {
        auto const v =
            json(R"([[{"account":")" + std::string(account) + R"("}]])");
        auto const bytes =
            emit([&](auto& s) { codecs::PathSetCodec::encode(s, v); });
        EXPECT_EQ(bytes.size(), codecs::PathSetCodec::encoded_size(v))
            << "account=" << account;
        // type byte + 20-byte account + END_BYTE
        EXPECT_EQ(bytes.size(), 22u) << "account=" << account;
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
    // base58 is O(n^2); the cap bounds CPU on attacker-controlled strings.
    std::string const huge(2048, 'r');
    EXPECT_FALSE(catl::base58::xrpl_codec.decode(huge));
    EXPECT_FALSE(catl::base58::decode_account_id(huge));

    std::vector<std::uint8_t> const big(2048, 0x41);
    EXPECT_TRUE(catl::base58::xrpl_codec.encode(big.data(), big.size()).empty());
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
