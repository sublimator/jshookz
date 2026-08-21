#include "catl/xdata/amount-view.h"
#include "catl/xdata/certified-index.h"
#include "catl/xdata/protocol.h"
#include "catl/xdata/scan.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <new>
#include <sstream>
#include <stdlib.h>
#include <string>

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
std::atomic<std::size_t> g_heap_bytes{0};
bool g_track = false;

}  // namespace

void*
operator new(std::size_t n)
{
    void* p = std::malloc(n ? n : 1);
    if (!p)
        throw std::bad_alloc();
    if (g_track)
    {
        g_heap.fetch_add(1, std::memory_order_relaxed);
        g_heap_bytes.fetch_add(n, std::memory_order_relaxed);
    }
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
    {
        g_heap.fetch_add(1, std::memory_order_relaxed);
        g_heap_bytes.fetch_add(n, std::memory_order_relaxed);
    }
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

TEST(AmountView, KindDoesNotRequireParts)
{
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    auto idx =
        certify_amount_span(Slice{kNativeAmt, sizeof(kNativeAmt)}, protocol);
    ASSERT_TRUE(idx.has_value());
    auto view = AmountView::bind(*idx, 0);
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->kind(), AmountRules::Kind::Native);
    EXPECT_EQ(view->kind(), view->parts().kind);
    EXPECT_EQ(AmountRules::kind(Slice{kNativeAmt, sizeof(kNativeAmt)}),
        AmountRules::Kind::Native);
#ifndef JSHOOKZ_AMOUNT_VIEW_H
    GTEST_SKIP();
#else
    std::ifstream in(JSHOOKZ_AMOUNT_VIEW_H);
    ASSERT_TRUE(in) << JSHOOKZ_AMOUNT_VIEW_H;
    std::ostringstream oss;
    oss << in.rdbuf();
    auto text = oss.str();
    auto kind = text.find("kind() const noexcept");
    ASSERT_NE(kind, std::string::npos);
    auto body = text.find("AmountRules::kind(payload_)", kind);
    auto parts = text.find("parts()", kind);
    ASSERT_NE(body, std::string::npos);
    EXPECT_TRUE(parts == std::string::npos || parts > body + 40)
        << "kind() must not call parts()";
#endif
}

TEST(AmountView, BindSurvivesProtocolTemporary)
{
    auto make_root = [] {
        auto protocol = Protocol::load_embedded_xahau_protocol();
        return CertifiedRoot::copy_and_certify_amount(
            Slice{kNativeAmt, sizeof(kNativeAmt)}, protocol);
    };
    auto root = make_root();
    ASSERT_TRUE(root.has_value());
    auto view = AmountView::bind(*root, 0);
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->parts().magnitude, 1000000u);
    EXPECT_EQ(view->kind(), AmountRules::Kind::Native);
}

TEST(AmountView, AnchoredAmountOutlivesLocals)
{
    auto make = []() -> std::optional<AnchoredAmount> {
        auto protocol = Protocol::load_embedded_xahau_protocol();
        auto root = CertifiedRoot::copy_and_certify(
            Slice{kNativeAmtObj, sizeof(kNativeAmtObj)}, 0, protocol);
        if (!root)
            return std::nullopt;
        for (size_t i = 0; i < root->frame_count(); ++i)
        {
            auto a = AnchoredAmount::bind(std::move(*root), i);
            if (a)
                return a;
        }
        return std::nullopt;
    };
    auto a = make();
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->parts().magnitude, 1000000u);
    EXPECT_EQ(a->kind(), AmountRules::Kind::Native);
    auto payload = a->payload();
    EXPECT_FALSE(payload.empty());
}

TEST(AmountView, RootOwnsBytesAndBindBorrows)
{
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    auto root = CertifiedRoot::copy_and_certify(
        Slice{kNativeAmtObj, sizeof(kNativeAmtObj)}, 0, protocol);
    ASSERT_TRUE(root.has_value()) << root.error().message;
    EXPECT_EQ(root->backing().size(), sizeof(kNativeAmtObj));
    EXPECT_NE(root->backing().data(), kNativeAmtObj);
    std::optional<AmountView> view;
    for (size_t i = 0; i < root->frame_count(); ++i)
    {
        view = AmountView::bind(*root, i);
        if (view)
            break;
    }
    ASSERT_TRUE(view.has_value());
    auto payload = view->payload();
    EXPECT_GE(payload.data(), root->backing().data());
    EXPECT_LE(
        payload.data() + payload.size(),
        root->backing().data() + root->backing().size());
    EXPECT_EQ(view->parts().magnitude, 1000000u);
    EXPECT_EQ(view->kind(), AmountRules::Kind::Native);
}

