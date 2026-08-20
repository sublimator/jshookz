#include "catl/xdata/protocol.h"
#include "catl/xdata/scan.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <new>
#include <stdlib.h>

namespace {

std::atomic<int> g_heap{0};
bool g_track = false;

void
count_alloc()
{
    if (g_track)
        g_heap.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

void*
operator new(std::size_t n)
{
    void* p = std::malloc(n ? n : 1);
    if (!p)
        throw std::bad_alloc();
    count_alloc();
    return p;
}

void*
operator new(std::size_t n, std::align_val_t a)
{
    void* p = nullptr;
    if (posix_memalign(&p, static_cast<std::size_t>(a), n ? n : static_cast<std::size_t>(a)) != 0)
        throw std::bad_alloc();
    count_alloc();
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

TEST(ScanAlloc, LocateAndCertifyNullSinkDoNotHeapAllocate)
{
    using namespace catl::xdata;
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    std::uint8_t const blob[] = {
        0x81, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    Slice backing{blob, sizeof(blob)};

    g_heap.store(0);
    g_track = true;
    {
        NullSink sink;
        auto loc = scan_scope<ScanMode::Locate>(backing, 0, protocol, sink);
        EXPECT_TRUE(loc.has_value());
        EXPECT_EQ(*loc, sizeof(blob));
    }
    int const locate_allocs = g_heap.load();
    g_heap.store(0);
    {
        NullSink sink;
        auto cert = scan_scope<ScanMode::CertifyWire>(backing, 0, protocol, sink);
        EXPECT_TRUE(cert.has_value());
        EXPECT_EQ(*cert, sizeof(blob));
    }
    int const certify_allocs = g_heap.load();
    g_track = false;

    EXPECT_EQ(locate_allocs, 0) << "Locate+NullSink heap allocations";
    EXPECT_EQ(certify_allocs, 0) << "CertifyWire+NullSink heap allocations";
}
