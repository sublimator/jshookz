#include "oracle_run.hpp"

#include <catl/xdata/protocol.h>
#include <catl/xdata/scan.h>

#include <gtest/gtest.h>

#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <set>
#include <sstream>

#ifndef JSHOOKZ_ORACLE_CORPUS_JSON
#error "JSHOOKZ_ORACLE_CORPUS_JSON is required"
#endif

using namespace catl::xdata;

namespace catl::xdata {

std::array<std::atomic<unsigned>, 5> g_pathset_route_calls{};

void
pathset_rules_test_hook(PathSetRuleRoute route) noexcept
{
    g_pathset_route_calls[static_cast<size_t>(route)].fetch_add(
        1, std::memory_order_relaxed);
}

}  // namespace catl::xdata

namespace {

boost::json::value
load_corpus()
{
    std::ifstream in(JSHOOKZ_ORACLE_CORPUS_JSON);
    std::ostringstream ss;
    ss << in.rdbuf();
    return boost::json::parse(ss.str());
}

oracle_run::Outcomes
run_case(Protocol const& protocol, boost::json::object const& c)
{
    auto const type = std::string(c.at("codec_type").as_string());
    auto const blob = std::string(c.at("blob").as_string());
    if (type == "amount")
        return oracle_run::run_amount(
            protocol, blob, c.if_contains("fields"), c.if_contains("json"));
    if (type == "pathset")
        return oracle_run::run_pathset(protocol, blob, c.if_contains("json"));
    auto const* fields = c.if_contains("fields");
    auto const* js = c.if_contains("json");
    std::string canonical;
    if (c.contains("canonical_blob"))
        canonical = std::string(c.at("canonical_blob").as_string());
    return oracle_run::run_stobject(protocol, blob, fields, js, canonical);
}

void
reset_pathset_route_counts()
{
    for (auto& value : g_pathset_route_calls)
        value.store(0, std::memory_order_relaxed);
}

unsigned
pathset_route_count(PathSetRuleRoute route)
{
    return g_pathset_route_calls[static_cast<size_t>(route)].load(
        std::memory_order_relaxed);
}

struct CapturePathSetField
{
    size_t visits = 0;
    size_t bytes = 0;
    uint8_t last = 0xff;
    uint8_t const* data = nullptr;

    void
    visit_field(FieldPath const&, FieldSlice const& field)
    {
        ++visits;
        bytes = field.data.size();
        data = field.data.data();
        if (!field.data.empty())
            last = field.data.data()[field.data.size() - 1];
    }
};

struct CountPathSetEvents
{
    size_t paths = 0;
    size_t hops = 0;

    void
    on_hop(PathSetHop const&) noexcept
    {
        ++hops;
    }

    void
    on_path_end() noexcept
    {
        ++paths;
    }

    void
    on_end() const noexcept
    {
    }
};

}  // namespace

TEST(LookupTable, DimensionsAre32By128)
{
    EXPECT_EQ(Protocol::kFastTypeDim, 32);
    EXPECT_EQ(Protocol::kFastNthDim, 128);
    EXPECT_EQ(
        Protocol::fast_lookup_bytes(),
        sizeof(FieldDef const*) * 32u * 128u);
    auto const old_bytes = sizeof(FieldDef const*) * 256u * 256u;
    EXPECT_EQ(old_bytes / Protocol::fast_lookup_bytes(), 16u);
}

TEST(LookupTable, EmbeddedXahauFitsAndCountsFallbacks)
{
    auto const p = Protocol::load_embedded_xahau_protocol();
    EXPECT_LT(p.max_serialized_type_code(), Protocol::kFastTypeDim);
    // Some nth values exceed 128 (definitions go to 259+). Those are the
    // counted slow-path remainder, together with type codes 10001+.
    EXPECT_GT(p.max_serialized_nth(), 0u);
    EXPECT_GT(p.fast_lookup_fallback_count(), 0u);
}

