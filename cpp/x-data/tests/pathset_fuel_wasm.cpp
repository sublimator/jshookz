#include "pathset_fuel_once.h"

#include "catl/xdata/certified-index.h"
#include "catl/xdata/pathset-rules.h"
#include "catl/xdata/pathset-view.h"
#include "catl/xdata/protocol.h"
#include "catl/xdata/types/pathset.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace {

constexpr size_t kBankSize = 32;
constexpr size_t kMaxPayload = 270;
constexpr size_t kMaxDirectoryWords = 11;

enum class Lane : uint8_t
{
    Sequential,
    MeasureFill,
    CachedLength,
    CachedAt,
    RawRecertify,
};

struct FuelCase
{
    std::array<uint8_t, kMaxPayload> payload{};
    size_t size = 0;
    std::array<uint32_t, kMaxDirectoryWords> directory{};
    PathSetFuelCache cache{};
    size_t selected_path = 0;
    size_t selected_hop = 0;
    std::array<uint32_t, 5> expected{};
};

struct FuelBank
{
    std::array<FuelCase, kBankSize> cases{};
    std::vector<catl::xdata::CertifiedIndex> indices;
    std::array<std::optional<catl::xdata::PathSetView>, kBankSize> views{};
};

struct OneHopBank : FuelBank
{
};

struct ManyHopBank : FuelBank
{
};

size_t
pick_slot(int iteration) noexcept
{
    return (static_cast<size_t>(iteration) * 7u) % kBankSize;
}

bool
covers_all_slots() noexcept
{
    bool seen[kBankSize]{};
    for (int i = 0; i < static_cast<int>(kBankSize); ++i)
        seen[pick_slot(i)] = true;
    for (bool const value : seen)
    {
        if (!value)
            return false;
    }
    return true;
}

bool
append_hop(FuelCase& item, uint8_t type, uint8_t seed) noexcept
{
    size_t const width = catl::xdata::PathSetRules::hop_width(type);
    if (item.size > item.payload.size() - width)
        return false;
    item.payload[item.size++] = type;
    for (uint8_t component : {catl::xdata::PathSet::TYPE_ACCOUNT,
                              catl::xdata::PathSet::TYPE_CURRENCY,
                              catl::xdata::PathSet::TYPE_ISSUER})
    {
        if ((type & component) == 0)
            continue;
        for (size_t j = 0; j < 20; ++j)
        {
            item.payload[item.size++] = static_cast<uint8_t>(
                seed + component * 3u + j * 11u);
        }
    }
    return true;
}

bool
finish_case(
    FuelBank& bank,
    size_t slot,
    catl::xdata::Protocol const& protocol,
    size_t expected_paths,
    size_t expected_hops) noexcept
{
    FuelCase& item = bank.cases[slot];
    if (item.size >= item.payload.size())
        return false;
    item.payload[item.size++] = catl::xdata::PathSet::END_BYTE;
    auto index = catl::xdata::certify_pathset_span(
        Slice{item.payload.data(), item.size}, protocol);
    if (!index)
        return false;
    bank.indices.push_back(std::move(*index));
    bank.views[slot] =
        catl::xdata::PathSetView::bind(bank.indices.back(), 0);
    if (!bank.views[slot])
        return false;

    auto const shape = catl::xdata::PathSetRules::measure_directory(
        bank.views[slot]->payload());
    if (!shape || shape->paths != expected_paths ||
        shape->hops != expected_hops ||
        shape->words > item.directory.size() ||
        !catl::xdata::PathSetRules::fill_directory(
            bank.views[slot]->payload(),
            *shape,
            std::span<uint32_t>{item.directory}.first(shape->words)))
        return false;

    item.cache = PathSetFuelCache{
        item.payload.data(),
        item.size,
        item.directory.data(),
        static_cast<uint32_t>(shape->paths),
        static_cast<uint32_t>(shape->hops)};

    std::array<uint32_t, kMaxDirectoryWords> scratch{};
    item.expected[static_cast<size_t>(Lane::Sequential)] =
        pathset_sequential_once_c(&*bank.views[slot]);
    item.expected[static_cast<size_t>(Lane::MeasureFill)] =
        pathset_measure_fill_once_c(
            item.payload.data(),
            item.size,
            scratch.data(),
            shape->words);
    item.expected[static_cast<size_t>(Lane::CachedLength)] =
        pathset_cached_length_once_c(&item.cache, item.selected_path);
    item.expected[static_cast<size_t>(Lane::CachedAt)] =
        pathset_cached_at_once_c(
            &item.cache, item.selected_path, item.selected_hop);
    item.expected[static_cast<size_t>(Lane::RawRecertify)] =
        pathset_raw_recertify_once_c(
            item.payload.data(), item.size, &protocol);
    if (item.expected[static_cast<size_t>(Lane::Sequential)] !=
            item.expected[static_cast<size_t>(Lane::RawRecertify)] ||
        item.expected[static_cast<size_t>(Lane::MeasureFill)] == 0 ||
        item.expected[static_cast<size_t>(Lane::CachedLength)] == 0 ||
        item.expected[static_cast<size_t>(Lane::CachedAt)] == 0)
        return false;
    return true;
}

