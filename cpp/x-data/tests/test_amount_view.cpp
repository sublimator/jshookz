#include "catl/xdata/amount-view.h"
#include "catl/xdata/certified-index.h"
#include "catl/xdata/protocol.h"
#include "catl/xdata/scan.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <new>
#include <stdlib.h>

using namespace catl::xdata;

namespace {

// Account + Amount 1000000 native.
constexpr uint8_t kNativeAmtObj[] = {
    0x61, 0x40, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x42, 0x40, 0x81, 0x14,
    0xb5, 0xf7, 0x62, 0x79, 0x8a, 0x53, 0xd5, 0x43, 0xa0, 0x14, 0xca,
    0xf8, 0xb2, 0x97, 0xcf, 0xf8, 0xf2, 0xf9, 0x37, 0xe8};

constexpr uint8_t kNativeAmt[] = {
    0x40, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x42, 0x40};

std::atomic<int> g_heap{0};
bool g_track = false;

}  // namespace

void*
operator new(std::size_t n)
{
    void* p = std::malloc(n ? n : 1);
    if (!p)
        throw std::bad_alloc();
    if (g_track)
        g_heap.fetch_add(1, std::memory_order_relaxed);
    return p;
}

void*
operator new(std::size_t n, std::align_val_t a)
{
    void* p = nullptr;
    if (posix_memalign(
            &p,
            static_cast<std::size_t>(a),
            n ? n : static_cast<std::size_t>(a)) != 0)
        throw std::bad_alloc();
    if (g_track)
        g_heap.fetch_add(1, std::memory_order_relaxed);
    return p;
}

void*
operator new[](std::size_t n)
{
    return ::operator new(n);
}

void*
operator new[](std::size_t n, std::align_val_t a)
{
    return ::operator new(n, a);
}

void
operator delete(void* p) noexcept
{
    std::free(p);
}

void
operator delete(void* p, std::size_t) noexcept
{
    std::free(p);
}

void
operator delete(void* p, std::align_val_t) noexcept
{
    std::free(p);
}

void
operator delete(void* p, std::size_t, std::align_val_t) noexcept
{
    std::free(p);
}

void
operator delete[](void* p) noexcept
{
    std::free(p);
}

void
operator delete[](void* p, std::size_t) noexcept
{
    std::free(p);
}

void
operator delete[](void* p, std::align_val_t) noexcept
{
    std::free(p);
}

TEST(AmountView, MalformedCannotMintIndex)
{
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    std::uint8_t const bad[] = {0xff};
    auto idx = certify_indexed(Slice{bad, sizeof(bad)}, 0, protocol);
    EXPECT_FALSE(idx.has_value());
}

TEST(AmountView, LocateOnlyDoesNotProduceIndex)
{
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    Slice backing{kNativeAmtObj, sizeof(kNativeAmtObj)};
    NullSink sink;
    auto loc = scan_scope<ScanMode::Locate>(backing, 0, protocol, sink);
    ASSERT_TRUE(loc.has_value());
    auto idx = certify_indexed(backing, 0, protocol);
    ASSERT_TRUE(idx.has_value());
    EXPECT_NE(static_cast<void const*>(&sink), static_cast<void const*>(&*idx));
}

TEST(AmountView, BindWrongOrdinalAndWrongTypeFail)
{
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    auto idx =
        certify_indexed(Slice{kNativeAmtObj, sizeof(kNativeAmtObj)}, 0, protocol);
    ASSERT_TRUE(idx.has_value());
    EXPECT_FALSE(AmountView::bind(*idx, idx->frame_count()));
    EXPECT_FALSE(AmountView::bind(*idx, 99));
    size_t account_ord = idx->frame_count();
    for (size_t i = 0; i < idx->frame_count(); ++i)
    {
        auto const* f = protocol.get_field_by_code(idx->frame(i).field_code);
        if (f && f->name == "Account")
        {
            account_ord = i;
            break;
        }
    }
    ASSERT_LT(account_ord, idx->frame_count());
    EXPECT_FALSE(AmountView::bind(*idx, account_ord));
}

TEST(AmountView, BindAmountPartsAreTotal)
{
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    auto idx =
        certify_indexed(Slice{kNativeAmtObj, sizeof(kNativeAmtObj)}, 0, protocol);
    ASSERT_TRUE(idx.has_value());
    std::optional<AmountView> view;
    for (size_t i = 0; i < idx->frame_count(); ++i)
    {
        view = AmountView::bind(*idx, i);
        if (view)
            break;
    }
    ASSERT_TRUE(view.has_value());
    auto p = view->parts();
    EXPECT_EQ(p.kind, AmountRules::Kind::Native);
    EXPECT_FALSE(p.negative);
    EXPECT_EQ(p.magnitude, 1000000u);
}

TEST(AmountView, StandaloneSpanBind)
{
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    auto idx =
        certify_amount_span(Slice{kNativeAmt, sizeof(kNativeAmt)}, protocol);
    ASSERT_TRUE(idx.has_value()) << idx.error().message;
    auto view = AmountView::bind(*idx, 0);
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->parts().magnitude, 1000000u);
}

TEST(AmountView, BindAndPartsDoNotHeapAllocate)
{
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    auto idx =
        certify_indexed(Slice{kNativeAmtObj, sizeof(kNativeAmtObj)}, 0, protocol);
    ASSERT_TRUE(idx.has_value());
    size_t amt_ord = 0;
    for (size_t i = 0; i < idx->frame_count(); ++i)
    {
        auto const* f = protocol.get_field_by_code(idx->frame(i).field_code);
        if (f && f->meta.type == FieldTypes::Amount)
        {
            amt_ord = i;
            break;
        }
    }
    g_heap.store(0);
    g_track = true;
    auto view = AmountView::bind(*idx, amt_ord);
    ASSERT_TRUE(view.has_value());
    auto p = view->parts();
    auto k = view->kind();
    g_track = false;
    EXPECT_EQ(p.magnitude, 1000000u);
    EXPECT_EQ(k, AmountRules::Kind::Native);
    EXPECT_EQ(g_heap.load(), 0);
}

TEST(AmountView, NegativeZeroNativeRejected)
{
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    std::uint8_t const z[] = {0, 0, 0, 0, 0, 0, 0, 0};
    auto idx = certify_amount_span(Slice{z, sizeof(z)}, protocol);
    EXPECT_FALSE(idx.has_value());
    EXPECT_NE(std::string(idx.error().message).find("negative zero"), std::string::npos);
}
