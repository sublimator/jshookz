#pragma once

#include "catl/core/types.h"
#include "catl/xdata/parser-context.h"
#include "catl/xdata/types/pathset.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>

namespace catl::xdata {

enum class PathSetRuleMode : uint8_t
{
    Locate,
    CertifyWire,
    TraverseAdmitted,
};

enum class PathSetRuleRoute : uint8_t
{
    Locate,
    CertifyWire,
    TraverseAdmitted,
    MeasureDirectory,
    FillDirectory,
};

#if defined(CATL_XDATA_TEST_PATHSET_HOOK)
void
pathset_rules_test_hook(PathSetRuleRoute route) noexcept;
#endif

struct PathSetHop
{
    size_t offset = 0;
    uint8_t type = 0;
    Slice account{};
    Slice currency{};
    Slice issuer{};
};

struct PathSetNullSink
{
    void
    on_hop(PathSetHop const&) const noexcept
    {
    }

    void
    on_path_end() const noexcept
    {
    }

    void
    on_end() const noexcept
    {
    }
};

struct PathSetDirectoryShape
{
    size_t paths = 0;
    size_t hops = 0;
    size_t words = 0;
    size_t bytes = 0;
};

// One PathSet token engine. The representation walk itself reports malformed
// input through ParserContext and is noexcept whenever its sink callbacks are
// noexcept. Locate determines the finite extent, CertifyWire additionally
// enforces the seven admitted nonzero masks and nonempty paths, and
// TraverseAdmitted trusts a prior certificate.
struct PathSetRules
{
    static constexpr uint8_t kLegalMask = PathSet::TYPE_ACCOUNT |
        PathSet::TYPE_CURRENCY | PathSet::TYPE_ISSUER;

    static constexpr size_t
    hop_width(uint8_t type) noexcept
    {
        uint8_t const mask = type & kLegalMask;
        size_t components = 0;
        components += (mask & PathSet::TYPE_ACCOUNT) != 0;
        components += (mask & PathSet::TYPE_CURRENCY) != 0;
        components += (mask & PathSet::TYPE_ISSUER) != 0;
        return 1 + 20 * components;
    }

    template <PathSetRuleMode M, class Sink>
    static bool
    walk(ParserContext& ctx, Sink& sink) noexcept(
        noexcept(std::declval<Sink&>().on_hop(
            std::declval<PathSetHop const&>())) &&
        noexcept(std::declval<Sink&>().on_path_end()) &&
        noexcept(std::declval<Sink&>().on_end()))
    {
#if defined(CATL_XDATA_TEST_PATHSET_HOOK)
        if constexpr (M == PathSetRuleMode::Locate)
            pathset_rules_test_hook(PathSetRuleRoute::Locate);
        else if constexpr (M == PathSetRuleMode::CertifyWire)
            pathset_rules_test_hook(PathSetRuleRoute::CertifyWire);
        else
            pathset_rules_test_hook(PathSetRuleRoute::TraverseAdmitted);
#endif
        if (ctx.failed())
            return false;

        size_t const begin = ctx.pos();
        bool saw_hop = false;
        bool path_has_hop = false;
        while (!ctx.failed() && !ctx.empty())
        {
            size_t const token_offset = ctx.pos() - begin;
            uint8_t type = 0;
            if (!ctx.read_u8(type))
                return false;

            if (type == PathSet::END_BYTE)
            {
                if constexpr (M == PathSetRuleMode::CertifyWire)
                {
                    if (!saw_hop || !path_has_hop)
                    {
                        ctx.fail("empty path");
                        return false;
                    }
                }
                sink.on_path_end();
                sink.on_end();
                return true;
            }

            if (type == PathSet::PATH_SEPARATOR)
            {
                if constexpr (M == PathSetRuleMode::CertifyWire)
                {
                    if (!path_has_hop)
                    {
                        ctx.fail("empty path");
                        return false;
                    }
                }
                sink.on_path_end();
                path_has_hop = false;
                continue;
            }

            uint8_t const mask = type & kLegalMask;
            if constexpr (M == PathSetRuleMode::CertifyWire)
            {
                if ((type & static_cast<uint8_t>(~kLegalMask)) != 0 ||
                    mask == 0)
                {
                    ctx.fail("unknown PathSet type bits");
                    return false;
                }
            }

            size_t const component_bytes = hop_width(type) - 1;
            if (ctx.remaining() < component_bytes)
            {
                ctx.fail("truncated PathSet");
                return false;
            }

            uint8_t const* component = ctx.at();
            PathSetHop hop;
            hop.offset = token_offset;
            hop.type = type;
            if (mask & PathSet::TYPE_ACCOUNT)
            {
                hop.account = Slice{component, 20};
                component += 20;
            }
            if (mask & PathSet::TYPE_CURRENCY)
            {
                hop.currency = Slice{component, 20};
                component += 20;
            }
            if (mask & PathSet::TYPE_ISSUER)
                hop.issuer = Slice{component, 20};

            if (!ctx.advance(component_bytes))
                return false;
            sink.on_hop(hop);
            saw_hop = true;
            path_has_hop = true;
        }

        // xahaud-vectors:src/libxrpl/protocol/STPathSet.cpp:57
        // xahaud-vectors:src/libxrpl/protocol/Serializer.cpp:342
        if (!ctx.failed())
            ctx.fail("truncated PathSet");
        return false;
    }