TEST(ScanCorpus, LocateCertifyDecodeMatchOracleExpect)
{
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    auto const root = load_corpus().as_object();
    auto const& cases = root.at("cases").as_array();
    ASSERT_FALSE(cases.empty());

    for (auto const& item : cases)
    {
        auto const& c = item.as_object();
        auto const id = std::string(c.at("id").as_string());
        auto const expect = std::string(c.at("expect").as_string());
        auto const o = run_case(protocol, c);
        EXPECT_TRUE(o.sinks_agree) << id << " NullSink vs IndexSink";
        bool const trailing_ok =
            c.contains("trailing_ok") && c.at("trailing_ok").as_bool();
        bool const header_enum = id.rfind("hdr-", 0) == 0;
        bool const wire_ok =
            c.contains("wire_ok") && c.at("wire_ok").as_bool();
        if (wire_ok)
        {
            EXPECT_TRUE(o.certify_null_ok)
                << id << " template-only oracle reject, wire must certify "
                << o.certify_err;
            EXPECT_TRUE(o.sinks_agree) << id;
            continue;
        }
        if (expect == "accept")
        {
            EXPECT_TRUE(o.locate_ok) << id << " locate " << o.locate_err;
            EXPECT_TRUE(o.certify_null_ok)
                << id << " certify " << o.certify_err;
            EXPECT_TRUE(o.decode_frames_ok)
                << id << " decode " << o.decode_err;
            EXPECT_TRUE(o.names_ok) << id << " names " << o.names_err;
            EXPECT_TRUE(o.amount_parts_ok)
                << id << " parts " << o.amount_parts_err;
            if (!header_enum)
            {
                EXPECT_TRUE(o.json_ok) << id << " json " << o.json_err;
            }
            if (!trailing_ok)
            {
                EXPECT_TRUE(o.consumed_all)
                    << id << " end locate=" << o.locate_end
                    << " certify=" << o.certify_end
                    << " size=" << o.blob_size;
            }
            else
            {
                EXPECT_LT(o.locate_end, o.blob_size) << id;
            }
        }
        else
        {
            EXPECT_FALSE(o.certify_null_ok) << id << " should reject";
        }
    }
}

TEST(ScanCorpus, RequiredIdsPresent)
{
    auto const root = load_corpus().as_object();
    std::set<std::string> ids;
    for (auto const& item : root.at("cases").as_array())
        ids.insert(std::string(item.as_object().at("id").as_string()));
    for (char const* need : {
             "stobject-account-20",
             "stobject-account-empty-vl",
             "stobject-account-zero-20",
             "stobject-account-vl-1",
             "stobject-account-vl-1-truncated",
             "stobject-account-vl-19",
             "stobject-account-vl-19-truncated",
             "stobject-account-vl-21",
             "stobject-account-vl-21-truncated",
             "stobject-nop-63",
             "stobject-nop-64",
             "stobject-duplicate-account",
             "stobject-seq-account-out-of-order",
             "stobject-trailing-ff",
             "stobject-pathset",
             "stobject-pathset-truncated",
             "stobject-native-amount",
             "stobject-iou-amount",
             "amount-native-zero",
             "amount-native-negative",
             "amount-native-neg-zero",
             "stobject-iou-negative",
             "amount-mpt-negative",
             "amount-iou-exp-m96",
             "amount-iou-exp-80",
             "amount-iou-exp-m97",
             "amount-iou-exp-81",
             "amount-iou-mant-max",
             "amount-iou-mant-over",
             "stobject-vl-blob",
             "stobject-nested-memos",
             "stobject-array-nop-1",
             "stobject-array-nop-64",
             "stobject-xchain-bridge",
             "stobject-xchain-issue-native-mismatch",
             "stobject-issue-usd-zero-account",
             "stobject-e1-then-ff",
         })
    {
        EXPECT_TRUE(ids.count(need)) << need;
    }
}

