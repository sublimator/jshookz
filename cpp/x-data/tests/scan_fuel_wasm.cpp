// Standalone wasm fuel probe. Not part of the host test binary.
// Compile with wasi-sdk; run with wasmtime --fuel or python wasmtime.

#include "catl/xdata/parser-context.h"
#include "catl/xdata/parser.h"
#include "catl/xdata/protocol.h"
#include "catl/xdata/scan.h"
#include "catl/xdata/slice-visitor.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

struct CountVisitor
{
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
    visit_field(catl::xdata::FieldPath const&, catl::xdata::FieldSlice const&)
    {
    }
};

// stobject-nested-memos blob
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

    auto loc = [&] {
        for (int i = 0; i < kIters; ++i)
        {
            NullSink s;
            (void)scan_scope<ScanMode::Locate>(backing, 0, protocol, s);
        }
        std::puts("locate_no_index");
    };
    auto cert = [&] {
        for (int i = 0; i < kIters; ++i)
        {
            NullSink s;
            (void)scan_scope<ScanMode::CertifyWire>(backing, 0, protocol, s);
        }
        std::puts("certify_no_index");
    };
    auto idx = [&] {
        for (int i = 0; i < kIters; ++i)
        {
            IndexSink s;
            (void)scan_scope<ScanMode::CertifyWire>(backing, 0, protocol, s);
        }
        std::puts("certify_index");
    };
    auto eager = [&] {
        for (int i = 0; i < kIters; ++i)
        {
            ParserContext ctx{backing};
            CountVisitor visitor;
            parse_with_visitor(ctx, protocol, visitor);
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
    return 0;
}