    template <class Sink>
    static bool
    traverse_admitted(Slice payload, Sink& sink) noexcept(
        noexcept(std::declval<Sink&>().on_hop(
            std::declval<PathSetHop const&>())) &&
        noexcept(std::declval<Sink&>().on_path_end()) &&
        noexcept(std::declval<Sink&>().on_end()))
    {
        ParserContext ctx{payload};
        bool const ok = walk<PathSetRuleMode::TraverseAdmitted>(ctx, sink);
        return ok && !ctx.failed() && ctx.pos() == payload.size();
    }

    static std::optional<PathSetDirectoryShape>
    measure_directory(Slice payload) noexcept
    {
#if defined(CATL_XDATA_TEST_PATHSET_HOOK)
        pathset_rules_test_hook(PathSetRuleRoute::MeasureDirectory);
#endif
        struct MeasureSink
        {
            size_t paths = 0;
            size_t hops = 0;
            bool overflow = false;

            void
            on_hop(PathSetHop const&) noexcept
            {
                if (hops == std::numeric_limits<size_t>::max())
                    overflow = true;
                else
                    ++hops;
            }

            void
            on_path_end() noexcept
            {
                if (paths == std::numeric_limits<size_t>::max())
                    overflow = true;
                else
                    ++paths;
            }

            void
            on_end() const noexcept
            {
            }
        } sink;

        if (!traverse_admitted(payload, sink) || sink.overflow ||
            sink.paths > std::numeric_limits<uint32_t>::max() ||
            sink.hops > std::numeric_limits<uint32_t>::max() ||
            payload.size() > std::numeric_limits<uint32_t>::max())
            return std::nullopt;

        if (sink.paths == std::numeric_limits<size_t>::max())
            return std::nullopt;
        size_t const path_words = sink.paths + 1;
        if (sink.hops > std::numeric_limits<size_t>::max() - path_words)
            return std::nullopt;
        size_t const words = path_words + sink.hops;
        if (words > std::numeric_limits<size_t>::max() / sizeof(uint32_t))
            return std::nullopt;
        return PathSetDirectoryShape{
            sink.paths, sink.hops, words, words * sizeof(uint32_t)};
    }

    static bool
    fill_directory(
        Slice payload,
        PathSetDirectoryShape const& shape,
        std::span<uint32_t> words) noexcept
    {
#if defined(CATL_XDATA_TEST_PATHSET_HOOK)
        pathset_rules_test_hook(PathSetRuleRoute::FillDirectory);
#endif
        if (shape.paths == std::numeric_limits<size_t>::max())
            return false;
        size_t const path_words = shape.paths + 1;
        if (shape.hops > std::numeric_limits<size_t>::max() - path_words ||
            shape.words != path_words + shape.hops ||
            words.size() != shape.words || words.empty())
            return false;

        struct FillSink
        {
            std::span<uint32_t> path_starts;
            std::span<uint32_t> hop_offsets;
            size_t path = 0;
            size_t hop = 0;
            bool failed = false;

            void
            on_hop(PathSetHop const& value) noexcept
            {
                if (failed || hop >= hop_offsets.size() ||
                    value.offset > std::numeric_limits<uint32_t>::max())
                {
                    failed = true;
                    return;
                }
                hop_offsets[hop++] = static_cast<uint32_t>(value.offset);
            }

            void
            on_path_end() noexcept
            {
                if (failed || path + 1 >= path_starts.size() ||
                    hop > std::numeric_limits<uint32_t>::max())
                {
                    failed = true;
                    return;
                }
                path_starts[++path] = static_cast<uint32_t>(hop);
            }

            void
            on_end() const noexcept
            {
            }
        } sink{
            words.first(path_words), words.subspan(path_words), 0, 0, false};

        sink.path_starts[0] = 0;
        if (!traverse_admitted(payload, sink) || sink.failed ||
            sink.path != shape.paths || sink.hop != shape.hops)
            return false;
        return true;
    }
};

}  // namespace catl::xdata
