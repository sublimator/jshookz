#include "oracle_run.hpp"

#include <catl/xdata/json-visitor.h>
#include <catl/xdata/parser.h>
#include <catl/xdata/protocol.h>

#include <boost/json.hpp>

#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#ifndef JSHOOKZ_ORACLE_CORPUS_JSON
#error "JSHOOKZ_ORACLE_CORPUS_JSON is required"
#endif

int
main(int argc, char** argv)
{
    std::ifstream in(JSHOOKZ_ORACLE_CORPUS_JSON);
    std::ostringstream ss;
    ss << in.rdbuf();
    auto const root = boost::json::parse(ss.str()).as_object();
    {
        std::string prov_err;
        if (!oracle_run::corpus_provenance_ok(root, prov_err))
        {
            std::cerr << "FAIL provenance " << prov_err << "\n";
            return 1;
        }
        if (!oracle_run::header_enum_complete(root, prov_err))
        {
            std::cerr << "FAIL header-enum " << prov_err << "\n";
            return 1;
        }
        if (!oracle_run::amount_boundary_complete(root, prov_err))
        {
            std::cerr << "FAIL amount-boundary " << prov_err << "\n";
            return 1;
        }
        if (!oracle_run::uint32_boundary_complete(root, prov_err))
        {
            std::cerr << "FAIL uint32-boundary " << prov_err << "\n";
            return 1;
        }
        if (!oracle_run::account_id_boundary_complete(root, prov_err))
        {
            std::cerr << "FAIL account-id-boundary " << prov_err << "\n";
            return 1;
        }
    }
    auto const protocol = catl::xdata::Protocol::load_embedded_xahau_protocol();

    int fail = 0;
    for (auto const& item : root.at("cases").as_array())
    {
        auto const& c = item.as_object();
        auto const id = std::string(c.at("id").as_string());
        auto const expect = std::string(c.at("expect").as_string());
        auto const type = std::string(c.at("codec_type").as_string());
        auto const blob = std::string(c.at("blob").as_string());
        if (expect == "accept" && type == "stobject" &&
            (!c.contains("fields") || !c.contains("canonical_blob")))
        {
            std::cout << "FAIL " << id << " missing oracle envelope\n";
            ++fail;
            continue;
        }
        oracle_run::Outcomes o;
        if (type == "amount")
            o = oracle_run::run_amount(
                protocol, blob, c.if_contains("fields"), c.if_contains("json"));
        else if (type == "pathset")
            o = oracle_run::run_pathset(blob, c.if_contains("json"));
        else
        {
            auto const* fields = c.if_contains("fields");
            auto const* js = c.if_contains("json");
            std::string canonical;
            if (c.contains("canonical_blob"))
                canonical = std::string(c.at("canonical_blob").as_string());
            o = oracle_run::run_stobject(protocol, blob, fields, js, canonical);
        }

        bool const trailing_ok =
            c.contains("trailing_ok") && c.at("trailing_ok").as_bool();
        bool const header_enum = id.rfind("hdr-", 0) == 0;
        bool const wire_ok =
            c.contains("wire_ok") && c.at("wire_ok").as_bool();
        bool const certify_ok = o.certify_null_ok;
        bool const pass = o.sinks_agree &&
            (wire_ok
                 ? certify_ok
                 : (expect == "accept"
                        ? (o.locate_ok && certify_ok && o.decode_frames_ok &&
                           o.names_ok && o.amount_parts_ok && o.account_id_ok &&
                           (header_enum || o.json_ok) &&
                           (trailing_ok || o.consumed_all))
                        : !certify_ok));
        if (!pass)
            ++fail;
        std::cout << (pass ? "PASS" : "FAIL") << " " << id
                  << " expect=" << expect << " locate=" << o.locate_ok
                  << " certify=" << certify_ok
                  << " sinks_agree=" << o.sinks_agree
                  << " decode=" << o.decode_frames_ok
                  << " names=" << o.names_ok << " json=" << o.json_ok
                  << " end=" << o.locate_end << "/" << o.blob_size;
        if (!certify_ok && !o.certify_err.empty())
            std::cout << " err=" << o.certify_err;
        if (!o.decode_err.empty())
            std::cout << " decode_err=" << o.decode_err;
        if (!o.json_err.empty())
            std::cout << " json_err=" << o.json_err;
        if (!o.names_err.empty())
            std::cout << " names_err=" << o.names_err;
        if (!o.amount_parts_err.empty())
            std::cout << " parts_err=" << o.amount_parts_err;
        if (!o.account_id_err.empty())
            std::cout << " account_id_err=" << o.account_id_err;
        std::cout << "\n";
    }
    std::cout << "fallbacks=" << protocol.fast_lookup_fallback_count()
              << " max_type=" << protocol.max_serialized_type_code()
              << " max_nth=" << protocol.max_serialized_nth()
              << " lookup_bytes=" << protocol.fast_lookup_bytes() << "\n";

    if (argc > 1 && std::string(argv[1]) == "--fuel")
    {
        std::string blob;
        for (auto const& item : root.at("cases").as_array())
        {
            auto const& c = item.as_object();
            if (std::string(c.at("id").as_string()) == "stobject-nested-memos")
            {
                blob = std::string(c.at("blob").as_string());
                break;
            }
        }
        auto bytes = oracle_run::decode_hex(blob);
        Slice backing{bytes.data(), bytes.size()};
        constexpr int kIters = 50000;
        auto bench = [&](char const* name, auto&& fn) {
            auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < kIters; ++i)
                fn();
            auto t1 = std::chrono::steady_clock::now();
            auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                          .count();
            std::cout << "fuel " << name << " ns=" << ns
                      << " per=" << (ns / kIters) << "\n";
        };
        bench("locate_no_index", [&] {
            catl::xdata::NullSink s;
            (void)catl::xdata::scan_scope<catl::xdata::ScanMode::Locate>(
                backing, 0, protocol, s);
        });
        bench("certify_no_index", [&] {
            catl::xdata::NullSink s;
            (void)catl::xdata::scan_scope<catl::xdata::ScanMode::CertifyWire>(
                backing, 0, protocol, s);
        });
        bench("certify_index", [&] {
            catl::xdata::IndexSink s;
            (void)catl::xdata::scan_scope<catl::xdata::ScanMode::CertifyWire>(
                backing, 0, protocol, s);
        });
        bench("full_eager_decode", [&] {
            catl::xdata::ParserContext ctx{backing};
            catl::xdata::JsonVisitor visitor(protocol);
            catl::xdata::parse_with_visitor(ctx, protocol, visitor);
        });
    }
    return fail == 0 ? 0 : 1;
}