bool
build_one_hop_bank(
    OneHopBank& bank, catl::xdata::Protocol const& protocol) noexcept
{
    constexpr std::array<uint8_t, 7> types = {
        0x01, 0x10, 0x20, 0x11, 0x21, 0x30, 0x31};
    bank.indices.clear();
    bank.indices.reserve(kBankSize);
    for (size_t i = 0; i < kBankSize; ++i)
    {
        FuelCase& item = bank.cases[i];
        item.selected_path = 0;
        item.selected_hop = 0;
        if (!append_hop(
                item,
                types[i % types.size()],
                static_cast<uint8_t>(3u + i * 17u)) ||
            !finish_case(bank, i, protocol, 1, 1))
            return false;
    }
    return true;
}

bool
build_many_hop_bank(
    ManyHopBank& bank, catl::xdata::Protocol const& protocol) noexcept
{
    constexpr std::array<uint8_t, 8> types = {
        0x01, 0x10, 0x20, 0x11, 0x21, 0x30, 0x31, 0x01};
    bank.indices.clear();
    bank.indices.reserve(kBankSize);
    for (size_t i = 0; i < kBankSize; ++i)
    {
        FuelCase& item = bank.cases[i];
        item.selected_path = i % 2;
        item.selected_hop = (i * 3u) % 4;
        for (size_t h = 0; h < types.size(); ++h)
        {
            if (h == 4)
                item.payload[item.size++] = catl::xdata::PathSet::PATH_SEPARATOR;
            if (!append_hop(
                    item,
                    types[h],
                    static_cast<uint8_t>(5u + i * 13u + h * 19u)))
                return false;
        }
        if (!finish_case(bank, i, protocol, 2, 8))
            return false;
        constexpr std::array<uint32_t, kMaxDirectoryWords> expected = {
            0, 4, 8, 0, 21, 42, 63, 105, 146, 187, 248};
        if (item.size != kMaxPayload || item.directory != expected)
            return false;
    }
    return true;
}

bool
run_lane(
    FuelBank const& bank,
    Lane lane,
    int iterations,
    catl::xdata::Protocol const& protocol,
    uint64_t& escape) noexcept
{
    std::array<uint32_t, kMaxDirectoryWords> scratch{};
    for (int i = 0; i < iterations; ++i)
    {
        size_t const slot = pick_slot(i);
        FuelCase const& item = bank.cases[slot];
        uint32_t value = 0;
        switch (lane)
        {
            case Lane::Sequential:
                value = pathset_sequential_once_c(&*bank.views[slot]);
                break;
            case Lane::MeasureFill:
                value = pathset_measure_fill_once_c(
                    item.payload.data(),
                    item.size,
                    scratch.data(),
                    item.cache.paths + 1u + item.cache.hops);
                break;
            case Lane::CachedLength:
                value = pathset_cached_length_once_c(
                    &item.cache, item.selected_path);
                break;
            case Lane::CachedAt:
                value = pathset_cached_at_once_c(
                    &item.cache, item.selected_path, item.selected_hop);
                break;
            case Lane::RawRecertify:
                value = pathset_raw_recertify_once_c(
                    item.payload.data(), item.size, &protocol);
                break;
        }
        if (value != item.expected[static_cast<size_t>(lane)])
            return false;
        escape += value;
    }
    return true;
}

