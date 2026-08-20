#pragma once

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
    bool consumed_all = false;
    std::string locate_err;
    std::string certify_err;
    std::string decode_err;
    std::string names_err;
    std::string json_err;
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
    catl::xdata::Protocol const& protocol,
    std::uint8_t const* bytes,
    std::size_t size,
    std::vector<catl::xdata::FieldFrame> const& frames,
    std::string& err)
{
    using namespace catl::xdata;
    for (auto const& f : frames)
    {
        if (f.wire_end < f.payload_begin || f.wire_end > size)
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
        Slice payload{bytes + f.payload_begin, f.wire_end - f.payload_begin};
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
compare_frame_names(
    catl::xdata::Protocol const& protocol,
    std::vector<catl::xdata::FieldFrame> const& frames,
    boost::json::value const* fields,
    std::string& err)
{
    if (!fields)
        return true;
    std::set<std::string> got;
    for (auto const& f : frames)
    {
        auto const* field = protocol.get_field_by_code(f.field_code);
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

    IndexSink index_sink;
    auto ci = scan_scope<ScanMode::CertifyWire>(backing, 0, protocol, index_sink);
    o.certify_index_ok = ci.has_value();
    o.sinks_agree = o.certify_null_ok == o.certify_index_ok;
    o.frame_count = index_sink.frames.size();
    o.consumed_all = o.locate_ok && o.certify_null_ok &&
        o.locate_end == bytes.size() && o.certify_end == bytes.size();

    if (o.certify_index_ok)
    {
        o.decode_frames_ok = decode_leaf_frames(
            protocol, bytes.data(), bytes.size(), index_sink.frames, o.decode_err);
        o.names_ok = compare_frame_names(
            protocol, index_sink.frames, fields, o.names_err);
        auto canon = canonical_hex.empty() ? std::vector<std::uint8_t>{}
                                           : decode_hex(canonical_hex);
        Slice json_backing = canon.empty() ? backing
                                           : Slice{canon.data(), canon.size()};
        o.json_ok = decode_whole_json(protocol, json_backing, oracle_json, o.json_err);
    }
    return o;
}

inline Outcomes
run_amount(std::string_view hex)
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
    size_t n = get_amount_size(bytes[0]);
    o.locate_ok = bytes.size() == n;
    o.locate_end = o.locate_ok ? static_cast<std::uint32_t>(bytes.size()) : 0;
    char const* c =
        scan_detail::certify_amount(Slice{bytes.data(), bytes.size()});
    o.certify_null_ok = c == nullptr;
    o.certify_index_ok = o.certify_null_ok;
    o.sinks_agree = true;
    o.consumed_all = o.locate_ok && o.certify_null_ok;
    if (c)
        o.certify_err = c;
    if (!o.locate_ok)
        o.locate_err = "amount extent mismatch";
    if (o.certify_null_ok)
    {
        try
        {
            (void)codecs::AmountCodec::decode(Slice{bytes.data(), bytes.size()});
            o.decode_frames_ok = true;
        }
        catch (std::exception const& e)
        {
            o.decode_err = e.what();
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
