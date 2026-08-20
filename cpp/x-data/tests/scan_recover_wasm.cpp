// Same-instance recovery: malformed scan then valid scan without
// reloading the module. Compile with wasi-sdk; run with wasmtime.

#include "catl/xdata/protocol.h"
#include "catl/xdata/scan.h"

#include <cstdint>
#include <cstdio>

namespace {

constexpr uint8_t kBad[] = {0xff};
constexpr uint8_t kGood[] = {0x81, 0x00};

}  // namespace

int
main()
{
    using namespace catl::xdata;
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    NullSink sink;
    ParserContext ctx{Slice{kBad, sizeof(kBad)}};
    scan_detail::scan_object<ScanMode::CertifyWire>(
        ctx, protocol, sink, 0, true);
    if (!ctx.failed())
    {
        std::puts("FAIL malformed did not fail");
        return 2;
    }
    std::puts("malformed_failed");
    ctx.reset(Slice{kGood, sizeof(kGood)});
    scan_detail::scan_object<ScanMode::CertifyWire>(
        ctx, protocol, sink, 0, true);
    if (ctx.failed())
    {
        std::puts("FAIL valid after malformed");
        return 3;
    }
    std::puts("valid_after_malformed");
    return 0;
}