bool
dispatch(
    char const* which,
    char const* expected,
    char const* bank_marker,
    FuelBank const& bank,
    Lane lane,
    int iterations,
    catl::xdata::Protocol const& protocol,
    uint64_t& escape) noexcept
{
    if (std::strcmp(which, expected) != 0)
        return false;
    if (!run_lane(bank, lane, iterations, protocol, escape))
        std::puts("FAIL lane");
    else
    {
        std::puts("coverage_32");
        std::puts(bank_marker);
        std::puts(expected);
    }
    return true;
}

}  // namespace

int
main(int argc, char** argv)
{
    auto const protocol =
        catl::xdata::Protocol::load_embedded_xahau_protocol();
    char const* which = argc > 1 ? argv[1] : "all";
    int iterations = argc > 2 ? std::atoi(argv[2]) : 2048;
    if (iterations < 0)
        iterations = 0;

    OneHopBank one_hop;
    ManyHopBank many_hop;
    if (!covers_all_slots() || !build_one_hop_bank(one_hop, protocol) ||
        !build_many_hop_bank(many_hop, protocol))
    {
        std::puts("FAIL setup");
        return 0;
    }

#if defined(CATL_XDATA_PATHSET_HELPER_CALL_COUNTS)
    pathset_helper_counts_reset_c();
#endif
    uint64_t escape = 0;
    bool matched = false;
#define CATL_PATHSET_DISPATCH(mode, marker, bank, lane)                         \
    matched = dispatch(                                                        \
                  which, mode, marker, bank, Lane::lane, iterations, protocol, \
                  escape) ||                                                   \
        matched
    CATL_PATHSET_DISPATCH(
        "pathset_one_sequential_repeat", "bank_one_hop", one_hop, Sequential);
    CATL_PATHSET_DISPATCH(
        "pathset_one_measure_fill_repeat", "bank_one_hop", one_hop, MeasureFill);
    CATL_PATHSET_DISPATCH(
        "pathset_one_cached_length_repeat", "bank_one_hop", one_hop, CachedLength);
    CATL_PATHSET_DISPATCH(
        "pathset_one_cached_at_repeat", "bank_one_hop", one_hop, CachedAt);
    CATL_PATHSET_DISPATCH(
        "pathset_one_raw_recertify_repeat", "bank_one_hop", one_hop, RawRecertify);
    CATL_PATHSET_DISPATCH(
        "pathset_many_sequential_repeat", "bank_many_hop", many_hop, Sequential);
    CATL_PATHSET_DISPATCH(
        "pathset_many_measure_fill_repeat", "bank_many_hop", many_hop, MeasureFill);
    CATL_PATHSET_DISPATCH(
        "pathset_many_cached_length_repeat", "bank_many_hop", many_hop, CachedLength);
    CATL_PATHSET_DISPATCH(
        "pathset_many_cached_at_repeat", "bank_many_hop", many_hop, CachedAt);
    CATL_PATHSET_DISPATCH(
        "pathset_many_raw_recertify_repeat", "bank_many_hop", many_hop, RawRecertify);
#undef CATL_PATHSET_DISPATCH
    if (!matched)
        std::puts("FAIL unknown mode");

    if (escape == 0xffffffffffffffffull)
        std::puts("never");
#if defined(CATL_XDATA_PATHSET_HELPER_CALL_COUNTS)
    uint32_t helpers[5]{};
    uint32_t routes[5]{};
    pathset_helper_counts_read_c(helpers, routes);
    std::printf(
        "helper_counts sequential=%u measure_fill=%u cached_length=%u "
        "cached_at=%u raw=%u\n",
        helpers[0],
        helpers[1],
        helpers[2],
        helpers[3],
        helpers[4]);
    std::printf(
        "route_counts locate=%u certify=%u traverse=%u measure=%u fill=%u\n",
        routes[0],
        routes[1],
        routes[2],
        routes[3],
        routes[4]);
#endif
    return 0;
}