TEST(AmountView, RootBelowMinMantissaRejected)
{
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    uint8_t iou[48]{};
    uint64_t const word = AmountRules::kIssued | AmountRules::kPositive |
        (97ull << 54) | 1ull;
    for (int i = 0; i < 8; ++i)
        iou[i] = static_cast<uint8_t>(word >> (56 - 8 * i));
    iou[8 + 12] = 'U';
    iou[8 + 13] = 'S';
    iou[8 + 14] = 'D';
    iou[28] = 0xb5;
    auto root = CertifiedRoot::copy_and_certify_amount(
        Slice{iou, sizeof(iou)}, protocol);
    EXPECT_FALSE(root.has_value());
}

TEST(AmountView, RepresentationSizes)
{
    EXPECT_EQ(sizeof(Slice), 2 * sizeof(void*));
    EXPECT_EQ(sizeof(AmountView), sizeof(Slice));
    EXPECT_EQ(sizeof(FieldFrame), 16u);
#if defined(__wasm32__)
    EXPECT_EQ(sizeof(AmountView), 8u);
    EXPECT_EQ(sizeof(std::optional<AmountView>), 12u);
    EXPECT_EQ(sizeof(CertifiedIndex), 32u);
    EXPECT_EQ(sizeof(AmountRules::Parts), 48u);
#elif defined(__aarch64__) || defined(__x86_64__)
    EXPECT_EQ(sizeof(AmountView), 16u);
    EXPECT_EQ(sizeof(std::optional<AmountView>), 24u);
    EXPECT_EQ(sizeof(CertifiedIndex), 48u);
    EXPECT_EQ(sizeof(AmountRules::Parts), 72u);
#endif
}

