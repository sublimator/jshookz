#pragma once

#include "catl/xdata/amount-view.h"
#include "catl/xdata/certified-index.h"
#include "catl/xdata/codecs/codecs.h"
#include "catl/xdata/json-visitor.h"
#include "catl/xdata/parser-context.h"
#include "catl/xdata/parser.h"
#include "catl/xdata/scan.h"

#include <cstdio>
#include <exception>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace oracle_run {

inline constexpr char const* kZeroAccount =
    "rrrrrrrrrrrrrrrrrrrrrhoLvTp";
inline constexpr char const* kOracleCommit =
    "cb829d7657607643f0bdc29c65f9a41fbd86a688";

inline bool
corpus_provenance_ok(boost::json::object const& root, std::string& err)
{
    if (!root.contains("oracle_commit") || !root.at("oracle_commit").is_string())
    {
        err = "missing root oracle_commit";
        return false;
    }
    auto const pin = std::string(root.at("oracle_commit").as_string());
    if (pin != kOracleCommit)
    {
        err = "root oracle_commit is not the pinned vectors commit";
        return false;
    }
    if (!root.contains("cases") || !root.at("cases").is_array())
    {
        err = "missing cases";
        return false;
    }
    for (auto const& item : root.at("cases").as_array())
    {
        auto const& c = item.as_object();
        std::string const id = c.contains("id") && c.at("id").is_string()
            ? std::string(c.at("id").as_string())
            : "<missing-id>";
        if (!c.contains("oracle_commit") || !c.at("oracle_commit").is_string())
        {
            err = id + " missing oracle_commit";
            return false;
        }
        if (std::string(c.at("oracle_commit").as_string()) != pin)
        {
            err = id + " mixed oracle_commit";
            return false;
        }
    }
    return true;
}

// Cartesian header-enumerator IDs. Independent of the JSON file so deleting
// hdr-* rows cannot stay green. Must match export_oracle_corpus.py.
inline std::set<std::string>
header_enum_ids()
{
    std::set<std::string> ids;
    char buf[64];
    for (int b = 0; b < 256; ++b)
    {
        std::snprintf(buf, sizeof(buf), "hdr-b-%02X", b);
        ids.insert(buf);
    }
    static constexpr int kTypeBytes[] = {0, 1, 5, 15, 16, 26, 255};
    static constexpr int kNameNibbles[] = {0x1, 0x5, 0xE};
    for (int tb : kTypeBytes)
    {
        for (int nn : kNameNibbles)
        {
            std::snprintf(buf, sizeof(buf), "hdr-t-%02X-n-%X", tb, nn);
            ids.insert(buf);
        }
    }
    static constexpr int kTypeNibbles[] = {1, 8, 14};
    static constexpr int kNameBytes[] = {0, 1, 5, 15, 16, 255};
    for (int tn : kTypeNibbles)
    {
        for (int nb : kNameBytes)
        {
            std::snprintf(buf, sizeof(buf), "hdr-n-t%X-f%02X", tn, nb);
            ids.insert(buf);
        }
    }
    static constexpr char const* kArr[] = {
        "E1", "E032E1", "E032E1F1", "F1", "99", "0105"};
    for (char const* inner : kArr)
    {
        std::string id = std::string("hdr-arr-") + inner;
        ids.insert(id);
        std::string_view iv{inner};
        if (iv != "F1" && !iv.ends_with("F1"))
            ids.insert(id + "-F1");
    }
    return ids;
}

inline std::set<std::string>
amount_boundary_ids()
{
    return {
        "amount-iou-exp-m96",
        "amount-iou-exp-80",
        "amount-iou-exp-m97",
        "amount-iou-exp-81",
        "amount-iou-mant-max",
        "amount-iou-mant-over",
        "amount-mpt-negative",
    };
}

inline bool
amount_boundary_complete(boost::json::object const& root, std::string& err)
{
    if (!root.contains("cases") || !root.at("cases").is_array())
    {
        err = "missing cases";
        return false;
    }
    std::set<std::string> have;
    for (auto const& item : root.at("cases").as_array())
    {
        auto const& c = item.as_object();
        if (c.contains("id") && c.at("id").is_string())
            have.insert(std::string(c.at("id").as_string()));
    }
    for (auto const& id : amount_boundary_ids())
    {
        if (!have.count(id))
        {
            err = "missing amount boundary " + id;
            return false;
        }
    }
    return true;
}

