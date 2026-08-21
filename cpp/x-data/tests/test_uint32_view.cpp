#include "catl/xdata/amount-view.h"
#include "catl/xdata/certified-index.h"
#include "catl/xdata/protocol.h"
#include "catl/xdata/uint32-view.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <optional>
#include <type_traits>
#include <vector>

using namespace catl::xdata;

namespace {

std::atomic<int> g_heap{0};
std::atomic<std::size_t> g_heap_bytes{0};
bool g_track = false;

std::array<uint8_t, 5>
sequence_object(uint32_t value)
{
    return {
        0x24,
        static_cast<uint8_t>(value >> 24),
        static_cast<uint8_t>(value >> 16),
        static_cast<uint8_t>(value >> 8),
        static_cast<uint8_t>(value)};
}

std::optional<CertifiedObject>
make_owned_sequence(uint32_t value)
{
    auto bytes = sequence_object(value);
    auto protocol = Protocol::load_embedded_xahau_protocol();
    auto root = CertifiedRoot::copy_and_certify(
        Slice{bytes.data(), bytes.size()}, 0, protocol);
    if (!root)
        return std::nullopt;
    return CertifiedObject{std::move(*root)};
}

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
operator new[](std::size_t n)
{
    return ::operator new(n);
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

TEST(UInt32View, ZeroOneAndMaximumAreTotal)
{
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    for (uint32_t const value : {0u, 1u, UINT32_MAX})
    {
        auto bytes = sequence_object(value);
        auto idx = certify_indexed(
            Slice{bytes.data(), bytes.size()}, 0, protocol);
        ASSERT_TRUE(idx.has_value()) << idx.error().message;
        ASSERT_EQ(idx->frame_count(), 1u);
        auto view = UInt32View::bind(*idx, 0);
        ASSERT_TRUE(view.has_value());
        EXPECT_EQ(view->value(), value);
        EXPECT_EQ(view->payload().size(), 4u);
    }
}

TEST(UInt32View, EveryTruncatedPayloadFailsCertification)
{
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    for (size_t payload_size = 0; payload_size < 4; ++payload_size)
    {
        std::vector<uint8_t> bytes(1 + payload_size, 0);
        bytes[0] = 0x24;  // Sequence: UInt32 field 4.
        auto idx = certify_indexed(
            Slice{bytes.data(), bytes.size()}, 0, protocol);
        EXPECT_FALSE(idx.has_value()) << "payload_size=" << payload_size;
    }
}

TEST(UInt32View, WrongOrdinalAndTypeCannotBind)
{
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    auto bytes = sequence_object(1);
    auto idx = certify_indexed(
        Slice{bytes.data(), bytes.size()}, 0, protocol);
    ASSERT_TRUE(idx.has_value());
    EXPECT_FALSE(UInt32View::bind(*idx, 1));
    EXPECT_FALSE(UInt32View::bind(*idx, 99));

    constexpr uint8_t empty_account[] = {0x81, 0x00};
    auto account = certify_indexed(
        Slice{empty_account, sizeof(empty_account)}, 0, protocol);
    ASSERT_TRUE(account.has_value());
    EXPECT_FALSE(UInt32View::bind(*account, 0));
}

TEST(UInt32View, BindValueAndOwnedGetterDoNotAllocate)
{
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    auto bytes = sequence_object(0x12345678u);
    auto idx = certify_indexed(
        Slice{bytes.data(), bytes.size()}, 0, protocol);
    ASSERT_TRUE(idx.has_value());

    g_heap.store(0);
    g_heap_bytes.store(0);
    g_track = true;
    auto view = UInt32View::bind(*idx, 0);
    ASSERT_TRUE(view.has_value());
    uint32_t const native_value = view->value();
    g_track = false;
    EXPECT_EQ(native_value, 0x12345678u);
    EXPECT_EQ(g_heap.load(), 0);
    EXPECT_EQ(g_heap_bytes.load(), 0u);

    auto root = CertifiedRoot::copy_and_certify(
        Slice{bytes.data(), bytes.size()}, 0, protocol);
    ASSERT_TRUE(root.has_value());
    CertifiedObject object{std::move(*root)};
    g_heap.store(0);
    g_heap_bytes.store(0);
    g_track = true;
    auto owned_value = object.uint32_value(0);
    g_track = false;
    ASSERT_TRUE(owned_value.has_value());
    EXPECT_EQ(*owned_value, 0x12345678u);
    EXPECT_EQ(g_heap.load(), 0);
    EXPECT_EQ(g_heap_bytes.load(), 0u);
}

TEST(UInt32View, OwnedScalarSurvivesSourceAndOwner)
{
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    auto bytes = sequence_object(0x89abcdefu);
    auto root = CertifiedRoot::copy_and_certify(
        Slice{bytes.data(), bytes.size()}, 0, protocol);
    ASSERT_TRUE(root.has_value());
    CertifiedObject object{std::move(*root)};
    bytes.fill(0xff);
    auto value = object.uint32_value(0);
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 0x89abcdefu);

    auto escaped = make_owned_sequence(0x10203040u)->uint32_value(0);
    ASSERT_TRUE(escaped.has_value());
    EXPECT_EQ(*escaped, 0x10203040u);
}

TEST(UInt32View, RepresentationIsOneSlice)
{
    static_assert(std::is_trivially_copyable_v<UInt32View>);
    static_assert(sizeof(UInt32View) == sizeof(Slice));
    EXPECT_EQ(FieldTypes::UInt32.fixed_size, 4u);
#if defined(__wasm32__)
    EXPECT_EQ(sizeof(UInt32View), 8u);
    EXPECT_EQ(sizeof(std::optional<UInt32View>), 12u);
#elif defined(__aarch64__) || defined(__x86_64__)
    EXPECT_EQ(sizeof(UInt32View), 16u);
    EXPECT_EQ(sizeof(std::optional<UInt32View>), 24u);
#endif
}
