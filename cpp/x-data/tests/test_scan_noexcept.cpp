// Compile this TU with -fno-exceptions. scan_scope and AmountView must not
// throw.
#include "catl/xdata/account-id-view.h"
#include "catl/xdata/amount-view.h"
#include "catl/xdata/certified-index.h"
#include "catl/xdata/protocol.h"
#include "catl/xdata/pathset-view.h"
#include "catl/xdata/scan.h"
#include "catl/xdata/uint32-view.h"

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

    std::uint8_t const amt[] = {0x61, 0x40, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x42,
                                0x40, 0x81, 0x14, 0xb5, 0xf7, 0x62, 0x79, 0x8a,
                                0x53, 0xd5, 0x43, 0xa0, 0x14, 0xca, 0xf8, 0xb2,
                                0x97, 0xcf, 0xf8, 0xf2, 0xf9, 0x37, 0xe8};
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
    auto root =
        CertifiedRoot::copy_and_certify(Slice{amt, sizeof(amt)}, 0, protocol);
    if (!root)
        return 6;
    CertifiedObject object{std::move(*root)};
    bool found = false;
    for (size_t i = 0; i < object.frame_count(); ++i)
    {
        auto value = object.materialize_amount(i);
        if (value && value->magnitude == 1000000u)
        {
            found = true;
            break;
        }
    }
    if (!found)
        return 7;

    std::uint8_t const sequence[] = {0x24, 0x12, 0x34, 0x56, 0x78};
    auto seq_idx =
        certify_indexed(Slice{sequence, sizeof(sequence)}, 0, protocol);
    if (!seq_idx)
        return 8;
    auto seq_view = UInt32View::bind(*seq_idx, 0);
    if (!seq_view || seq_view->value() != 0x12345678u)
        return 9;
    auto seq_root = CertifiedRoot::copy_and_certify(
        Slice{sequence, sizeof(sequence)}, 0, protocol);
    if (!seq_root)
        return 10;
    auto seq_value =
        CertifiedObject{std::move(*seq_root)}.uint32_value(0);
    if (!seq_value || *seq_value != 0x12345678u)
        return 11;

    std::uint8_t const account[] = {
        0x81, 0x14, 0xb5, 0xf7, 0x62, 0x79, 0x8a, 0x53, 0xd5, 0x43, 0xa0,
        0x14, 0xca, 0xf8, 0xb2, 0x97, 0xcf, 0xf8, 0xf2, 0xf9, 0x37, 0xe8};
    auto account_idx =
        certify_indexed(Slice{account, sizeof(account)}, 0, protocol);
    if (!account_idx)
        return 12;
    auto account_view = AccountIDView::bind(*account_idx, 0);
    if (!account_view || account_view->is_default() ||
        account_view->bytes().size() != 20)
        return 13;
    auto account_root = CertifiedRoot::copy_and_certify(
        Slice{account, sizeof(account)}, 0, protocol);
    if (!account_root)
        return 14;
    auto account_value = CertifiedObject{std::move(*account_root)}
                             .materialize_account_id(0);
    if (!account_value || (*account_value)[0] != 0xb5 ||
        (*account_value)[19] != 0xe8)
        return 15;

    std::uint8_t const paths[] = {
        0x01, 0x12, 0x01, 0xb5, 0xf7, 0x62, 0x79, 0x8a, 0x53, 0xd5,
        0x43, 0xa0, 0x14, 0xca, 0xf8, 0xb2, 0x97, 0xcf, 0xf8, 0xf2,
        0xf9, 0x37, 0xe8, 0x00};
    auto paths_idx =
        certify_indexed(Slice{paths, sizeof(paths)}, 0, protocol);
    if (!paths_idx)
        return 16;
    auto paths_view = PathSetView::bind(*paths_idx, 0);
    if (!paths_view)
        return 17;
    struct CountPathSet
    {
        int hops = 0;
        void on_hop(PathSetHop const&) noexcept { ++hops; }
        void on_path_end() const noexcept {}
        void on_end() const noexcept {}
    } paths_sink;
    if (!paths_view->traverse(paths_sink) || paths_sink.hops != 1)
        return 18;
    return 0;
}