inline bool
header_enum_complete(boost::json::object const& root, std::string& err)
{
    if (!root.contains("cases") || !root.at("cases").is_array())
    {
        err = "missing cases";
        return false;
    }
    std::set<std::string> have;
    for (auto const& item : root.at("cases").as_array())
    {
        auto const& c = item.as_object();
        if (c.contains("id") && c.at("id").is_string())
            have.insert(std::string(c.at("id").as_string()));
    }
    auto const want = header_enum_ids();
    for (auto const& id : want)
    {
        if (!have.count(id))
        {
            err = "missing header enum " + id;
            return false;
        }
    }
    return true;
}

inline std::uint8_t
hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
        return static_cast<std::uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f')
        return static_cast<std::uint8_t>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F')
        return static_cast<std::uint8_t>(c - 'A' + 10);
    return 0;
}

inline std::vector<std::uint8_t>
decode_hex(std::string_view hex)
{
    std::vector<std::uint8_t> out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
        out.push_back(static_cast<std::uint8_t>(
            (hex_nibble(hex[i]) << 4) | hex_nibble(hex[i + 1])));
    return out;
}

struct Outcomes
{
    bool locate_ok = false;
    bool certify_null_ok = false;
    bool certify_index_ok = false;
    bool sinks_agree = false;
    bool decode_frames_ok = false;
    bool names_ok = true;
    bool json_ok = true;
    bool amount_parts_ok = true;
    bool consumed_all = false;
    std::string locate_err;
    std::string certify_err;
    std::string decode_err;
    std::string names_err;
    std::string json_err;
    std::string amount_parts_err;
    std::size_t frame_count = 0;
    std::uint32_t locate_end = 0;
    std::uint32_t certify_end = 0;
    std::size_t blob_size = 0;
};

inline void
collect_field_names(boost::json::value const& v, std::set<std::string>& out)
{
    if (v.is_array())
    {
        for (auto const& item : v.as_array())
            collect_field_names(item, out);
        return;
    }
    if (!v.is_object())
        return;
    auto const& o = v.as_object();
    if (auto const* name = o.if_contains("name"); name && name->is_string())
        out.insert(std::string(name->as_string()));
    if (auto const* els = o.if_contains("elements"))
        collect_field_names(*els, out);
    if (auto const* fields = o.if_contains("fields"))
        collect_field_names(*fields, out);
}

inline void
normalize_json(boost::json::value& v)
{
    if (v.is_string())
    {
        if (v.as_string() == kZeroAccount)
            v = "";
        else if (v.as_string() == "XRP")
            v = "XAH";
        return;
    }
    if (v.is_object())
    {
        auto& o = v.as_object();
        if (o.contains("account") && o.contains("type"))
            o.erase("type");
        std::vector<std::string> empty_arrays;
        for (auto& kv : o)
        {
            normalize_json(kv.value());
            if (kv.value().is_array() && kv.value().as_array().empty())
                empty_arrays.emplace_back(kv.key());
        }
        for (auto const& k : empty_arrays)
            o.erase(k);
        return;
    }
    if (v.is_array())
    {
        for (auto& item : v.as_array())
            normalize_json(item);
    }
}

inline bool
json_equiv(boost::json::value const& got, boost::json::value const& want)
{
    if (got == want)
        return true;
    if (got.is_string() && want.is_object())
    {
        auto const& o = want.as_object();
        return o.size() == 1 && o.contains("currency") &&
            o.at("currency") == got;
    }
    if (want.is_string() && got.is_object())
    {
        auto const& o = got.as_object();
        return o.size() == 1 && o.contains("currency") &&
            o.at("currency") == want;
    }
    if (got.is_object() && want.is_object())
    {
        auto const& a = got.as_object();
        auto const& b = want.as_object();
        if (a.size() != b.size())
            return false;
        for (auto const& kv : a)
        {
            auto const* other = b.if_contains(kv.key());
            if (!other || !json_equiv(kv.value(), *other))
                return false;
        }
        return true;
    }
    if (got.is_array() && want.is_array())
    {
        auto const& a = got.as_array();
        auto const& b = want.as_array();
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
        {
            if (!json_equiv(a[i], b[i]))
                return false;
        }
        return true;
    }
    return false;
}

inline bool
decode_leaf_frames(
    catl::xdata::CertifiedIndex const& idx,
    catl::xdata::Protocol const& protocol,
    std::string& err)
{
    using namespace catl::xdata;
    for (size_t i = 0; i < idx.frame_count(); ++i)
    {
        FieldFrame const& f = idx.frame(i);
        if (f.wire_end < f.payload_begin || f.wire_end > idx.backing().size())
        {
            err = "frame out of range";
            return false;
        }
        FieldDef const* field = protocol.get_field_by_code(f.field_code);
        if (!field)
        {
            err = "missing field def";
            return false;
        }
        auto const& t = field->meta.type;
        if (t == FieldTypes::STObject || t == FieldTypes::STArray)
            continue;
        if (t == FieldTypes::Amount)
        {
            auto view = AmountView::bind(idx, i);
            if (!view)
            {
                err = "amount bind failed";
                return false;
            }
            (void)codecs::AmountCodec::json_from_parts(view->parts());
            continue;
        }
        Slice payload{
            idx.backing().data() + f.payload_begin,
            f.wire_end - f.payload_begin};
        try
        {
            (void)codecs::decode_field_value(*field, payload, protocol);
        }
        catch (std::exception const& e)
        {
            err = e.what();
            return false;
        }
    }
    return true;
}

