#include "catl/xdata/amount-view.h"
#include "catl/xdata/certified-index.h"
#include "catl/xdata/fields.h"
#include "catl/xdata/protocol.h"
#include "catl/xdata/scan.h"
#include "catl/xdata/serializer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <span>
#include <stdlib.h>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace catl::xdata;

namespace catl::xdata {

std::atomic<int> g_amount_parts_calls{0};

void
amount_rules_parts_test_hook() noexcept
{
    g_amount_parts_calls.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace catl::xdata

namespace {

// Account + Amount 1000000 native.
constexpr uint8_t kNativeAmtObj[] = {
    0x61, 0x40, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x42, 0x40, 0x81, 0x14,
    0xb5, 0xf7, 0x62, 0x79, 0x8a, 0x53, 0xd5, 0x43, 0xa0, 0x14, 0xca,
    0xf8, 0xb2, 0x97, 0xcf, 0xf8, 0xf2, 0xf9, 0x37, 0xe8};

constexpr uint8_t kNativeAmt[] =
    {0x40, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x42, 0x40};

std::atomic<int> g_heap{0};
std::atomic<std::size_t> g_heap_bytes{0};
bool g_track = false;

struct BindRoot
{
    template <class T>
    auto
    operator()(T&& t) const
        -> decltype(AmountView::bind(std::forward<T>(t), size_t{0}));
};
struct ObjectMaterialize
{
    template <class T>
    auto
    operator()(T&& t) const
        -> decltype(std::forward<T>(t).materialize_amount(0));
};
struct RootIndex
{
    template <class T>
    auto
    operator()(T&& t) const -> decltype(std::forward<T>(t).index());
};
struct RootBacking
{
    template <class T>
    auto
    operator()(T&& t) const -> decltype(std::forward<T>(t).backing());
};

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
    auto idx = certify_indexed(
        Slice{kNativeAmtObj, sizeof(kNativeAmtObj)}, 0, protocol);
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
    auto idx = certify_indexed(
        Slice{kNativeAmtObj, sizeof(kNativeAmtObj)}, 0, protocol);
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
    auto idx = certify_indexed(
        Slice{kNativeAmtObj, sizeof(kNativeAmtObj)}, 0, protocol);
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
    EXPECT_NE(
        std::string(idx.error().message).find("negative zero"),
        std::string::npos);
}

TEST(AmountView, KindDoesNotRequireParts)
{
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    auto idx =
        certify_amount_span(Slice{kNativeAmt, sizeof(kNativeAmt)}, protocol);
    ASSERT_TRUE(idx.has_value());
    auto view = AmountView::bind(*idx, 0);
    ASSERT_TRUE(view.has_value());
    catl::xdata::g_amount_parts_calls.store(0);
    EXPECT_EQ(view->kind(), AmountRules::Kind::Native);
    EXPECT_EQ(catl::xdata::g_amount_parts_calls.load(), 0);
    EXPECT_EQ(view->kind(), view->parts().kind);
    EXPECT_EQ(catl::xdata::g_amount_parts_calls.load(), 1);
    EXPECT_EQ(
        AmountRules::kind(Slice{kNativeAmt, sizeof(kNativeAmt)}),
        AmountRules::Kind::Native);
}

TEST(AmountView, OwnedBoundaryDoesNotExposeBorrow)
{
    static_assert(std::is_invocable_v<BindRoot, CertifiedIndex const&>);
    static_assert(!std::is_invocable_v<BindRoot, CertifiedRoot const&>);
    static_assert(!std::is_invocable_v<BindRoot, CertifiedRoot&&>);
    static_assert(!std::is_invocable_v<BindRoot, CertifiedRoot const&&>);
    static_assert(!std::is_invocable_v<RootIndex, CertifiedRoot const&>);
    static_assert(!std::is_invocable_v<RootIndex, CertifiedRoot&&>);
    static_assert(!std::is_invocable_v<RootIndex, CertifiedRoot const&&>);
    static_assert(!std::is_invocable_v<RootBacking, CertifiedRoot const&>);
    static_assert(!std::is_invocable_v<RootBacking, CertifiedRoot&&>);
    static_assert(
        std::is_invocable_v<ObjectMaterialize, CertifiedObject const&>);
    static_assert(std::is_invocable_v<ObjectMaterialize, CertifiedObject&&>);
    static_assert(!std::is_reference_v<OwnedAmountParts>);
    static_assert(std::is_trivially_copyable_v<OwnedAmountParts>);
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
    CertifiedObject object{std::move(*root)};
    auto amount = object.materialize_amount(0);
    ASSERT_TRUE(amount.has_value());
    EXPECT_EQ(amount->magnitude, 1000000u);
    EXPECT_EQ(amount->kind, AmountRules::Kind::Native);
}

TEST(AmountView, RootOwnsBytesAndPublicOutputIsOwned)
{
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    std::array<uint8_t, sizeof(kNativeAmtObj)> source{};
    std::copy(
        std::begin(kNativeAmtObj), std::end(kNativeAmtObj), source.begin());
    auto root = CertifiedRoot::copy_and_certify(
        Slice{source.data(), source.size()}, 0, protocol);
    ASSERT_TRUE(root.has_value()) << root.error().message;
    CertifiedObject object{std::move(*root)};
    source.fill(0xff);
    std::optional<OwnedAmountParts> amount;
    for (size_t i = 0; i < object.frame_count(); ++i)
    {
        amount = object.materialize_amount(i);
        if (amount)
            break;
    }
    ASSERT_TRUE(amount.has_value());
    EXPECT_EQ(amount->magnitude, 1000000u);
    EXPECT_EQ(amount->kind, AmountRules::Kind::Native);
}

TEST(AmountView, RootBelowMinMantissaRejected)
{
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    uint8_t iou[48]{};
    uint64_t const word =
        AmountRules::kIssued | AmountRules::kPositive | (97ull << 54) | 1ull;
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
    EXPECT_EQ(sizeof(CertifiedIndex), 28u);
    EXPECT_EQ(sizeof(CertifiedRoot), 40u);
    EXPECT_EQ(sizeof(CertifiedObject), 40u);
    EXPECT_EQ(sizeof(AmountRules::Parts), 48u);
    EXPECT_EQ(sizeof(OwnedAmountParts), 64u);
    EXPECT_EQ(sizeof(std::optional<OwnedAmountParts>), 72u);
    EXPECT_EQ(sizeof(IndexSink), 144u);
#elif defined(__aarch64__) || defined(__x86_64__)
    EXPECT_EQ(sizeof(AmountView), 16u);
    EXPECT_EQ(sizeof(std::optional<AmountView>), 24u);
    EXPECT_EQ(sizeof(CertifiedIndex), 48u);
    EXPECT_EQ(sizeof(CertifiedRoot), 72u);
    EXPECT_EQ(sizeof(CertifiedObject), 72u);
    EXPECT_EQ(sizeof(AmountRules::Parts), 72u);
    EXPECT_EQ(sizeof(OwnedAmountParts), 64u);
    EXPECT_EQ(sizeof(std::optional<OwnedAmountParts>), 72u);
    EXPECT_EQ(sizeof(IndexSink), 160u);
#endif
}

TEST(AmountView, CertifyIndexAllocatesFramesBindDoesNot)
{
    auto const protocol = Protocol::load_embedded_xahau_protocol();

    constexpr uint8_t kNestedMemos[] = {
        0x81, 0x14, 0xB5, 0xF7, 0x62, 0x79, 0x8A, 0x53, 0xD5, 0x43,
        0xA0, 0x14, 0xCA, 0xF8, 0xB2, 0x97, 0xCF, 0xF8, 0xF2, 0xF9,
        0x37, 0xE8, 0xF9, 0xEA, 0x7D, 0x02, 0xDE, 0xAD, 0xE1, 0xF1};
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
    EXPECT_EQ(g_heap_bytes.load(), 32u);

    auto memos = measure(kNestedMemos, sizeof(kNestedMemos));
    ASSERT_TRUE(memos.has_value()) << memos.error().message;
    std::printf(
        "certify_indexed nested-memos frames=%zu allocs=%d bytes=%zu\n",
        memos->frame_count(),
        g_heap.load(),
        g_heap_bytes.load());
    EXPECT_EQ(memos->frame_count(), 4u);
    EXPECT_EQ(g_heap.load(), 1);
    EXPECT_EQ(g_heap_bytes.load(), 64u);

    auto nop = measure(kNop63, sizeof(kNop63));
    ASSERT_TRUE(nop.has_value());
    std::printf(
        "certify_indexed nop-63 frames=%zu allocs=%d bytes=%zu\n",
        nop->frame_count(),
        g_heap.load(),
        g_heap_bytes.load());
    EXPECT_EQ(nop->frame_count(), 1u);
    EXPECT_EQ(g_heap.load(), 1);
    EXPECT_EQ(g_heap_bytes.load(), 16u);

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
    EXPECT_EQ(g_heap_bytes.load(), 63u);
}

TEST(AmountView, IndexSinkInlineThenSingleAllocation)
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
    EXPECT_EQ(s.size(), 1u);
    EXPECT_EQ(g_heap.load(), 0);
    EXPECT_EQ(g_heap_bytes.load(), 0u);

    g_heap.store(0);
    g_heap_bytes.store(0);
    g_track = true;
    for (int i = 0; i < 7; ++i)
        s.emit(f);
    g_track = false;
    EXPECT_EQ(s.size(), 8u);
    EXPECT_EQ(g_heap.load(), 0);
    EXPECT_EQ(g_heap_bytes.load(), 0u);

    g_heap.store(0);
    g_heap_bytes.store(0);
    g_track = true;
    auto exact = std::move(s).finish();
    g_track = false;
    EXPECT_EQ(exact.size(), 8u);
    EXPECT_EQ(exact.capacity(), 8u);
    EXPECT_EQ(g_heap.load(), 1);
    EXPECT_EQ(g_heap_bytes.load(), 128u);

    g_heap.store(0);
    g_heap_bytes.store(0);
    g_track = true;
    IndexSink nine;
    for (int i = 0; i < 9; ++i)
        nine.emit(f);
    g_track = false;
    EXPECT_EQ(nine.size(), 9u);
    EXPECT_EQ(g_heap.load(), 1);
    EXPECT_EQ(g_heap_bytes.load(), 256u);

    g_heap.store(0);
    g_heap_bytes.store(0);
    g_track = true;
    auto spilled = std::move(nine).finish();
    g_track = false;
    EXPECT_EQ(spilled.size(), 9u);
    EXPECT_EQ(spilled.capacity(), 16u);
    EXPECT_EQ(g_heap.load(), 0);
    EXPECT_EQ(g_heap_bytes.load(), 0u);
}

TEST(AmountView, CertifiedObjectMaterializesManyFieldsWithoutAllocation)
{
    constexpr uint8_t two_amounts[] = {
        0x61, 0x40, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x42, 0x40, 0x68,
        0x40, 0x00, 0x00, 0x00, 0x00, 0x1e, 0x84, 0x80, 0x81, 0x14,
        0xb5, 0xf7, 0x62, 0x79, 0x8a, 0x53, 0xd5, 0x43, 0xa0, 0x14,
        0xca, 0xf8, 0xb2, 0x97, 0xcf, 0xf8, 0xf2, 0xf9, 0x37, 0xe8};
    auto protocol = Protocol::load_embedded_xahau_protocol();
    auto root = CertifiedRoot::copy_and_certify(
        Slice{two_amounts, sizeof(two_amounts)}, 0, protocol);
    ASSERT_TRUE(root.has_value());

    g_heap.store(0);
    g_heap_bytes.store(0);
    catl::xdata::g_amount_parts_calls.store(0);
    g_track = true;
    CertifiedObject host{std::move(*root)};
    std::array<OwnedAmountParts, 2> amounts{};
    size_t n_amt = 0;
    for (size_t i = 0; i < host.frame_count(); ++i)
    {
        auto value = host.materialize_amount(i);
        if (value)
            amounts[n_amt++] = *value;
    }
    CertifiedObject moved{std::move(host)};
    auto again = moved.materialize_amount(0);
    auto kind = moved.amount_kind(1);
    g_track = false;

    EXPECT_EQ(n_amt, 2u);
    EXPECT_EQ(amounts[0].magnitude, 1000000u);
    EXPECT_EQ(amounts[1].magnitude, 2000000u);
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again->magnitude, 1000000u);
    ASSERT_TRUE(kind.has_value());
    EXPECT_EQ(*kind, AmountRules::Kind::Native);
    EXPECT_EQ(catl::xdata::g_amount_parts_calls.load(), 3);
    EXPECT_EQ(g_heap.load(), 0);
    EXPECT_EQ(g_heap_bytes.load(), 0u);
}