TEST(ScanCorpus, AmountBoundaryIdsPresent)
{
    auto const root = load_corpus().as_object();
    std::string err;
    EXPECT_TRUE(oracle_run::amount_boundary_complete(root, err)) << err;
}

TEST(ScanCorpus, UInt32BoundaryIdsPresent)
{
    auto const root = load_corpus().as_object();
    std::string err;
    EXPECT_TRUE(oracle_run::uint32_boundary_complete(root, err)) << err;
}

TEST(ScanCorpus, UInt32BoundaryDeletionIsRejected)
{
    auto root = load_corpus().as_object();
    auto& cases = root.at("cases").as_array();
    auto const found = std::find_if(
        cases.begin(), cases.end(), [](boost::json::value const& item) {
            auto const& c = item.as_object();
            return c.contains("id") &&
                   c.at("id").as_string() == "stobject-uint32-max";
        });
    ASSERT_NE(found, cases.end());
    cases.erase(found);

    std::string err;
    EXPECT_FALSE(oracle_run::uint32_boundary_complete(root, err));
    EXPECT_EQ(err, "missing UInt32 boundary stobject-uint32-max");
}

TEST(ScanCorpus, AccountIDBoundaryIdsPresent)
{
    auto const root = load_corpus().as_object();
    std::string err;
    EXPECT_TRUE(oracle_run::account_id_boundary_complete(root, err)) << err;
}

TEST(ScanCorpus, AccountIDBoundaryDeletionIsRejected)
{
    auto root = load_corpus().as_object();
    auto& cases = root.at("cases").as_array();
    auto const found = std::find_if(
        cases.begin(), cases.end(), [](boost::json::value const& item) {
            auto const& c = item.as_object();
            return c.contains("id") &&
                   c.at("id").as_string() == "stobject-account-vl-21";
        });
    ASSERT_NE(found, cases.end());
    cases.erase(found);

    std::string err;
    EXPECT_FALSE(oracle_run::account_id_boundary_complete(root, err));
    EXPECT_EQ(err, "missing AccountID boundary stobject-account-vl-21");
}

TEST(ScanCorpus, PathSetBoundaryIdsPresent)
{
    auto const root = load_corpus().as_object();
    std::string err;
    EXPECT_TRUE(oracle_run::pathset_boundary_complete(root, err)) << err;
}

TEST(ScanCorpus, PathSetBoundaryDeletionIsRejected)
{
    auto root = load_corpus().as_object();
    auto& cases = root.at("cases").as_array();
    auto const found = std::find_if(
        cases.begin(), cases.end(), [](boost::json::value const& item) {
            auto const& c = item.as_object();
            return c.contains("id") &&
                   c.at("id").as_string() == "pathset-mask-aci";
        });
    ASSERT_NE(found, cases.end());
    cases.erase(found);

    std::string err;
    EXPECT_FALSE(oracle_run::pathset_boundary_complete(root, err));
    EXPECT_EQ(err, "missing PathSet boundary pathset-mask-aci");
}

