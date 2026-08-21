#include "account_id_fuel_once.h"
#include "catl/xdata/account-id-view.h"
#include "catl/xdata/certified-index.h"
#include "catl/xdata/protocol.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

namespace {

struct AccountIDBank
{
    static constexpr size_t kN = 32;
    uint8_t objects[kN][22]{};
    size_t sizes[kN]{};
    catl::xdata::AccountIDBytes normalized[kN]{};
    uint32_t checksums[kN]{};
    std::vector<catl::xdata::CertifiedIndex> indices;
    std::optional<catl::xdata::AccountIDView> views[kN];
};

size_t
pick_slot(int i)
{
    return (static_cast<size_t>(i) * 7u) % AccountIDBank::kN;
}

bool
covers_all_slots()
{
    bool seen[AccountIDBank::kN]{};
    for (int i = 0; i < static_cast<int>(AccountIDBank::kN); ++i)
        seen[pick_slot(i)] = true;
    for (bool const value : seen)
    {
        if (!value)
            return false;
    }
    return true;
}

uint32_t
checksum(uint8_t const* bytes)
{
    uint32_t value = 2166136261u;
    for (size_t i = 0; i < 20; ++i)
        value = (value ^ bytes[i]) * 16777619u;
    return value;
}

bool
build_bank(AccountIDBank& bank, catl::xdata::Protocol const& protocol)
{
    bank.indices.clear();
    bank.indices.reserve(AccountIDBank::kN);
    for (size_t i = 0; i < AccountIDBank::kN; ++i)
    {
        bank.objects[i][0] = 0x81;  // Account: AccountID field 1.
        if (i % 3 == 0)
        {
            bank.objects[i][1] = 0;
            bank.sizes[i] = 2;
        }
        else
        {
            bank.objects[i][1] = 20;
            bank.sizes[i] = 22;
            for (size_t j = 0; j < 20; ++j)
            {
                uint8_t const value = i % 3 == 1
                    ? 0
                    : static_cast<uint8_t>(1 + i * 13 + j * 7);
                bank.objects[i][2 + j] = value;
                bank.normalized[i][j] = value;
            }
        }
        bank.checksums[i] = checksum(bank.normalized[i].data());
        auto idx = catl::xdata::certify_indexed(
            Slice{bank.objects[i], bank.sizes[i]}, 0, protocol);
        if (!idx)
            return false;
        bank.indices.push_back(std::move(*idx));
        bank.views[i] =
            catl::xdata::AccountIDView::bind(bank.indices.back(), 0);
        if (!bank.views[i] ||
            bank.views[i]->normalized() != bank.normalized[i])
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

    AccountIDBank bank;
    if (!covers_all_slots() || !build_bank(bank, protocol))
    {
        std::puts("FAIL setup");
        return 0;
    }

    uint64_t sink = 0;
#if defined(CATL_XDATA_ACCOUNT_ID_HELPER_CALL_COUNTS)
    account_id_helper_counts_reset_c();
#endif

    auto retained = [&] {
        for (int i = 0; i < iterations; ++i)
        {
            size_t const k = pick_slot(i);
            uint32_t const value =
                account_id_retained_once_c(bank.normalized[k].data());
            if (value != bank.checksums[k])
            {
                std::puts("FAIL retained");
                return;
            }
            sink += value;
        }
        std::puts("coverage_32");
        std::puts("account_id_retained_repeat");
    };
    auto prebound = [&] {
        for (int i = 0; i < iterations; ++i)
        {
            size_t const k = pick_slot(i);
            uint32_t const value =
                account_id_prebound_once_c(&*bank.views[k]);
            if (value != bank.checksums[k])
            {
                std::puts("FAIL prebound");
                return;
            }
            sink += value;
        }
        std::puts("coverage_32");
        std::puts("account_id_prebound_repeat");
    };
    auto rebind = [&] {
        for (int i = 0; i < iterations; ++i)
        {
            size_t const k = pick_slot(i);
            uint32_t const value =
                account_id_rebind_once_c(&bank.indices[k], 0);
            if (value != bank.checksums[k])
            {
                std::puts("FAIL rebind");
                return;
            }
            sink += value;
        }
        std::puts("coverage_32");
        std::puts("account_id_rebind_repeat");
    };
    auto raw = [&] {
        for (int i = 0; i < iterations; ++i)
        {
            size_t const k = pick_slot(i);
            uint32_t const value = account_id_raw_once_c(
                bank.objects[k], bank.sizes[k], &protocol);
            if (value != bank.checksums[k])
            {
                std::puts("FAIL raw");
                return;
            }
            sink += value;
        }
        std::puts("coverage_32");
        std::puts("account_id_raw_recertify_repeat");
    };
    auto invalid = [&] {
        uint8_t invalid_account[21] = {0x81, 0x13};
        auto idx = certify_indexed(
            Slice{invalid_account, sizeof(invalid_account)}, 0, protocol);
        std::puts(idx ? "FAIL invalid accepted" : "invalid_rejected");
    };

    if (std::strcmp(which, "account_id_retained_repeat") == 0)
        retained();
    else if (std::strcmp(which, "account_id_prebound_repeat") == 0)
        prebound();
    else if (std::strcmp(which, "account_id_rebind_repeat") == 0)
        rebind();
    else if (std::strcmp(which, "account_id_raw_recertify_repeat") == 0)
        raw();
    else if (std::strcmp(which, "account_id_invalid_setup") == 0)
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
#if defined(CATL_XDATA_ACCOUNT_ID_HELPER_CALL_COUNTS)
    uint32_t counts[4]{};
    account_id_helper_counts_read_c(counts);
    std::printf(
        "helper_counts retained=%u prebound=%u rebind=%u raw=%u\n",
        counts[0],
        counts[1],
        counts[2],
        counts[3]);
#endif
    return 0;
}