TEST(AmountView, TemporaryOwnerReturnsOnlyOwnedMaterialization)
{
    auto make = []() -> std::optional<CertifiedObject> {
        auto protocol = Protocol::load_embedded_xahau_protocol();
        auto root = CertifiedRoot::copy_and_certify_amount(
            Slice{kNativeAmt, sizeof(kNativeAmt)}, protocol);
        if (!root)
            return std::nullopt;
        return CertifiedObject{std::move(*root)};
    };
    auto amount = make()->materialize_amount(0);
    ASSERT_TRUE(amount.has_value());
    EXPECT_EQ(amount->kind, AmountRules::Kind::Native);
    EXPECT_EQ(amount->magnitude, 1000000u);
}

TEST(AmountView, OwnedMaterializationCopiesIouAndMptIdentity)
{
    auto protocol = Protocol::load_embedded_xahau_protocol();
    std::array<uint8_t, 48> iou{};
    uint64_t const iou_word = AmountRules::kIssued | AmountRules::kPositive |
        (97ull << 54) | AmountRules::kMinMant;
    for (int i = 0; i < 8; ++i)
        iou[i] = static_cast<uint8_t>(iou_word >> (56 - 8 * i));
    iou[20] = 'U';
    iou[21] = 'S';
    iou[22] = 'D';
    for (size_t i = 0; i < 20; ++i)
        iou[28 + i] = static_cast<uint8_t>(i + 1);

    auto iou_root = CertifiedRoot::copy_and_certify_amount(
        Slice{iou.data(), iou.size()}, protocol);
    ASSERT_TRUE(iou_root.has_value());
    auto iou_value =
        CertifiedObject{std::move(*iou_root)}.materialize_amount(0);
    ASSERT_TRUE(iou_value.has_value());
    auto identity = iou_value->iou_identity();
    ASSERT_TRUE(identity.has_value());
    EXPECT_EQ(identity->currency[12], 'U');
    EXPECT_EQ(identity->currency[13], 'S');
    EXPECT_EQ(identity->currency[14], 'D');
    EXPECT_EQ(identity->issuer[0], 1);
    EXPECT_EQ(identity->issuer[19], 20);
    EXPECT_FALSE(iou_value->mpt_id().has_value());

    std::array<uint8_t, 33> mpt{};
    uint64_t const mpt_word = AmountRules::kMpt | AmountRules::kPositive;
    for (int i = 0; i < 8; ++i)
        mpt[i] = static_cast<uint8_t>(mpt_word >> (56 - 8 * i));
    mpt[8] = 42;
    for (size_t i = 0; i < 24; ++i)
        mpt[9 + i] = static_cast<uint8_t>(0xa0 + i);

    auto mpt_root = CertifiedRoot::copy_and_certify_amount(
        Slice{mpt.data(), mpt.size()}, protocol);
    ASSERT_TRUE(mpt_root.has_value());
    auto mpt_value =
        CertifiedObject{std::move(*mpt_root)}.materialize_amount(0);
    ASSERT_TRUE(mpt_value.has_value());
    auto mpt_id = mpt_value->mpt_id();
    ASSERT_TRUE(mpt_id.has_value());
    EXPECT_EQ((*mpt_id)[0], 0xa0);
    EXPECT_EQ((*mpt_id)[23], 0xb7);
    EXPECT_FALSE(mpt_value->iou_identity().has_value());
}