TEST(ScanCorpus, EveryPathSetBoundaryExercisesScannerCertificateAndView)
{
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    auto const root = load_corpus().as_object();
    std::set<std::string> const locate_only_rejections{"pathset-empty",
        "pathset-leading-separator", "pathset-doubled-separator",
        "pathset-trailing-separator", "pathset-illegal-low-bit",
        "pathset-illegal-high-bit"};
    std::set<std::string> seen;
    size_t accepted_count = 0;
    size_t rejected_count = 0;

    for (auto const& item : root.at("cases").as_array())
    {
        auto const& c = item.as_object();
        if (std::string(c.at("codec_type").as_string()) != "pathset")
            continue;
        std::string const id(c.at("id").as_string());
        seen.insert(id);
        bool const accepted =
            std::string(c.at("expect").as_string()) == "accept";
        accepted_count += accepted;
        rejected_count += !accepted;
        auto const payload =
            oracle_run::decode_hex(std::string(c.at("blob").as_string()));
        std::vector<uint8_t> object{0x01, 0x12};
        object.insert(object.end(), payload.begin(), payload.end());
        Slice const backing{object.data(), object.size()};

        reset_pathset_route_counts();
        NullSink locate_sink;
        auto const located =
            scan_scope<ScanMode::Locate>(backing, 0, protocol, locate_sink);
        bool const locate_expected =
            accepted || locate_only_rejections.count(id) != 0;
        EXPECT_EQ(located.has_value(), locate_expected) << id;
        if (located)
            EXPECT_EQ(*located, object.size()) << id;
        EXPECT_EQ(pathset_route_count(PathSetRuleRoute::Locate), 1u) << id;
        EXPECT_EQ(pathset_route_count(PathSetRuleRoute::CertifyWire), 0u) << id;
        EXPECT_EQ(pathset_route_count(PathSetRuleRoute::TraverseAdmitted), 0u)
            << id;
        EXPECT_EQ(pathset_route_count(PathSetRuleRoute::MeasureDirectory), 0u)
            << id;
        EXPECT_EQ(pathset_route_count(PathSetRuleRoute::FillDirectory), 0u)
            << id;

        reset_pathset_route_counts();
        NullSink certify_sink;
        auto const scanned = scan_scope<ScanMode::CertifyWire>(
            backing, 0, protocol, certify_sink);
        EXPECT_EQ(scanned.has_value(), accepted) << id;
        if (scanned)
            EXPECT_EQ(*scanned, object.size()) << id;
        EXPECT_EQ(pathset_route_count(PathSetRuleRoute::CertifyWire), 1u) << id;
        EXPECT_EQ(pathset_route_count(PathSetRuleRoute::Locate), 0u) << id;
        EXPECT_EQ(pathset_route_count(PathSetRuleRoute::TraverseAdmitted), 0u)
            << id;
        EXPECT_EQ(pathset_route_count(PathSetRuleRoute::MeasureDirectory), 0u)
            << id;
        EXPECT_EQ(pathset_route_count(PathSetRuleRoute::FillDirectory), 0u)
            << id;

        reset_pathset_route_counts();
        auto const idx = certify_indexed(backing, 0, protocol);
        EXPECT_EQ(idx.has_value(), accepted) << id;
        EXPECT_EQ(pathset_route_count(PathSetRuleRoute::CertifyWire), 1u) << id;
        EXPECT_EQ(pathset_route_count(PathSetRuleRoute::Locate), 0u) << id;
        EXPECT_EQ(pathset_route_count(PathSetRuleRoute::TraverseAdmitted), 0u)
            << id;
        EXPECT_EQ(pathset_route_count(PathSetRuleRoute::MeasureDirectory), 0u)
            << id;
        EXPECT_EQ(pathset_route_count(PathSetRuleRoute::FillDirectory), 0u)
            << id;

        if (!accepted)
            continue;
        ASSERT_TRUE(idx) << id;
        ASSERT_EQ(idx->frame_count(), 1u) << id;

        reset_pathset_route_counts();
        auto const view = PathSetView::bind(*idx, 0);
        ASSERT_TRUE(view) << id;
        EXPECT_EQ(view->payload().data(), object.data() + 2) << id;
        EXPECT_EQ(view->payload().size(), payload.size()) << id;
        EXPECT_EQ(pathset_route_count(PathSetRuleRoute::Locate), 0u) << id;
        EXPECT_EQ(pathset_route_count(PathSetRuleRoute::CertifyWire), 0u) << id;
        EXPECT_EQ(pathset_route_count(PathSetRuleRoute::TraverseAdmitted), 0u)
            << id;
        EXPECT_EQ(pathset_route_count(PathSetRuleRoute::MeasureDirectory), 0u)
            << id;
        EXPECT_EQ(pathset_route_count(PathSetRuleRoute::FillDirectory), 0u)
            << id;

        reset_pathset_route_counts();
        CountPathSetEvents events;
        EXPECT_TRUE(view->traverse(events)) << id;
        EXPECT_GT(events.paths, 0u) << id;
        EXPECT_GT(events.hops, 0u) << id;
        EXPECT_EQ(pathset_route_count(PathSetRuleRoute::TraverseAdmitted), 1u)
            << id;
        EXPECT_EQ(pathset_route_count(PathSetRuleRoute::Locate), 0u) << id;
        EXPECT_EQ(pathset_route_count(PathSetRuleRoute::CertifyWire), 0u) << id;
        EXPECT_EQ(pathset_route_count(PathSetRuleRoute::MeasureDirectory), 0u)
            << id;
        EXPECT_EQ(pathset_route_count(PathSetRuleRoute::FillDirectory), 0u)
            << id;
    }

    EXPECT_EQ(seen, oracle_run::pathset_boundary_ids());
    EXPECT_EQ(seen.size(), 22u);
    EXPECT_EQ(accepted_count, 9u);
    EXPECT_EQ(rejected_count, 13u);
}

