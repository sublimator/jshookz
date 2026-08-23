#include "pathset_fuel_once.h"

#include "catl/xdata/certified-index.h"
#include "catl/xdata/pathset-rules.h"
#include "catl/xdata/pathset-view.h"
#include "catl/xdata/protocol.h"

#include <limits>
#include <span>

namespace {

volatile uint32_t g_pathset_escape = 0;

#if defined(CATL_XDATA_PATHSET_HELPER_CALL_COUNTS)
uint32_t g_pathset_helper_counts[5]{};
uint32_t g_pathset_route_counts[5]{};
#define CATL_COUNT_PATHSET_HELPER(i) ++g_pathset_helper_counts[i]
#else
#define CATL_COUNT_PATHSET_HELPER(i) ((void)0)
#endif

void
mix(uint32_t& value, uint8_t byte) noexcept
{
    value = (value ^ byte) * 16777619u;
}

void
mix_u32(uint32_t& value, uint32_t word) noexcept
{
    mix(value, static_cast<uint8_t>(word));
    mix(value, static_cast<uint8_t>(word >> 8));
    mix(value, static_cast<uint8_t>(word >> 16));
    mix(value, static_cast<uint8_t>(word >> 24));
}

struct ChecksumSink
{
    uint32_t value = 2166136261u;
    uint32_t paths = 0;
    uint32_t hops = 0;

    void
    on_hop(catl::xdata::PathSetHop const& hop) noexcept
    {
        mix(value, 0xa1);
        mix_u32(value, static_cast<uint32_t>(hop.offset));
        mix(value, hop.type);
        mix_u32(
            value,
            static_cast<uint32_t>(
                catl::xdata::PathSetRules::hop_width(hop.type)));
        ++hops;
    }

    void
    on_path_end() noexcept
    {
        mix(value, 0xff);
        ++paths;
    }

    void
    on_end() noexcept
    {
        mix(value, 0x00);
        mix_u32(value, paths);
        mix_u32(value, hops);
    }
};

uint32_t
traversal_checksum(catl::xdata::PathSetView const& view) noexcept
{
    ChecksumSink sink;
    if (!view.traverse(sink))
        return 0;
    return sink.value == 0 ? 1 : sink.value;
}

uint32_t
directory_checksum(
    catl::xdata::PathSetDirectoryShape const& shape,
    std::span<uint32_t const> words) noexcept
{
    uint32_t value = 2166136261u;
    mix_u32(value, static_cast<uint32_t>(shape.paths));
    mix_u32(value, static_cast<uint32_t>(shape.hops));
    mix_u32(value, static_cast<uint32_t>(shape.words));
    for (uint32_t const word : words)
        mix_u32(value, word);
    return value == 0 ? 1 : value;
}

}  // namespace

#if defined(CATL_XDATA_PATHSET_HELPER_CALL_COUNTS)
namespace catl::xdata {

void
pathset_rules_test_hook(PathSetRuleRoute route) noexcept
{
    ++g_pathset_route_counts[static_cast<size_t>(route)];
}

}  // namespace catl::xdata
#endif