inline bool
compare_amount_parts(
    catl::xdata::CertifiedIndex const& idx,
    catl::xdata::Protocol const& protocol,
    boost::json::value const* fields,
    std::string& err)
{
    using namespace catl::xdata;
    if (!fields || !fields->is_array())
        return true;
    for (auto const& item : fields->as_array())
    {
        if (!item.is_object())
            continue;
        auto const& o = item.as_object();
        auto const* parts = o.if_contains("parts");
        if (!parts || !parts->is_object())
            continue;
        std::string want_name;
        if (auto const* n = o.if_contains("name"); n && n->is_string())
            want_name = std::string(n->as_string());
        bool matched = false;
        for (size_t i = 0; i < idx.frame_count(); ++i)
        {
            FieldDef const* field =
                protocol.get_field_by_code(idx.frame(i).field_code);
            if (!field || field->meta.type != FieldTypes::Amount)
                continue;
            if (!want_name.empty() && field->name != want_name)
                continue;
            auto view = AmountView::bind(idx, i);
            if (!view)
            {
                err = "amount bind failed for " + field->name;
                return false;
            }
            auto got = codecs::AmountCodec::oracle_parts(view->parts());
            if (!json_equiv(got, *parts))
            {
                err = "amount parts mismatch for " + field->name +
                    " got=" + boost::json::serialize(got) +
                    " want=" + boost::json::serialize(*parts);
                return false;
            }
            matched = true;
            break;
        }
        if (!matched && !want_name.empty())
        {
            err = "no Amount frame for " + want_name;
            return false;
        }
        if (!matched)
        {
            err = "oracle Amount parts with no bindable frame";
            return false;
        }
    }
    return true;
}

inline bool
compare_frame_names(
    catl::xdata::CertifiedIndex const& idx,
    catl::xdata::Protocol const& protocol,
    boost::json::value const* fields,
    std::string& err)
{
    if (!fields)
        return true;
    std::set<std::string> got;
    for (size_t i = 0; i < idx.frame_count(); ++i)
    {
        auto const* field =
            protocol.get_field_by_code(idx.frame(i).field_code);
        if (!field)
        {
            err = "frame missing field def";
            return false;
        }
        got.insert(field->name);
    }
    std::set<std::string> want;
    collect_field_names(*fields, want);
    if (got != want)
    {
        err = "frame names != oracle fields";
        return false;
    }
    return true;
}

inline bool
decode_whole_json(
    catl::xdata::Protocol const& protocol,
    Slice backing,
    boost::json::value const* oracle_json,
    std::string& err)
{
    if (!oracle_json)
        return true;
    try
    {
        catl::xdata::ParserContext ctx{backing};
        catl::xdata::JsonVisitor visitor(
            protocol, catl::xdata::JsonVisitor::Options{false});
        catl::xdata::parse_with_visitor(ctx, protocol, visitor);
        auto got = visitor.get_result();
        auto want = *oracle_json;
        normalize_json(got);
        normalize_json(want);
        if (!json_equiv(got, want))
        {
            err = "json mismatch got=" + boost::json::serialize(got) +
                " want=" + boost::json::serialize(want);
            return false;
        }
    }
    catch (std::exception const& e)
    {
        err = e.what();
        return false;
    }
    return true;
}