TEST(ScanCorpus, EveryPathSetBoundaryUsesVisitorCertifyWithoutRewind)
{
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    auto const root = load_corpus().as_object();
    std::set<std::string> seen;
    size_t accepted_count = 0;
    size_t rejected_count = 0;
    for (auto const& item : root.at("cases").as_array())
    {
        auto const& c = item.as_object();
        if (std::string(c.at("codec_type").as_string()) != "pathset")
            continue;
        std::string const id(c.at("id").as_string());
        seen.insert(id);
        auto const payload =
            oracle_run::decode_hex(std::string(c.at("blob").as_string()));
        std::vector<uint8_t> object{0x01, 0x12};
        object.insert(object.end(), payload.begin(), payload.end());
        ParserContext ctx{Slice{object.data(), object.size()}};
        CapturePathSetField visitor;
        reset_pathset_route_counts();
        bool threw = false;
        try
        {
            parse_with_visitor(ctx, protocol, visitor);
        }
        catch (std::exception const&)
        {
            threw = true;
        }
        bool const accepted =
            std::string(c.at("expect").as_string()) == "accept";
        accepted_count += accepted;
        rejected_count += !accepted;
        EXPECT_EQ(threw, !accepted) << id;
        EXPECT_EQ(pathset_route_count(PathSetRuleRoute::CertifyWire), 1u) << id;
        EXPECT_EQ(pathset_route_count(PathSetRuleRoute::Locate), 0u) << id;
        EXPECT_EQ(pathset_route_count(PathSetRuleRoute::TraverseAdmitted), 0u)
            << id;
        EXPECT_EQ(pathset_route_count(PathSetRuleRoute::MeasureDirectory), 0u)
            << id;
        EXPECT_EQ(pathset_route_count(PathSetRuleRoute::FillDirectory), 0u)
            << id;
        if (accepted)
        {
            EXPECT_EQ(visitor.visits, 1u) << id;
            EXPECT_EQ(visitor.bytes, payload.size()) << id;
            EXPECT_EQ(visitor.data, object.data() + 2) << id;
            if (visitor.data)
                EXPECT_TRUE(
                    std::equal(payload.begin(), payload.end(), visitor.data))
                    << id;
            EXPECT_EQ(visitor.last, PathSet::END_BYTE) << id;
            EXPECT_EQ(ctx.pos(), object.size()) << id;
        }
        else
        {
            EXPECT_EQ(visitor.visits, 0u) << id;
            EXPECT_TRUE(ctx.failed()) << id;
        }
    }
    EXPECT_EQ(seen, oracle_run::pathset_boundary_ids());
    EXPECT_EQ(seen.size(), 22u);
    EXPECT_EQ(accepted_count, 9u);
    EXPECT_EQ(rejected_count, 13u);
}

