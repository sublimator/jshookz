// Compile this TU with -fno-exceptions. scan_scope must not throw.
#include "catl/xdata/protocol.h"
#include "catl/xdata/scan.h"

int
main()
{
    using namespace catl::xdata;
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    std::uint8_t bad[] = {0xff};
    Slice backing{bad, sizeof(bad)};
    NullSink sink;
    auto r = scan_scope<ScanMode::Locate>(backing, 0, protocol, sink);
    auto c = scan_scope<ScanMode::CertifyWire>(backing, 0, protocol, sink);
    return (r || c) ? 2 : 0;
}