TEST(AmountView, RealObjectsLockSevenEightNineFrameAllocs)
{
    using catl::xdata::Serializer;
    using catl::xdata::VectorSink;
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    auto append = [&](std::vector<uint8_t>& buf,
                      char const* name,
                      std::span<uint8_t const> payload) {
        auto f = protocol.find_field(name);
        if (!f.has_value())
        {
            ADD_FAILURE() << "missing field " << name;
            return;
        }
        VectorSink sink(buf);
        Serializer<VectorSink> ser(sink);
        ser.add_field_header(*f);
        if (f->meta.is_vl_encoded)
            ser.add_vl(payload);
        else
            ser.add_raw(payload);
    };
    uint8_t acct[20]{};
    acct[0] = 0xb5;
    uint8_t drops[8] = {0x40, 0, 0, 0, 0, 0x0f, 0x42, 0x40};
    uint8_t u16[2] = {0, 0};
    uint8_t u32[4] = {0, 0, 0, 1};
    uint8_t key[1] = {0x02};

    auto measure_n = [&](int nfields) {
        std::vector<uint8_t> buf;
        append(buf, "TransactionType", u16);
        append(buf, "Sequence", u32);
        append(buf, "Fee", drops);
        append(buf, "SigningPubKey", key);
        append(buf, "Account", acct);
        append(buf, "Destination", acct);
        append(buf, "Amount", drops);
        if (nfields >= 8)
            append(buf, "Flags", u32);
        if (nfields >= 9)
            append(buf, "LastLedgerSequence", u32);
        g_heap.store(0);
        g_heap_bytes.store(0);
        g_track = true;
        auto idx = certify_indexed(Slice{buf.data(), buf.size()}, 0, protocol);
        g_track = false;
        return idx;
    };

    auto seven = measure_n(7);
    ASSERT_TRUE(seven.has_value()) << seven.error().message;
    EXPECT_EQ(seven->frame_count(), 7u);
    EXPECT_EQ(g_heap.load(), 1);
    EXPECT_EQ(g_heap_bytes.load(), 112u);

    auto eight = measure_n(8);
    ASSERT_TRUE(eight.has_value()) << eight.error().message;
    EXPECT_EQ(eight->frame_count(), 8u);
    EXPECT_EQ(g_heap.load(), 1);
    EXPECT_EQ(g_heap_bytes.load(), 128u);

    auto nine = measure_n(9);
    ASSERT_TRUE(nine.has_value()) << nine.error().message;
    EXPECT_EQ(nine->frame_count(), 9u);
    EXPECT_EQ(g_heap.load(), 1);
    EXPECT_EQ(g_heap_bytes.load(), 256u);
}