TEST(ScanCorpus, EveryRejectedPathSetFailsStrictCodecIndependently)
{
    std::set<std::string> const expected_rejections{"pathset-empty",
        "pathset-leading-separator", "pathset-doubled-separator",
        "pathset-trailing-separator", "pathset-illegal-low-bit",
        "pathset-illegal-high-bit", "pathset-truncated-account",
        "pathset-truncated-currency", "pathset-truncated-issuer",
        "pathset-aci-truncated-account", "pathset-aci-truncated-currency",
        "pathset-aci-truncated-issuer", "pathset-missing-end"};
    auto const root = load_corpus().as_object();
    std::set<std::string> seen_pathsets;
    std::set<std::string> seen_rejections;
    for (auto const& item : root.at("cases").as_array())
    {
        auto const& c = item.as_object();
        if (std::string(c.at("codec_type").as_string()) != "pathset")
            continue;
        std::string const id(c.at("id").as_string());
        seen_pathsets.insert(id);
        if (std::string(c.at("expect").as_string()) != "reject")
            continue;
        seen_rejections.insert(id);
        auto const bytes =
            oracle_run::decode_hex(std::string(c.at("blob").as_string()));

        reset_pathset_route_counts();
        EXPECT_THROW((void)codecs::PathSetCodec::decode(
                         Slice{bytes.data(), bytes.size()}),
            ParserError)
            << id;
        EXPECT_EQ(pathset_route_count(PathSetRuleRoute::CertifyWire), 1u) << id;
        EXPECT_EQ(pathset_route_count(PathSetRuleRoute::Locate), 0u) << id;
        EXPECT_EQ(pathset_route_count(PathSetRuleRoute::TraverseAdmitted), 0u)
            << id;
        EXPECT_EQ(pathset_route_count(PathSetRuleRoute::MeasureDirectory), 0u)
            << id;
        EXPECT_EQ(pathset_route_count(PathSetRuleRoute::FillDirectory), 0u)
            << id;
    }
    EXPECT_EQ(seen_pathsets, oracle_run::pathset_boundary_ids());
    EXPECT_EQ(seen_rejections, expected_rejections);
    EXPECT_EQ(seen_pathsets.size(), 22u);
    EXPECT_EQ(seen_rejections.size(), 13u);
}

TEST(ScanCorpus, SkipObjectIllegalMaskUsesCertifyAndRejects) {
  auto const protocol = Protocol::load_embedded_xahau_protocol();
  // Paths field followed by illegal hop mask 0x02 and the PathSet end.
  std::vector<uint8_t> object{0x01, 0x12, 0x02, 0x00};
  ParserContext ctx{Slice{object.data(), object.size()}};
  reset_pathset_route_counts();
  EXPECT_THROW(skip_object(ctx, protocol), std::exception);
  EXPECT_TRUE(ctx.failed());
  EXPECT_EQ(pathset_route_count(PathSetRuleRoute::Locate), 0u);
  EXPECT_EQ(pathset_route_count(PathSetRuleRoute::CertifyWire), 1u);
  EXPECT_EQ(pathset_route_count(PathSetRuleRoute::TraverseAdmitted), 0u);
  EXPECT_EQ(pathset_route_count(PathSetRuleRoute::MeasureDirectory), 0u);
  EXPECT_EQ(pathset_route_count(PathSetRuleRoute::FillDirectory), 0u);
}

TEST(ScanCorpus, SkipObjectEmptyPathSetUsesCertifyAndRejects) {
  auto const protocol = Protocol::load_embedded_xahau_protocol();
  std::vector<uint8_t> object{0x01, 0x12, 0x00};
  ParserContext ctx{Slice{object.data(), object.size()}};
  reset_pathset_route_counts();
  EXPECT_THROW(skip_object(ctx, protocol), std::exception);
  EXPECT_TRUE(ctx.failed());
  EXPECT_EQ(pathset_route_count(PathSetRuleRoute::Locate), 0u);
  EXPECT_EQ(pathset_route_count(PathSetRuleRoute::CertifyWire), 1u);
}

