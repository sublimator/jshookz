#include "catl/xdata/certified-index.h"
#include "catl/xdata/protocol.h"
#include "catl/xdata/uint32-view.h"
#include "uint32_fuel_once.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <vector>

namespace {

struct UInt32Bank
{
    static constexpr size_t kN = 32;
    uint8_t objects[kN][5]{};
    uint32_t values[kN]{};
    std::vector<catl::xdata::CertifiedIndex> indices;
    std::optional<catl::xdata::UInt32View> views[kN];
};

size_t
pick_slot(int i)
{
    return (static_cast<size_t>(i) * 7u) % UInt32Bank::kN;
}

bool
covers_all_slots()
{
    bool seen[UInt32Bank::kN]{};
    for (int i = 0; i < static_cast<int>(UInt32Bank::kN); ++i)
        seen[pick_slot(i)] = true;
    for (bool const value : seen)
    {
        if (!value)
            return false;
    }
    return true;
}

bool
build_bank(UInt32Bank& bank, catl::xdata::Protocol const& protocol)
{
    bank.indices.clear();
    bank.indices.reserve(UInt32Bank::kN);
    for (size_t i = 0; i < UInt32Bank::kN; ++i)
    {
        uint32_t const value =
            0x10203040u + static_cast<uint32_t>(i * 0x010101u);
        bank.values[i] = value;
        bank.objects[i][0] = 0x24;  // Sequence: UInt32 field 4.
        bank.objects[i][1] = static_cast<uint8_t>(value >> 24);
        bank.objects[i][2] = static_cast<uint8_t>(value >> 16);
        bank.objects[i][3] = static_cast<uint8_t>(value >> 8);
        bank.objects[i][4] = static_cast<uint8_t>(value);
        auto idx = catl::xdata::certify_indexed(
            Slice{bank.objects[i], sizeof(bank.objects[i])}, 0, protocol);
        if (!idx)
            return false;
        bank.indices.push_back(std::move(*idx));
        bank.views[i] = catl::xdata::UInt32View::bind(bank.indices.back(), 0);
        if (!bank.views[i])
            return false;
    }
    return true;
}

}  // namespace

int
main(int argc, char** argv)
{
    using namespace catl::xdata;
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    char const* which = argc > 1 ? argv[1] : "all";
    int iterations = argc > 2 ? std::atoi(argv[2]) : 2000;
    if (iterations < 0)
        iterations = 0;

    UInt32Bank bank;
    if (!covers_all_slots() || !build_bank(bank, protocol))
    {
        std::puts("FAIL setup");
        return 0;
    }

    uint64_t sink = 0;
#if defined(CATL_XDATA_UINT32_HELPER_CALL_COUNTS)
    u32_helper_counts_reset_c();
#endif

    auto retained = [&] {
        for (int i = 0; i < iterations; ++i)
        {
            size_t const k = pick_slot(i);
            uint32_t const value = u32_retained_once_c(bank.values[k]);
            if (value != bank.values[k])
            {
                std::puts("FAIL retained");
                return;
            }
            sink += value;
        }
        std::puts("coverage_32");
        std::puts("uint32_retained_repeat");
    };
    auto prebound = [&] {
        for (int i = 0; i < iterations; ++i)
        {
            size_t const k = pick_slot(i);
            uint32_t const value = u32_prebound_once_c(&*bank.views[k]);
            if (value != bank.values[k])
            {
                std::puts("FAIL prebound");
                return;
            }
            sink += value;
        }
        std::puts("coverage_32");
        std::puts("uint32_prebound_repeat");
    };
    auto rebind = [&] {
        for (int i = 0; i < iterations; ++i)
        {
            size_t const k = pick_slot(i);
            uint32_t const value =
                u32_rebind_once_c(&bank.indices[k], 0);
            if (value != bank.values[k])
            {
                std::puts("FAIL rebind");
                return;
            }
            sink += value;
        }
        std::puts("coverage_32");
        std::puts("uint32_rebind_repeat");
    };
    auto raw = [&] {
        for (int i = 0; i < iterations; ++i)
        {
            size_t const k = pick_slot(i);
            uint32_t const value = u32_raw_once_c(
                bank.objects[k], sizeof(bank.objects[k]), &protocol);
            if (value != bank.values[k])
            {
                std::puts("FAIL raw");
                return;
            }
            sink += value;
        }
        std::puts("coverage_32");
        std::puts("uint32_raw_recertify_repeat");
    };
    auto invalid = [&] {
        constexpr uint8_t truncated[] = {0x24, 0x00, 0x00, 0x00};
        auto idx = certify_indexed(
            Slice{truncated, sizeof(truncated)}, 0, protocol);
        std::puts(idx ? "FAIL invalid accepted" : "invalid_rejected");
    };

    if (std::strcmp(which, "uint32_retained_repeat") == 0)
        retained();
    else if (std::strcmp(which, "uint32_prebound_repeat") == 0)
        prebound();
    else if (std::strcmp(which, "uint32_rebind_repeat") == 0)
        rebind();
    else if (std::strcmp(which, "uint32_raw_recertify_repeat") == 0)
        raw();
    else if (std::strcmp(which, "uint32_invalid_setup") == 0)
        invalid();
    else
    {
        retained();
        prebound();
        rebind();
        raw();
    }

    if (sink == 0xffffffffffffull)
        std::puts("never");
#if defined(CATL_XDATA_UINT32_HELPER_CALL_COUNTS)
    uint32_t counts[4]{};
    u32_helper_counts_read_c(counts);
    std::printf(
        "helper_counts retained=%u prebound=%u rebind=%u raw=%u\n",
        counts[0],
        counts[1],
        counts[2],
        counts[3]);
#endif
    return 0;
}
