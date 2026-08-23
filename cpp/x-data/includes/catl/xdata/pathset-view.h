#pragma once

#include "catl/xdata/certified-index.h"
#include "catl/xdata/pathset-rules.h"
#include "catl/xdata/types.h"

#include <optional>
#include <utility>

namespace catl::xdata {

// Sequential native view over one already-certified PathSet field. The view
// owns no directory or backing and does not repeat admission checks.
class PathSetView
{
public:
    PathSetView() = delete;

    static std::optional<PathSetView>
    bind(CertifiedIndex const& idx, size_t ordinal) noexcept
    {
        auto payload = detail::bind_certified_payload(
            idx, ordinal, FieldTypes::PathSet.code);
        if (!payload)
            return std::nullopt;
        return PathSetView{*payload};
    }

    template <class Sink>
    bool
    traverse(Sink& sink) const noexcept(
        noexcept(PathSetRules::traverse_admitted(
            std::declval<Slice>(), std::declval<Sink&>())))
    {
        return PathSetRules::traverse_admitted(payload_, sink);
    }

    Slice
    payload() const noexcept
    {
        return payload_;
    }

private:
    explicit PathSetView(Slice payload) noexcept : payload_(payload)
    {
    }

    Slice payload_{};
};

}  // namespace catl::xdata
