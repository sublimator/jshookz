// Compile this translation unit with -fno-exceptions and -fno-rtti.
#include "catl/xdata/certified-index.h"
#include "catl/xdata/parser-context.h"
#include "catl/xdata/pathset-rules.h"
#include "catl/xdata/pathset-view.h"
#include "catl/xdata/protocol.h"

#include <array>
#include <span>
#include <utility>

namespace {

struct CountSink
{
    size_t paths = 0;
    size_t hops = 0;

    void
    on_hop(catl::xdata::PathSetHop const&) noexcept
    {
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

static_assert(noexcept(catl::xdata::PathSetRules::hop_width(0x31)));
static_assert(noexcept(catl::xdata::PathSetRules::walk<
                       catl::xdata::PathSetRuleMode::CertifyWire>(
    std::declval<catl::xdata::ParserContext&>(),
    std::declval<CountSink&>())));
static_assert(noexcept(catl::xdata::PathSetRules::traverse_admitted(
    std::declval<Slice>(), std::declval<CountSink&>())));
static_assert(noexcept(catl::xdata::PathSetRules::measure_directory(
    std::declval<Slice>())));
static_assert(noexcept(catl::xdata::PathSetRules::fill_directory(
    std::declval<Slice>(),
    std::declval<catl::xdata::PathSetDirectoryShape const&>(),
    std::declval<std::span<uint32_t>>())));
static_assert(noexcept(catl::xdata::PathSetView::bind(
    std::declval<catl::xdata::CertifiedIndex const&>(), 0)));
static_assert(noexcept(std::declval<catl::xdata::PathSetView const&>().traverse(
    std::declval<CountSink&>())));

}  // namespace

int
main()
{
    using namespace catl::xdata;
    auto const protocol = Protocol::load_embedded_xahau_protocol();

    std::array<uint8_t, 44> payload{};
    payload[0] = PathSet::TYPE_ACCOUNT;
    for (size_t i = 0; i < 20; ++i)
        payload[1 + i] = static_cast<uint8_t>(i + 1);
    payload[21] = PathSet::PATH_SEPARATOR;
    payload[22] = PathSet::TYPE_CURRENCY;
    for (size_t i = 0; i < 20; ++i)
        payload[23 + i] = static_cast<uint8_t>(0x80 + i);
    payload[43] = PathSet::END_BYTE;
    Slice const admitted{payload.data(), payload.size()};

    ParserContext certify_ctx{admitted};
    PathSetNullSink null_sink;
    if (!PathSetRules::walk<PathSetRuleMode::CertifyWire>(
            certify_ctx, null_sink) ||
        certify_ctx.failed() || certify_ctx.pos() != admitted.size())
        return 1;

    auto index = certify_pathset_span(admitted, protocol);
    if (!index)
        return 2;
    auto view = PathSetView::bind(*index, 0);
    if (!view || PathSetView::bind(*index, 1))
        return 3;
    CountSink traversed;
    if (!view->traverse(traversed) || traversed.paths != 2 ||
        traversed.hops != 2)
        return 4;

    auto const shape = PathSetRules::measure_directory(view->payload());
    if (!shape || shape->paths != 2 || shape->hops != 2 ||
        shape->words != 5 || shape->bytes != 20)
        return 5;
    std::array<uint32_t, 5> directory{};
    if (!PathSetRules::fill_directory(view->payload(), *shape, directory) ||
        directory != std::array<uint32_t, 5>{0, 1, 2, 0, 22})
        return 6;

    std::array<uint32_t, 5> sentinel{};
    sentinel.fill(0xdeadbeefu);
    if (PathSetRules::fill_directory(
            view->payload(),
            *shape,
            std::span<uint32_t>{sentinel}.first(4)))
        return 7;
    for (uint32_t const word : sentinel)
    {
        if (word != 0xdeadbeefu)
            return 8;
    }

    std::array<uint8_t, 21> missing_end{};
    missing_end[0] = PathSet::TYPE_ACCOUNT;
    ParserContext locate_missing{Slice{missing_end.data(), missing_end.size()}};
    if (PathSetRules::walk<PathSetRuleMode::Locate>(
            locate_missing, null_sink) ||
        !locate_missing.failed())
        return 9;

    constexpr std::array<uint8_t, 2> illegal = {0x02, 0x00};
    ParserContext locate_illegal{Slice{illegal.data(), illegal.size()}};
    if (!PathSetRules::walk<PathSetRuleMode::Locate>(
            locate_illegal, null_sink) ||
        locate_illegal.failed())
        return 10;
    ParserContext certify_illegal{Slice{illegal.data(), illegal.size()}};
    if (PathSetRules::walk<PathSetRuleMode::CertifyWire>(
            certify_illegal, null_sink) ||
        !certify_illegal.failed())
        return 11;

    return 0;
}