TEST(ScanCorpus, CertifyAndSkipRejectNumberConstructorOverflow) {
  auto const protocol = Protocol::load_embedded_xahau_protocol();
  std::vector<uint8_t> object{0x91, 0x7f, 0xff, 0xff, 0xff, 0xff, 0xff,
                              0xff, 0xff, 0x00, 0x00, 0x80, 0x00};
  Slice const backing{object.data(), object.size()};

  NullSink locate_sink;
  EXPECT_TRUE(scan_scope<ScanMode::Locate>(backing, 0, protocol, locate_sink));
  NullSink certify_sink;
  EXPECT_FALSE(
      scan_scope<ScanMode::CertifyWire>(backing, 0, protocol, certify_sink));

  ParserContext ctx{backing};
  EXPECT_THROW(skip_object(ctx, protocol), std::exception);
  EXPECT_TRUE(ctx.failed());
}

TEST(ScanCorpus, SkipObjectMissingPathSetEndUsesCertifyAndFails) {
  auto const protocol = Protocol::load_embedded_xahau_protocol();
  std::vector<uint8_t> object{0x01, 0x12, 0x01, 0xb5, 0xf7, 0x62, 0x79, 0x8a,
                              0x53, 0xd5, 0x43, 0xa0, 0x14, 0xca, 0xf8, 0xb2,
                              0x97, 0xcf, 0xf8, 0xf2, 0xf9, 0x37, 0xe8};
  ParserContext ctx{Slice{object.data(), object.size()}};
  reset_pathset_route_counts();
  EXPECT_THROW(skip_object(ctx, protocol), std::exception);
  EXPECT_TRUE(ctx.failed());
  EXPECT_EQ(ctx.pos(), object.size());
  EXPECT_EQ(pathset_route_count(PathSetRuleRoute::Locate), 0u);
  EXPECT_EQ(pathset_route_count(PathSetRuleRoute::CertifyWire), 1u);
  EXPECT_EQ(pathset_route_count(PathSetRuleRoute::TraverseAdmitted), 0u);
  EXPECT_EQ(pathset_route_count(PathSetRuleRoute::MeasureDirectory), 0u);
  EXPECT_EQ(pathset_route_count(PathSetRuleRoute::FillDirectory), 0u);
}

TEST(ScanCorpus, NegativeMptJsonMatchesView)
{
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    auto const root = load_corpus().as_object();
    for (auto const& item : root.at("cases").as_array())
    {
        auto const& c = item.as_object();
        if (std::string(c.at("id").as_string()) != "amount-mpt-negative")
            continue;
        auto o = oracle_run::run_amount(
            protocol,
            std::string(c.at("blob").as_string()),
            c.if_contains("fields"),
            c.if_contains("json"));
        EXPECT_TRUE(o.certify_null_ok);
        EXPECT_TRUE(o.json_ok) << o.json_err;
        return;
    }
    FAIL() << "amount-mpt-negative missing";
}

TEST(ScanCorpus, NegativeMptUnsignedJsonFails)
{
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    auto const root = load_corpus().as_object();
    for (auto const& item : root.at("cases").as_array())
    {
        auto c = item.as_object();
        if (std::string(c.at("id").as_string()) != "amount-mpt-negative")
            continue;
        boost::json::object unsigned_json;
        unsigned_json["value"] = "1";
        unsigned_json["mpt_issuance_id"] =
            "000000000000000000000000000000000000000000000000";
        boost::json::value jv = unsigned_json;
        auto o = oracle_run::run_amount(
            protocol,
            std::string(c.at("blob").as_string()),
            c.if_contains("fields"),
            &jv);
        EXPECT_FALSE(o.json_ok);
        return;
    }
    FAIL() << "amount-mpt-negative missing";
}