extern "C" {

uint32_t
pathset_sequential_once_c(void const* view)
{
    CATL_COUNT_PATHSET_HELPER(0);
    auto const* typed = static_cast<catl::xdata::PathSetView const*>(view);
#if defined(CATL_XDATA_PATHSET_WRONG_SEQUENTIAL_CERTIFY_CONTROL)
    // Deliberate poison: admitted traversal must never repeat certification.
    if (typed)
    {
        catl::xdata::ParserContext poisoned_ctx{typed->payload()};
        catl::xdata::PathSetNullSink poisoned_sink;
        (void)catl::xdata::PathSetRules::walk<
            catl::xdata::PathSetRuleMode::CertifyWire>(
            poisoned_ctx, poisoned_sink);
    }
#endif
    uint32_t const value = typed ? traversal_checksum(*typed) : 0;
    g_pathset_escape = value;
    return value;
}

uint32_t
pathset_measure_fill_once_c(
    uint8_t const* payload,
    size_t payload_size,
    uint32_t* words,
    size_t word_capacity)
{
    CATL_COUNT_PATHSET_HELPER(1);
    if (!payload || !words)
    {
        g_pathset_escape = 0;
        return 0;
    }
    Slice const bytes{payload, payload_size};
    auto const shape = catl::xdata::PathSetRules::measure_directory(bytes);
    if (!shape || shape->words != word_capacity ||
        !catl::xdata::PathSetRules::fill_directory(
            bytes, *shape, std::span<uint32_t>{words, word_capacity}))
    {
        g_pathset_escape = 0;
        return 0;
    }
    uint32_t const value = directory_checksum(
        *shape, std::span<uint32_t const>{words, word_capacity});
    g_pathset_escape = value;
    return value;
}

uint32_t
pathset_cached_length_once_c(void const* cache, size_t path)
{
    CATL_COUNT_PATHSET_HELPER(2);
    auto const* typed = static_cast<PathSetFuelCache const*>(cache);
    if (!typed || !typed->directory || path >= typed->paths)
    {
        g_pathset_escape = 0;
        return 0;
    }
#if defined(CATL_XDATA_PATHSET_WRONG_RULE_CONTROL)
    // Deliberate poison: a cached length must never tokenize the payload.
    auto const poisoned = catl::xdata::PathSetRules::measure_directory(
        Slice{typed->payload, typed->payload_size});
    g_pathset_escape ^= poisoned ? static_cast<uint32_t>(poisoned->words) : 0;
#endif
    uint32_t const begin = typed->directory[path];
    uint32_t const end = typed->directory[path + 1];
    if (begin > end || end > typed->hops)
    {
        g_pathset_escape = 0;
        return 0;
    }
    uint32_t value = 2166136261u;
    mix_u32(value, typed->paths);
    mix_u32(value, typed->hops);
    mix_u32(value, static_cast<uint32_t>(path));
    mix_u32(value, end - begin);
    if (value == 0)
        value = 1;
    g_pathset_escape = value;
    return value;
}

uint32_t
pathset_cached_at_once_c(void const* cache, size_t path, size_t path_hop)
{
    CATL_COUNT_PATHSET_HELPER(3);
    auto const* typed = static_cast<PathSetFuelCache const*>(cache);
    if (!typed || !typed->payload || !typed->directory || path >= typed->paths)
    {
        g_pathset_escape = 0;
        return 0;
    }
#if defined(CATL_XDATA_PATHSET_WRONG_HELPER_CONTROL)
    // Deliberate poison: the helper counter must expose this extra route.
    (void)pathset_cached_length_once_c(cache, path);
#endif
    uint32_t const begin = typed->directory[path];
    uint32_t const end = typed->directory[path + 1];
    if (begin > end || end > typed->hops || path_hop >= end - begin)
    {
        g_pathset_escape = 0;
        return 0;
    }
    uint32_t const ordinal = begin + static_cast<uint32_t>(path_hop);
    uint32_t const* hop_offsets = typed->directory + typed->paths + 1;
    uint32_t const offset = hop_offsets[ordinal];
    if (offset >= typed->payload_size)
    {
        g_pathset_escape = 0;
        return 0;
    }
    uint8_t const type = typed->payload[offset];
    size_t const width = catl::xdata::PathSetRules::hop_width(type);
    if (width > typed->payload_size - offset)
    {
        g_pathset_escape = 0;
        return 0;
    }
    uint32_t value = 2166136261u;
    mix_u32(value, static_cast<uint32_t>(path));
    mix_u32(value, static_cast<uint32_t>(path_hop));
    mix_u32(value, ordinal);
    mix_u32(value, offset);
    mix(value, type);
    mix_u32(value, static_cast<uint32_t>(width));
    if (value == 0)
        value = 1;
    g_pathset_escape = value;
    return value;
}

uint32_t
pathset_raw_recertify_once_c(
    uint8_t const* payload,
    size_t payload_size,
    void const* protocol)
{
    CATL_COUNT_PATHSET_HELPER(4);
    auto const* typed_protocol =
        static_cast<catl::xdata::Protocol const*>(protocol);
    if (!payload || !typed_protocol ||
        payload_size > std::numeric_limits<uint32_t>::max())
    {
        g_pathset_escape = 0;
        return 0;
    }
    auto index = catl::xdata::certify_pathset_span(
        Slice{payload, payload_size}, *typed_protocol);
    if (!index)
    {
        g_pathset_escape = 0;
        return 0;
    }
    auto view = catl::xdata::PathSetView::bind(*index, 0);
    uint32_t const value = view ? traversal_checksum(*view) : 0;
    g_pathset_escape = value;
    return value;
}

#if defined(CATL_XDATA_PATHSET_HELPER_CALL_COUNTS)
void
pathset_helper_counts_reset_c()
{
    for (uint32_t& count : g_pathset_helper_counts)
        count = 0;
    for (uint32_t& count : g_pathset_route_counts)
        count = 0;
}

void
pathset_helper_counts_read_c(uint32_t* helpers, uint32_t* routes)
{
    for (size_t i = 0; i < 5; ++i)
    {
        helpers[i] = g_pathset_helper_counts[i];
        routes[i] = g_pathset_route_counts[i];
    }
}
#endif
}
