#include "catl/xdata/account-id-view.h"
#include "catl/xdata/amount-view.h"
#include "catl/xdata/codecs/account_id.h"
#include "catl/xdata/protocol.h"
#include "catl/xdata/scan.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

using namespace catl::xdata;

namespace catl::xdata::codecs {

std::atomic<int> g_account_base58_calls{0};

void
AccountIDCodec::base58_test_hook() noexcept
{
    g_account_base58_calls.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace catl::xdata::codecs

namespace {

constexpr std::array<uint8_t, 20> kGenesis = {
    0xb5, 0xf7, 0x62, 0x79, 0x8a, 0x53, 0xd5, 0x43, 0xa0, 0x14,
    0xca, 0xf8, 0xb2, 0x97, 0xcf, 0xf8, 0xf2, 0xf9, 0x37, 0xe8};

std::atomic<int> g_heap{0};
std::atomic<std::size_t> g_heap_bytes{0};
bool g_track = false;

std::vector<uint8_t>
account_object(std::span<uint8_t const> payload)
{
    std::vector<uint8_t> out;
    out.reserve(2 + payload.size());
    out.push_back(0x81);  // Account: AccountID field 1.
    out.push_back(static_cast<uint8_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::optional<CertifiedObject>
make_owned_account(std::span<uint8_t const> payload)
{
    auto bytes = account_object(payload);
    auto protocol = Protocol::load_embedded_xahau_protocol();
    auto root = CertifiedRoot::copy_and_certify(
        Slice{bytes.data(), bytes.size()}, 0, protocol);
    if (!root)
        return std::nullopt;
    return CertifiedObject{std::move(*root)};
}

template <class T>
concept AccountBinds = requires(T&& value) {
    AccountIDView::bind(std::forward<T>(value), size_t{0});
};

template <class T>
concept UInt32Binds = requires(T&& value) {
    UInt32View::bind(std::forward<T>(value), size_t{0});
};

static_assert(AccountBinds<CertifiedIndex const&>);
static_assert(!AccountBinds<Slice>);
static_assert(!AccountBinds<CertifiedRoot const&>);
static_assert(UInt32Binds<CertifiedIndex const&>);
static_assert(!UInt32Binds<Slice>);
static_assert(!UInt32Binds<CertifiedRoot const&>);

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

TEST(AccountIDView, EmptyExplicitZeroAndRealRemainWireDistinct)
{
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    std::array<uint8_t, 20> const zero{};

    auto empty_bytes = account_object({});
    auto zero_bytes = account_object(zero);
    auto real_bytes = account_object(kGenesis);
    auto empty_idx = certify_indexed(
        Slice{empty_bytes.data(), empty_bytes.size()}, 0, protocol);
    auto zero_idx = certify_indexed(
        Slice{zero_bytes.data(), zero_bytes.size()}, 0, protocol);
    auto real_idx = certify_indexed(
        Slice{real_bytes.data(), real_bytes.size()}, 0, protocol);
    ASSERT_TRUE(empty_idx.has_value());
    ASSERT_TRUE(zero_idx.has_value());
    ASSERT_TRUE(real_idx.has_value());

    auto empty = AccountIDView::bind(*empty_idx, 0);
    auto explicit_zero = AccountIDView::bind(*zero_idx, 0);
    auto real = AccountIDView::bind(*real_idx, 0);
    ASSERT_TRUE(empty.has_value());
    ASSERT_TRUE(explicit_zero.has_value());
    ASSERT_TRUE(real.has_value());
    EXPECT_TRUE(empty->is_default());
    EXPECT_FALSE(explicit_zero->is_default());
    EXPECT_FALSE(real->is_default());
    EXPECT_EQ(empty->bytes().size(), 0u);
    EXPECT_EQ(explicit_zero->bytes().size(), 20u);
    EXPECT_EQ(real->bytes().size(), 20u);
    EXPECT_EQ(empty->normalized(), explicit_zero->normalized());
    EXPECT_EQ(real->normalized(), kGenesis);
}

TEST(AccountIDView, InvalidLengthsFailExactAndTruncatedWire)
{
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    for (size_t const length : {1u, 19u, 21u})
    {
        std::vector<uint8_t> payload(length, 0x5a);
        auto exact = account_object(payload);
        NullSink sink;
        auto locate = scan_scope<ScanMode::Locate>(
            Slice{exact.data(), exact.size()}, 0, protocol, sink);
        EXPECT_TRUE(locate.has_value()) << "length=" << length;
        auto certified = certify_indexed(
            Slice{exact.data(), exact.size()}, 0, protocol);
        EXPECT_FALSE(certified.has_value()) << "length=" << length;
        EXPECT_FALSE(codecs::AccountIDCodec::normalize_vl_payload(
            Slice{payload.data(), payload.size()}));

        exact.pop_back();
        locate = scan_scope<ScanMode::Locate>(
            Slice{exact.data(), exact.size()}, 0, protocol, sink);
        EXPECT_FALSE(locate.has_value()) << "truncated length=" << length;
        certified = certify_indexed(
            Slice{exact.data(), exact.size()}, 0, protocol);
        EXPECT_FALSE(certified.has_value())
            << "truncated length=" << length;
    }
}

TEST(AccountIDView, WrongOrdinalAndTypeCannotBind)
{
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    auto bytes = account_object(kGenesis);
    auto idx = certify_indexed(
        Slice{bytes.data(), bytes.size()}, 0, protocol);
    ASSERT_TRUE(idx.has_value());
    EXPECT_FALSE(AccountIDView::bind(*idx, 1));
    EXPECT_FALSE(AccountIDView::bind(*idx, 99));

    constexpr uint8_t sequence[] = {0x24, 0x00, 0x00, 0x00, 0x01};
    auto wrong = certify_indexed(
        Slice{sequence, sizeof(sequence)}, 0, protocol);
    ASSERT_TRUE(wrong.has_value());
    EXPECT_FALSE(AccountIDView::bind(*wrong, 0));
}

TEST(AccountIDView, BindBytesNormalizeAndOwnedGetterDoNotAllocate)
{
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    auto bytes = account_object(kGenesis);
    auto idx = certify_indexed(
        Slice{bytes.data(), bytes.size()}, 0, protocol);
    ASSERT_TRUE(idx.has_value());

    g_heap.store(0);
    g_heap_bytes.store(0);
    g_track = true;
    auto view = AccountIDView::bind(*idx, 0);
    ASSERT_TRUE(view.has_value());
    auto const borrowed = view->bytes();
    auto const normalized = view->normalized();
    g_track = false;
    EXPECT_EQ(borrowed.size(), 20u);
    EXPECT_EQ(normalized, kGenesis);
    EXPECT_EQ(g_heap.load(), 0);
    EXPECT_EQ(g_heap_bytes.load(), 0u);

    auto root = CertifiedRoot::copy_and_certify(
        Slice{bytes.data(), bytes.size()}, 0, protocol);
    ASSERT_TRUE(root.has_value());
    CertifiedObject object{std::move(*root)};
    g_heap.store(0);
    g_heap_bytes.store(0);
    g_track = true;
    auto owned = object.materialize_account_id(0);
    g_track = false;
    ASSERT_TRUE(owned.has_value());
    EXPECT_EQ(*owned, kGenesis);
    EXPECT_EQ(g_heap.load(), 0);
    EXPECT_EQ(g_heap_bytes.load(), 0u);
}

TEST(AccountIDView, OwnedIdentitySurvivesSourceAndOwner)
{
    auto bytes = account_object(kGenesis);
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    auto root = CertifiedRoot::copy_and_certify(
        Slice{bytes.data(), bytes.size()}, 0, protocol);
    ASSERT_TRUE(root.has_value());
    CertifiedObject object{std::move(*root)};
    bytes.assign(bytes.size(), 0xff);
    auto owned = object.materialize_account_id(0);
    ASSERT_TRUE(owned.has_value());
    EXPECT_EQ(*owned, kGenesis);

    auto escaped = make_owned_account(kGenesis)->materialize_account_id(0);
    ASSERT_TRUE(escaped.has_value());
    EXPECT_EQ(*escaped, kGenesis);
}

TEST(AccountIDView, Base58RunsOnlyForExplicitAddressSpelling)
{
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    auto bytes = account_object(kGenesis);
    auto idx = certify_indexed(
        Slice{bytes.data(), bytes.size()}, 0, protocol);
    ASSERT_TRUE(idx.has_value());
    auto view = AccountIDView::bind(*idx, 0);
    ASSERT_TRUE(view.has_value());

    codecs::g_account_base58_calls.store(0);
    (void)view->is_default();
    (void)view->bytes();
    (void)view->normalized();
    EXPECT_EQ(codecs::g_account_base58_calls.load(), 0);

    std::array<uint8_t, 19> const invalid{};
    auto invalid_address = codecs::AccountIDCodec::decode_string_expected(
        Slice{invalid.data(), invalid.size()});
    EXPECT_FALSE(invalid_address.has_value());
    EXPECT_EQ(codecs::g_account_base58_calls.load(), 0);

    auto address =
        codecs::AccountIDCodec::decode_string_expected(view->bytes());
    ASSERT_TRUE(address.has_value());
    EXPECT_EQ(*address, "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh");
    EXPECT_EQ(codecs::g_account_base58_calls.load(), 1);

    auto default_address =
        codecs::AccountIDCodec::decode_string_expected(Slice{});
    ASSERT_TRUE(default_address.has_value());
    EXPECT_EQ(*default_address, codecs::AccountIDCodec::ZERO_ACCOUNT_B58);
    EXPECT_EQ(codecs::g_account_base58_calls.load(), 2);

    std::array<uint8_t, 20> const explicit_zero{};
    auto zero_address = codecs::AccountIDCodec::decode_string_expected(
        Slice{explicit_zero.data(), explicit_zero.size()});
    ASSERT_TRUE(zero_address.has_value());
    EXPECT_EQ(*zero_address, codecs::AccountIDCodec::ZERO_ACCOUNT_B58);
    EXPECT_EQ(codecs::g_account_base58_calls.load(), 3);
}

TEST(AccountIDView, RepresentationIsOneSliceAndOwnedBytesAreInline)
{
    static_assert(std::is_trivially_copyable_v<AccountIDView>);
    static_assert(std::is_trivially_copyable_v<AccountIDBytes>);
    static_assert(sizeof(AccountIDView) == sizeof(Slice));
    static_assert(sizeof(AccountIDBytes) == 20);
#if defined(__wasm32__)
    EXPECT_EQ(sizeof(AccountIDView), 8u);
    EXPECT_EQ(sizeof(std::optional<AccountIDView>), 12u);
#elif defined(__aarch64__) || defined(__x86_64__)
    EXPECT_EQ(sizeof(AccountIDView), 16u);
    EXPECT_EQ(sizeof(std::optional<AccountIDView>), 24u);
#endif
}
