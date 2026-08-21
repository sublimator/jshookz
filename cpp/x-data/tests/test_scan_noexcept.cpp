// Compile this TU with -fno-exceptions. scan_scope and AmountView must not throw.
#include "catl/xdata/amount-view.h"
#include "catl/xdata/certified-index.h"
#include "catl/xdata/protocol.h"
#include "catl/xdata/scan.h"

#include <optional>

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
    if (r || c)
        return 2;

    std::uint8_t const amt[] = {
        0x61, 0x40, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x42, 0x40, 0x81, 0x14,
        0xb5, 0xf7, 0x62, 0x79, 0x8a, 0x53, 0xd5, 0x43, 0xa0, 0x14, 0xca,
        0xf8, 0xb2, 0x97, 0xcf, 0xf8, 0xf2, 0xf9, 0x37, 0xe8};
    auto idx = certify_indexed(Slice{amt, sizeof(amt)}, 0, protocol);
    if (!idx)
        return 3;
    std::optional<AmountView> view;
    for (size_t i = 0; i < idx->frame_count(); ++i)
    {
        view = AmountView::bind(*idx, i);
        if (view)
            break;
    }
    if (!view)
        return 4;
    auto p = view->parts();
    if (p.kind != AmountRules::Kind::Native || p.magnitude != 1000000u)
        return 5;
    return 0;
}
