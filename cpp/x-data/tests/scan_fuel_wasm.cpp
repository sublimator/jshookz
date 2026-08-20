// No-exceptions wasm fuel probe. JsonVisitor cannot link under wasi-sdk
// (libc++abi has no __cxa_throw). This uses the shipped parse_with_visitor
// path and materializes every leaf with shipped decode_raw / AccountID
// decode_string_expected / Amount certify — not a skip visitor.

#include "catl/xdata/codecs/account_id.h"
#include "catl/xdata/codecs/int.h"
#include "catl/xdata/codecs/uint.h"
#include "catl/xdata/parser-context.h"
#include "catl/xdata/parser.h"
#include "catl/xdata/protocol.h"
#include "catl/xdata/scan.h"
#include "catl/xdata/slice-visitor.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct MaterializeVisitor
{
    uint64_t acc = 0;
    std::vector<std::string> owned;

    bool
    visit_object_start(catl::xdata::FieldPath const&, catl::xdata::FieldSlice const&)
    {
        return true;
    }
    void
    visit_object_end(catl::xdata::FieldPath const&, catl::xdata::FieldSlice const&)
    {
    }
    bool
    visit_array_start(catl::xdata::FieldPath const&, catl::xdata::FieldSlice const&)
    {
        return true;
    }
    void
    visit_array_end(catl::xdata::FieldPath const&, catl::xdata::FieldSlice const&)
    {
    }
    void
    visit_field(catl::xdata::FieldPath const&, catl::xdata::FieldSlice const& fs)
    {
        using namespace catl::xdata;
        auto const& t = fs.get_field().meta.type;
        Slice const d = fs.data;
        if (t == FieldTypes::UInt8)
            acc += codecs::UInt8Codec::decode_raw(d);
        else if (t == FieldTypes::UInt16)
            acc += codecs::UInt16Codec::decode_raw(d);
        else if (t == FieldTypes::UInt32)
            acc += codecs::UInt32Codec::decode_raw(d);
        else if (t == FieldTypes::Int32)
            acc += static_cast<uint64_t>(codecs::Int32Codec::decode_raw(d));
        else if (t == FieldTypes::AccountID)
        {
            auto s = codecs::AccountIDCodec::decode_string_expected(d);
            if (s)
            {
                owned.push_back(std::move(*s));
                acc += owned.back().size();
            }
        }
        else if (t == FieldTypes::Amount)
        {
            (void)scan_detail::certify_amount(d);
            if (d.size() >= 8)
            {
                uint64_t v = 0;
                for (int i = 0; i < 8; ++i)
                    v = (v << 8) | d.data()[i];
                acc += v;
            }
            owned.emplace_back(
                reinterpret_cast<char const*>(d.data()), d.size());
        }
        else
        {
            owned.emplace_back(
                reinterpret_cast<char const*>(d.data()), d.size());
            for (size_t i = 0; i < d.size(); ++i)
                acc += d.data()[i];
        }
    }
};

constexpr uint8_t kBlob[] = {
    0x81, 0x14, 0xB5, 0xF7, 0x62, 0x79, 0x8A, 0x53, 0xD5, 0x43, 0xA0, 0x14,
    0xCA, 0xF8, 0xB2, 0x97, 0xCF, 0xF8, 0xF2, 0xF9, 0x37, 0xE8, 0xF9, 0xEA,
    0x7D, 0x02, 0xDE, 0xAD, 0xE1, 0xF1};

}  // namespace

int
main(int argc, char** argv)
{
    using namespace catl::xdata;
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    Slice backing{kBlob, sizeof(kBlob)};
    constexpr int kIters = 2000;
    char const* which = argc > 1 ? argv[1] : "all";
    uint64_t sink = 0;

    auto loc = [&] {
        for (int i = 0; i < kIters; ++i)
        {
            NullSink s;
            auto r = scan_scope<ScanMode::Locate>(backing, 0, protocol, s);
            if (r)
                sink += *r;
        }
        std::puts("locate_no_index");
    };
    auto cert = [&] {
        for (int i = 0; i < kIters; ++i)
        {
            NullSink s;
            auto r = scan_scope<ScanMode::CertifyWire>(backing, 0, protocol, s);
            if (r)
                sink += *r;
        }
        std::puts("certify_no_index");
    };
    auto idx = [&] {
        for (int i = 0; i < kIters; ++i)
        {
            IndexSink s;
            auto r = scan_scope<ScanMode::CertifyWire>(backing, 0, protocol, s);
            if (r)
                sink += *r + s.frames.size();
        }
        std::puts("certify_index");
    };
    auto eager = [&] {
        for (int i = 0; i < kIters; ++i)
        {
            ParserContext ctx{backing};
            MaterializeVisitor visitor;
            parse_with_visitor(ctx, protocol, visitor);
            sink += visitor.acc + visitor.owned.size();
        }
        std::puts("full_eager_decode");
    };

    if (which[0] == '0' || std::strcmp(which, "locate_no_index") == 0)
        loc();
    else if (which[0] == '1' || std::strcmp(which, "certify_no_index") == 0)
        cert();
    else if (which[0] == '2' || std::strcmp(which, "certify_index") == 0)
        idx();
    else if (which[0] == '3' || std::strcmp(which, "full_eager_decode") == 0)
        eager();
    else
    {
        loc();
        cert();
        idx();
        eager();
    }
    if (sink == 0xffffffffffffull)
        std::puts("never");
    return 0;
}
