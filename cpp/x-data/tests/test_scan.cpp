#include "oracle_run.hpp"

#include <catl/xdata/protocol.h>
#include <catl/xdata/scan.h>

#include <gtest/gtest.h>

#include <boost/json.hpp>

#include <fstream>
#include <set>
#include <sstream>

#ifndef JSHOOKZ_ORACLE_CORPUS_JSON
#error "JSHOOKZ_ORACLE_CORPUS_JSON is required"
#endif

using namespace catl::xdata;

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
        return oracle_run::run_pathset(blob);
    auto const* fields = c.if_contains("fields");
    auto const* js = c.if_contains("json");
    std::string canonical;
    if (c.contains("canonical_blob"))
        canonical = std::string(c.at("canonical_blob").as_string());
    return oracle_run::run_stobject(protocol, blob, fields, js, canonical);
}

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