inline Outcomes
run_stobject(
    catl::xdata::Protocol const& protocol,
    std::string_view hex,
    boost::json::value const* fields = nullptr,
    boost::json::value const* oracle_json = nullptr,
    std::string_view canonical_hex = {})
{
    using namespace catl::xdata;
    Outcomes o;
    auto bytes = decode_hex(hex);
    o.blob_size = bytes.size();
    Slice backing{bytes.data(), bytes.size()};

    NullSink locate_sink;
    auto loc = scan_scope<ScanMode::Locate>(backing, 0, protocol, locate_sink);
    o.locate_ok = loc.has_value();
    if (loc)
        o.locate_end = *loc;
    else
        o.locate_err = loc.error().message;

    NullSink null_sink;
    auto cn = scan_scope<ScanMode::CertifyWire>(backing, 0, protocol, null_sink);
    o.certify_null_ok = cn.has_value();
    if (cn)
        o.certify_end = *cn;
    else
        o.certify_err = cn.error().message;

    auto idx = certify_indexed(backing, 0, protocol);
    o.certify_index_ok = idx.has_value();
    o.sinks_agree = o.certify_null_ok == o.certify_index_ok;
    o.frame_count = idx ? idx->frame_count() : 0;
    o.consumed_all = o.locate_ok && o.certify_null_ok &&
        o.locate_end == bytes.size() && o.certify_end == bytes.size();

    if (idx)
    {
        o.decode_frames_ok = decode_leaf_frames(*idx, protocol, o.decode_err);
        o.names_ok = compare_frame_names(*idx, protocol, fields, o.names_err);
        o.amount_parts_ok =
            compare_amount_parts(*idx, protocol, fields, o.amount_parts_err);
        auto canon = canonical_hex.empty() ? std::vector<std::uint8_t>{}
                                           : decode_hex(canonical_hex);
        Slice json_backing = canon.empty() ? backing
                                           : Slice{canon.data(), canon.size()};
        o.json_ok = decode_whole_json(protocol, json_backing, oracle_json, o.json_err);
    }
    return o;
}

inline Outcomes
run_amount(
    catl::xdata::Protocol const& protocol,
    std::string_view hex,
    boost::json::value const* fields = nullptr,
    boost::json::value const* oracle_json = nullptr)
{
    using namespace catl::xdata;
    Outcomes o;
    auto bytes = decode_hex(hex);
    o.blob_size = bytes.size();
    if (bytes.empty())
    {
        o.locate_err = "empty";
        return o;
    }
    Slice payload{bytes.data(), bytes.size()};
    size_t n = AmountRules::extent(bytes[0]);
    o.locate_ok = bytes.size() == n;
    o.locate_end = o.locate_ok ? static_cast<std::uint32_t>(bytes.size()) : 0;
    char const* c = AmountRules::certify(payload);
    o.certify_null_ok = c == nullptr;
    if (c)
        o.certify_err = c;
    if (!o.locate_ok)
        o.locate_err = "amount extent mismatch";
    auto idx = certify_amount_span(payload, protocol);
    o.certify_index_ok = idx.has_value();
    o.sinks_agree = o.certify_null_ok == o.certify_index_ok;
    o.consumed_all = o.locate_ok && o.certify_null_ok;
    if (idx)
    {
        o.frame_count = idx->frame_count();
        auto view = AmountView::bind(*idx, 0);
        if (!view)
        {
            o.decode_err = "amount bind failed";
        }
        else
        {
            auto got = codecs::AmountCodec::json_from_parts(view->parts());
            o.decode_frames_ok = true;
            o.amount_parts_ok =
                compare_amount_parts(*idx, protocol, fields, o.amount_parts_err);
            if (oracle_json)
            {
                auto want = *oracle_json;
                normalize_json(got);
                normalize_json(want);
                o.json_ok = json_equiv(got, want);
                if (!o.json_ok)
                {
                    o.json_err = "amount json mismatch got=" +
                        boost::json::serialize(got) +
                        " want=" + boost::json::serialize(want);
                }
            }
        }
    }
    return o;
}

inline Outcomes
run_pathset(std::string_view hex)
{
    using namespace catl::xdata;
    Outcomes o;
    auto bytes = decode_hex(hex);
    o.blob_size = bytes.size();
    Slice backing{bytes.data(), bytes.size()};
    ParserContext loc_ctx{backing};
    scan_detail::scan_pathset(loc_ctx, ScanMode::Locate);
    o.locate_ok = !loc_ctx.failed();
    if (o.locate_ok)
        o.locate_end = static_cast<std::uint32_t>(loc_ctx.pos());
    else
        o.locate_err = loc_ctx.as_error().message;
    ParserContext cert_ctx{backing};
    scan_detail::scan_pathset(cert_ctx, ScanMode::CertifyWire);
    o.certify_null_ok = !cert_ctx.failed();
    o.certify_index_ok = o.certify_null_ok;
    o.sinks_agree = true;
    o.consumed_all = o.locate_ok && o.certify_null_ok &&
        o.locate_end == bytes.size();
    if (cert_ctx.failed())
        o.certify_err = cert_ctx.as_error().message;
    if (o.certify_null_ok)
    {
        try
        {
            (void)codecs::PathSetCodec::decode(Slice{bytes.data(), bytes.size()});
            o.decode_frames_ok = true;
        }
        catch (std::exception const& e)
        {
            o.decode_err = e.what();
        }
    }
    return o;
}

}  // namespace oracle_run