TEST(ScanNoThrow, MalformedUnknownFieldIsError)
{
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    auto o = oracle_run::run_stobject(protocol, "FF");
    EXPECT_FALSE(o.certify_null_ok);
    EXPECT_FALSE(o.locate_ok);
}

TEST(ScanCorpus, AcceptsRetainOracleEnvelope)
{
    auto const root = load_corpus().as_object();
    std::string err;
    EXPECT_TRUE(oracle_run::corpus_provenance_ok(root, err)) << err;
    for (auto const& item : root.at("cases").as_array())
    {
        auto const& c = item.as_object();
        if (std::string(c.at("expect").as_string()) != "accept")
            continue;
        if (std::string(c.at("codec_type").as_string()) != "stobject")
            continue;
        auto const id = std::string(c.at("id").as_string());
        EXPECT_TRUE(c.contains("fields")) << id;
        EXPECT_TRUE(c.contains("canonical_blob")) << id;
    }
}

TEST(ScanCorpus, MixedOracleCommitIsRejected)
{
    auto v = load_corpus();
    auto& root = v.as_object();
    ASSERT_FALSE(root.at("cases").as_array().empty());
    root.at("cases").as_array()[0].as_object()["oracle_commit"] = "deadbeef";
    std::string err;
    EXPECT_FALSE(oracle_run::corpus_provenance_ok(root, err));
    EXPECT_NE(err.find("mixed oracle_commit"), std::string::npos) << err;
}

TEST(ScanCorpus, HeaderEnumCartesianSetPresent)
{
    auto const root = load_corpus().as_object();
    std::string err;
    EXPECT_TRUE(oracle_run::header_enum_complete(root, err)) << err;
    EXPECT_EQ(oracle_run::header_enum_ids().size(), 305u);
}

TEST(ScanCorpus, HeaderEnumFamilyDeletionIsRejected)
{
    auto v = load_corpus();
    auto& cases = v.as_object().at("cases").as_array();
    boost::json::array kept;
    for (auto const& item : cases)
    {
        auto const id = std::string(item.as_object().at("id").as_string());
        if (id.rfind("hdr-", 0) != 0)
            kept.push_back(item);
    }
    v.as_object()["cases"] = std::move(kept);
    std::string err;
    EXPECT_FALSE(oracle_run::header_enum_complete(v.as_object(), err)) << err;
    EXPECT_NE(err.find("missing header enum hdr-"), std::string::npos) << err;
}

TEST(ScanCorpus, HeaderEnumSingleRowDeletionIsRejected)
{
    auto v = load_corpus();
    auto& cases = v.as_object().at("cases").as_array();
    boost::json::array kept;
    for (auto const& item : cases)
    {
        auto const id = std::string(item.as_object().at("id").as_string());
        if (id != "hdr-arr-E1")
            kept.push_back(item);
    }
    v.as_object()["cases"] = std::move(kept);
    std::string err;
    EXPECT_FALSE(oracle_run::header_enum_complete(v.as_object(), err)) << err;
    EXPECT_NE(err.find("hdr-arr-E1"), std::string::npos) << err;
}

TEST(ScanIssue, UsdWithNativeAccountIsCertifyReject)
{
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    auto const blob =
        "011914B5F762798A53D543A014CAF8B297CFF8F2F937E8"
                      "0000000000000000000000005553440000000000"
                      "0000000000000000000000000000000000000000"
                      "14B5F762798A53D543A014CAF8B297CFF8F2F937E8"
                      "0000000000000000000000000000000000000000";
    auto o = oracle_run::run_stobject(protocol, blob);
    EXPECT_TRUE(o.locate_ok) << o.locate_err;
    EXPECT_FALSE(o.certify_null_ok);
    EXPECT_FALSE(o.certify_index_ok);
    EXPECT_TRUE(o.sinks_agree);
    EXPECT_NE(
        o.certify_err.find("native mismatch"), std::string::npos)
        << o.certify_err;
}
