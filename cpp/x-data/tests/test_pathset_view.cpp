#include "catl/xdata/certified-index.h"
#include "catl/xdata/pathset-rules.h"
#include "catl/xdata/pathset-view.h"
#include "catl/xdata/protocol.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <new>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

using namespace catl::xdata;

static_assert(std::is_trivially_copyable_v<PathSetView>);
static_assert(noexcept(std::declval<PathSetView const&>().traverse(
    std::declval<PathSetNullSink&>())));

namespace catl::xdata {

std::array<std::atomic<unsigned>, 5> g_pathset_rule_calls{};

void
pathset_rules_test_hook(PathSetRuleRoute route) noexcept
{
    g_pathset_rule_calls[static_cast<size_t>(route)].fetch_add(
        1, std::memory_order_relaxed);
}

}  // namespace catl::xdata

namespace {

std::atomic<size_t> g_allocations{0};
std::atomic<size_t> g_requested_bytes{0};
bool g_track_allocations = false;

constexpr std::array<uint8_t, 20> kAccount = {0xb5, 0xf7, 0x62, 0x79, 0x8a,
    0x53, 0xd5, 0x43, 0xa0, 0x14, 0xca, 0xf8, 0xb2, 0x97, 0xcf, 0xf8, 0xf2,
    0xf9, 0x37, 0xe8};

std::vector<uint8_t>
hop(uint8_t type)
{
    std::vector<uint8_t> out{type};
    auto append = [&](uint8_t seed) {
        for (size_t i = 0; i < 20; ++i)
            out.push_back(static_cast<uint8_t>(seed + i));
    };
    if (type & PathSet::TYPE_ACCOUNT)
        out.insert(out.end(), kAccount.begin(), kAccount.end());
    if (type & PathSet::TYPE_CURRENCY)
        append(0x40);
    if (type & PathSet::TYPE_ISSUER)
        append(0x80);
    return out;
}

std::vector<uint8_t>
worked_payload()
{
    auto out = hop(PathSet::TYPE_ACCOUNT);
    auto currency = hop(PathSet::TYPE_CURRENCY);
    auto ci = hop(PathSet::TYPE_CURRENCY | PathSet::TYPE_ISSUER);
    out.insert(out.end(), currency.begin(), currency.end());
    out.push_back(PathSet::PATH_SEPARATOR);
    out.insert(out.end(), ci.begin(), ci.end());
    out.push_back(PathSet::END_BYTE);
    return out;
}

std::vector<uint8_t>
paths_object(std::span<uint8_t const> payload)
{
    // Paths is serialized type 18, field 1: long type header 0x01, 0x12.
    std::vector<uint8_t> out{0x01, 0x12};
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

void
reset_rule_calls()
{
    for (auto& value : g_pathset_rule_calls)
        value.store(0, std::memory_order_relaxed);
}

unsigned
rule_calls(PathSetRuleRoute route)
{
    return g_pathset_rule_calls[static_cast<size_t>(route)].load(
        std::memory_order_relaxed);
}

struct CountSink
{
    size_t paths = 0;
    size_t hops = 0;
    std::array<uint8_t, 3> types{};

    void
    on_hop(PathSetHop const& value) noexcept
    {
        if (hops < types.size())
            types[hops] = value.type;
        ++hops;
    }

    void
    on_path_end() noexcept
    {
        ++paths;
    }

    void
    on_end() const noexcept
    {
    }
};

bool
walks(std::span<uint8_t const> bytes, PathSetRuleMode mode)
{
    ParserContext ctx{Slice{bytes.data(), bytes.size()}};
    PathSetNullSink sink;
    bool ok = false;
    if (mode == PathSetRuleMode::Locate)
        ok = PathSetRules::walk<PathSetRuleMode::Locate>(ctx, sink);
    else
        ok = PathSetRules::walk<PathSetRuleMode::CertifyWire>(ctx, sink);
    return ok && !ctx.failed() && ctx.pos() == bytes.size();
}

}  // namespace

void*
operator new(std::size_t n)
{
    void* value = std::malloc(n ? n : 1);
    if (!value)
        throw std::bad_alloc();
    if (g_track_allocations)
    {
        g_allocations.fetch_add(1, std::memory_order_relaxed);
        g_requested_bytes.fetch_add(n, std::memory_order_relaxed);
    }
    return value;
}

void*
operator new[](std::size_t n)
{
    return ::operator new(n);
}

void*
operator new(std::size_t n, std::align_val_t alignment)
{
    void* value = nullptr;
    if (posix_memalign(&value, static_cast<std::size_t>(alignment),
            n ? n : static_cast<std::size_t>(alignment)) != 0)
        throw std::bad_alloc();
    if (g_track_allocations)
    {
        g_allocations.fetch_add(1, std::memory_order_relaxed);
        g_requested_bytes.fetch_add(n, std::memory_order_relaxed);
    }
    return value;
}

void*
operator new[](std::size_t n, std::align_val_t alignment)
{
    return ::operator new(n, alignment);
}

void
operator delete(void* value) noexcept
{
    std::free(value);
}

void
operator delete(void* value, std::size_t) noexcept
{
    std::free(value);
}

void
operator delete(void* value, std::align_val_t) noexcept
{
    std::free(value);
}

void
operator delete(void* value, std::size_t, std::align_val_t) noexcept
{
    std::free(value);
}

void
operator delete[](void* value) noexcept
{
    std::free(value);
}

void
operator delete[](void* value, std::size_t) noexcept
{
    std::free(value);
}

void
operator delete[](void* value, std::align_val_t) noexcept
{
    std::free(value);
}

void
operator delete[](void* value, std::size_t, std::align_val_t) noexcept
{
    std::free(value);
}

TEST(PathSetRules, EveryMaskHasExactWidthAndAdmission)
{
    size_t admitted = 0;
    for (unsigned raw = 0; raw < 256; ++raw)
    {
        auto const mask = static_cast<uint8_t>(raw);
        size_t const components = ((mask & PathSet::TYPE_ACCOUNT) != 0) +
                                  ((mask & PathSet::TYPE_CURRENCY) != 0) +
                                  ((mask & PathSet::TYPE_ISSUER) != 0);
        EXPECT_EQ(PathSetRules::hop_width(mask), 1 + 20 * components)
            << "mask=" << raw;

        bool const legal_hop =
            mask != PathSet::END_BYTE && mask != PathSet::PATH_SEPARATOR &&
            (mask & static_cast<uint8_t>(~PathSetRules::kLegalMask)) == 0;
        admitted += legal_hop;

        std::vector<uint8_t> bytes;
        if (mask == PathSet::END_BYTE)
            bytes = {PathSet::END_BYTE};
        else if (mask == PathSet::PATH_SEPARATOR)
            bytes = {PathSet::PATH_SEPARATOR, PathSet::END_BYTE};
        else
        {
            bytes = hop(mask);
            bytes.push_back(PathSet::END_BYTE);
        }

        reset_rule_calls();
        EXPECT_TRUE(walks(bytes, PathSetRuleMode::Locate)) << "mask=" << raw;
        EXPECT_EQ(walks(bytes, PathSetRuleMode::CertifyWire), legal_hop)
            << "mask=" << raw;
        EXPECT_EQ(rule_calls(PathSetRuleRoute::Locate), 1u) << raw;
        EXPECT_EQ(rule_calls(PathSetRuleRoute::CertifyWire), 1u) << raw;
        EXPECT_EQ(rule_calls(PathSetRuleRoute::TraverseAdmitted), 0u) << raw;
        EXPECT_EQ(rule_calls(PathSetRuleRoute::MeasureDirectory), 0u) << raw;
        EXPECT_EQ(rule_calls(PathSetRuleRoute::FillDirectory), 0u) << raw;
    }
    EXPECT_EQ(admitted, 7u);
}

TEST(PathSetRules, LocateAndCertifyHaveDifferentDeclaredLaws)
{
    for (uint8_t mask : {uint8_t{0x01}, uint8_t{0x10}, uint8_t{0x20},
             uint8_t{0x11}, uint8_t{0x21}, uint8_t{0x30}, uint8_t{0x31}})
    {
        auto bytes = hop(mask);
        bytes.push_back(0x00);
        EXPECT_TRUE(walks(bytes, PathSetRuleMode::Locate)) << +mask;
        EXPECT_TRUE(walks(bytes, PathSetRuleMode::CertifyWire)) << +mask;
    }

    for (std::vector<uint8_t> bytes :
        {std::vector<uint8_t>{0x00}, std::vector<uint8_t>{0xff, 0x00},
            std::vector<uint8_t>{0xff, 0xff, 0x00},
            std::vector<uint8_t>{0x02, 0x00}, std::vector<uint8_t>{0x80, 0x00}})
    {
        EXPECT_TRUE(walks(bytes, PathSetRuleMode::Locate));
        EXPECT_FALSE(walks(bytes, PathSetRuleMode::CertifyWire));
    }

    auto missing_end = hop(PathSet::TYPE_ACCOUNT);
    EXPECT_FALSE(walks(missing_end, PathSetRuleMode::Locate));
    EXPECT_FALSE(walks(missing_end, PathSetRuleMode::CertifyWire));
}

TEST(PathSetRules, DirectoryWorkedCurveAndMismatchRefusal)
{
    auto const payload = worked_payload();
    reset_rule_calls();
    auto const shape =
        PathSetRules::measure_directory(Slice{payload.data(), payload.size()});
    ASSERT_TRUE(shape);
    EXPECT_EQ(shape->paths, 2u);
    EXPECT_EQ(shape->hops, 3u);
    EXPECT_EQ(shape->words, 6u);
    EXPECT_EQ(shape->bytes, 24u);
    EXPECT_EQ(rule_calls(PathSetRuleRoute::MeasureDirectory), 1u);
    EXPECT_EQ(rule_calls(PathSetRuleRoute::TraverseAdmitted), 1u);
    EXPECT_EQ(rule_calls(PathSetRuleRoute::Locate), 0u);
    EXPECT_EQ(rule_calls(PathSetRuleRoute::CertifyWire), 0u);
    EXPECT_EQ(rule_calls(PathSetRuleRoute::FillDirectory), 0u);

    constexpr uint32_t sentinel = 0xdeadbeefu;
    constexpr std::array<uint32_t, 6> expected{0, 2, 3, 0, 21, 43};
    std::array<uint32_t, 8> guarded{};
    guarded.fill(sentinel);
    reset_rule_calls();
    ASSERT_TRUE(
        PathSetRules::fill_directory(Slice{payload.data(), payload.size()},
            *shape, std::span<uint32_t>{guarded}.subspan(1, expected.size())));
    EXPECT_EQ(guarded.front(), sentinel);
    EXPECT_EQ(guarded.back(), sentinel);
    EXPECT_TRUE(
        std::equal(expected.begin(), expected.end(), guarded.begin() + 1));
    EXPECT_EQ(rule_calls(PathSetRuleRoute::FillDirectory), 1u);
    EXPECT_EQ(rule_calls(PathSetRuleRoute::TraverseAdmitted), 1u);
    EXPECT_EQ(rule_calls(PathSetRuleRoute::MeasureDirectory), 0u);
    EXPECT_EQ(rule_calls(PathSetRuleRoute::Locate), 0u);
    EXPECT_EQ(rule_calls(PathSetRuleRoute::CertifyWire), 0u);

    for (size_t const wrong_size : {size_t{5}, size_t{7}})
    {
        std::array<uint32_t, 7> untouched{};
        untouched.fill(sentinel);
        reset_rule_calls();
        EXPECT_FALSE(
            PathSetRules::fill_directory(Slice{payload.data(), payload.size()},
                *shape, std::span<uint32_t>{untouched}.first(wrong_size)))
            << "wrong_size=" << wrong_size;
        EXPECT_TRUE(std::all_of(untouched.begin(), untouched.end(),
            [](uint32_t value) { return value == sentinel; }))
            << "wrong_size=" << wrong_size;
        EXPECT_EQ(rule_calls(PathSetRuleRoute::FillDirectory), 1u)
            << wrong_size;
        EXPECT_EQ(rule_calls(PathSetRuleRoute::TraverseAdmitted), 0u)
            << wrong_size;
        EXPECT_EQ(rule_calls(PathSetRuleRoute::MeasureDirectory), 0u)
            << wrong_size;
        EXPECT_EQ(rule_calls(PathSetRuleRoute::Locate), 0u) << wrong_size;
        EXPECT_EQ(rule_calls(PathSetRuleRoute::CertifyWire), 0u) << wrong_size;
    }
}

TEST(PathSetView, CertifiedBindAndSequentialTraversal)
{
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    auto const payload = worked_payload();
    auto const object = paths_object(payload);
    auto idx =
        certify_indexed(Slice{object.data(), object.size()}, 0, protocol);
    ASSERT_TRUE(idx);
    ASSERT_EQ(idx->frame_count(), 1u);

    auto view = PathSetView::bind(*idx, 0);
    ASSERT_TRUE(view);
    EXPECT_EQ(view->payload().size(), payload.size());

    CountSink sink;
    ASSERT_TRUE(view->traverse(sink));
    EXPECT_EQ(sink.paths, 2u);
    EXPECT_EQ(sink.hops, 3u);
    EXPECT_EQ(sink.types[0], PathSet::TYPE_ACCOUNT);
    EXPECT_EQ(sink.types[1], PathSet::TYPE_CURRENCY);
    EXPECT_EQ(sink.types[2], PathSet::TYPE_CURRENCY | PathSet::TYPE_ISSUER);
}

TEST(PathSetView, WrongOrdinalAndFieldTypeCannotBind)
{
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    auto const payload = worked_payload();
    auto const object = paths_object(payload);
    auto idx =
        certify_indexed(Slice{object.data(), object.size()}, 0, protocol);
    ASSERT_TRUE(idx);
    ASSERT_EQ(idx->frame_count(), 1u);

    reset_rule_calls();
    EXPECT_FALSE(PathSetView::bind(*idx, idx->frame_count()));
    EXPECT_FALSE(PathSetView::bind(*idx, 99));

    constexpr uint8_t sequence[] = {0x24, 0x00, 0x00, 0x00, 0x01};
    auto wrong_type =
        certify_indexed(Slice{sequence, sizeof(sequence)}, 0, protocol);
    ASSERT_TRUE(wrong_type);
    ASSERT_EQ(wrong_type->frame_count(), 1u);
    EXPECT_FALSE(PathSetView::bind(*wrong_type, 0));

    EXPECT_EQ(rule_calls(PathSetRuleRoute::Locate), 0u);
    EXPECT_EQ(rule_calls(PathSetRuleRoute::CertifyWire), 0u);
    EXPECT_EQ(rule_calls(PathSetRuleRoute::TraverseAdmitted), 0u);
    EXPECT_EQ(rule_calls(PathSetRuleRoute::MeasureDirectory), 0u);
    EXPECT_EQ(rule_calls(PathSetRuleRoute::FillDirectory), 0u);
}

TEST(PathSetView, TraverseAndDirectoryHelpersAllocateNothingAndDoNotCertify)
{
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    auto const payload = worked_payload();
    auto const object = paths_object(payload);
    auto idx =
        certify_indexed(Slice{object.data(), object.size()}, 0, protocol);
    ASSERT_TRUE(idx);
    auto view = PathSetView::bind(*idx, 0);
    ASSERT_TRUE(view);

    reset_rule_calls();
    g_allocations.store(0, std::memory_order_relaxed);
    g_requested_bytes.store(0, std::memory_order_relaxed);
    g_track_allocations = true;
    CountSink sink;
    bool const traversed = view->traverse(sink);
    auto const shape = PathSetRules::measure_directory(view->payload());
    std::array<uint32_t, 6> words{};
    bool const filled =
        shape && PathSetRules::fill_directory(view->payload(), *shape, words);
    g_track_allocations = false;

    ASSERT_TRUE(traversed);
    ASSERT_TRUE(shape);
    ASSERT_TRUE(filled);
    EXPECT_EQ(g_allocations.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(g_requested_bytes.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(rule_calls(PathSetRuleRoute::CertifyWire), 0u);
    EXPECT_EQ(rule_calls(PathSetRuleRoute::Locate), 0u);
    EXPECT_EQ(rule_calls(PathSetRuleRoute::TraverseAdmitted), 3u);
    EXPECT_EQ(rule_calls(PathSetRuleRoute::MeasureDirectory), 1u);
    EXPECT_EQ(rule_calls(PathSetRuleRoute::FillDirectory), 1u);
}

TEST(PathSetView, RepresentationIsExactlyOneSlice)
{
    EXPECT_EQ(sizeof(PathSetView), sizeof(Slice));
    EXPECT_EQ(alignof(PathSetView), alignof(Slice));
}