TEST(AmountView, CertifyIndexAllocatesFramesBindDoesNot)
{
    auto const protocol = Protocol::load_embedded_xahau_protocol();

    constexpr uint8_t kNestedMemos[] = {
        0x81, 0x14, 0xB5, 0xF7, 0x62, 0x79, 0x8A, 0x53, 0xD5, 0x43, 0xA0,
        0x14, 0xCA, 0xF8, 0xB2, 0x97, 0xCF, 0xF8, 0xF2, 0xF9, 0x37, 0xE8,
        0xF9, 0xEA, 0x7D, 0x02, 0xDE, 0xAD, 0xE1, 0xF1};
    constexpr uint8_t kNop63[] = {
        0x81, 0x14, 0xB5, 0xF7, 0x62, 0x79, 0x8A, 0x53, 0xD5, 0x43, 0xA0,
        0x14, 0xCA, 0xF8, 0xB2, 0x97, 0xCF, 0xF8, 0xF2, 0xF9, 0x37, 0xE8};

    auto measure = [&](uint8_t const* p, size_t n) {
        g_heap.store(0);
        g_heap_bytes.store(0);
        g_track = true;
        auto idx = certify_indexed(Slice{p, n}, 0, protocol);
        g_track = false;
        return idx;
    };

    g_heap.store(0);
    g_heap_bytes.store(0);
    g_track = true;
    auto span2 =
        certify_amount_span(Slice{kNativeAmt, sizeof(kNativeAmt)}, protocol);
    g_track = false;
    ASSERT_TRUE(span2.has_value());
    std::printf(
        "certify_amount_span frames=%zu allocs=%d bytes=%zu\n",
        span2->frame_count(),
        g_heap.load(),
        g_heap_bytes.load());
    EXPECT_EQ(span2->frame_count(), 1u);
    EXPECT_EQ(g_heap.load(), 1);
    EXPECT_EQ(g_heap_bytes.load(), 16u);

    auto obj = measure(kNativeAmtObj, sizeof(kNativeAmtObj));
    ASSERT_TRUE(obj.has_value());
    std::printf(
        "certify_indexed native-amt frames=%zu allocs=%d bytes=%zu\n",
        obj->frame_count(),
        g_heap.load(),
        g_heap_bytes.load());
    EXPECT_EQ(obj->frame_count(), 2u);
    EXPECT_EQ(g_heap.load(), 1);
    EXPECT_EQ(g_heap_bytes.load(), 128u);

    auto memos = measure(kNestedMemos, sizeof(kNestedMemos));
    ASSERT_TRUE(memos.has_value()) << memos.error().message;
    std::printf(
        "certify_indexed nested-memos frames=%zu allocs=%d bytes=%zu\n",
        memos->frame_count(),
        g_heap.load(),
        g_heap_bytes.load());
    EXPECT_GE(memos->frame_count(), 4u);
    EXPECT_EQ(g_heap.load(), 1);
    EXPECT_EQ(g_heap_bytes.load(), 128u);

    auto nop = measure(kNop63, sizeof(kNop63));
    ASSERT_TRUE(nop.has_value());
    std::printf(
        "certify_indexed nop-63 frames=%zu allocs=%d bytes=%zu\n",
        nop->frame_count(),
        g_heap.load(),
        g_heap_bytes.load());
    EXPECT_EQ(nop->frame_count(), 1u);
    EXPECT_EQ(g_heap.load(), 1);
    EXPECT_EQ(g_heap_bytes.load(), 128u);

    size_t amt_ord = 0;
    for (size_t i = 0; i < obj->frame_count(); ++i)
    {
        auto const* f = protocol.get_field_by_code(obj->frame(i).field_code);
        if (f && f->meta.type == FieldTypes::Amount)
        {
            amt_ord = i;
            break;
        }
    }
    g_heap.store(0);
    g_heap_bytes.store(0);
    g_track = true;
    auto view = AmountView::bind(*obj, amt_ord);
    ASSERT_TRUE(view.has_value());
    auto p = view->parts();
    auto k = view->kind();
    (void)p.currency.size();
    (void)p.issuer.size();
    g_track = false;
    EXPECT_EQ(k, AmountRules::Kind::Native);
    EXPECT_EQ(g_heap.load(), 0);
    EXPECT_EQ(g_heap_bytes.load(), 0u);

    g_heap.store(0);
    g_heap_bytes.store(0);
    g_track = true;
    auto root = CertifiedRoot::copy_and_certify(
        Slice{kNativeAmtObj, sizeof(kNativeAmtObj)}, 0, protocol);
    g_track = false;
    ASSERT_TRUE(root.has_value());
    std::printf(
        "CertifiedRoot copy_and_certify allocs=%d bytes=%zu sizeof=%zu\n",
        g_heap.load(),
        g_heap_bytes.load(),
        sizeof(CertifiedRoot));
    EXPECT_EQ(g_heap.load(), 2);
    EXPECT_EQ(g_heap_bytes.load(), 159u);
}

TEST(AmountView, IndexSinkLazyReserveEight)
{
    FieldFrame f{};
    g_heap.store(0);
    g_heap_bytes.store(0);
    g_track = true;
    IndexSink empty;
    g_track = false;
    EXPECT_EQ(g_heap.load(), 0);

    g_heap.store(0);
    g_heap_bytes.store(0);
    g_track = true;
    IndexSink s;
    s.emit(f);
    g_track = false;
    EXPECT_EQ(s.frames.size(), 1u);
    EXPECT_EQ(g_heap.load(), 1);
    EXPECT_EQ(g_heap_bytes.load(), 128u);

    g_heap.store(0);
    g_heap_bytes.store(0);
    g_track = true;
    for (int i = 0; i < 7; ++i)
        s.emit(f);
    g_track = false;
    EXPECT_EQ(s.frames.size(), 8u);
    EXPECT_EQ(g_heap.load(), 0);

    g_heap.store(0);
    g_heap_bytes.store(0);
    g_track = true;
    s.emit(f);
    g_track = false;
    EXPECT_EQ(s.frames.size(), 9u);
    EXPECT_GE(g_heap.load(), 1);
}
