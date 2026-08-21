#pragma once

#include "catl/xdata/amount-rules.h"
#include "catl/xdata/certified-index.h"
#include "catl/xdata/types.h"

#include <optional>

namespace catl::xdata {

class AmountView
{
public:
    AmountView() = delete;

    // Capability/frame/type check only. Does not recertify the wire.
    static std::optional<AmountView>
    bind(CertifiedIndex const& idx, size_t ordinal) noexcept
    {
        if (ordinal >= idx.frame_count())
            return std::nullopt;
        FieldFrame const& f = idx.frame(ordinal);
        if (f.header_begin < idx.begin() || f.wire_end > idx.end())
            return std::nullopt;
        if (f.payload_begin < f.header_begin || f.wire_end < f.payload_begin)
            return std::nullopt;
        if (f.wire_end > idx.backing().size())
            return std::nullopt;
        FieldDef const* field = idx.protocol().get_field_by_code(f.field_code);
        if (!field || field->meta.type != FieldTypes::Amount)
            return std::nullopt;
        Slice payload{
            idx.backing().data() + f.payload_begin,
            f.wire_end - f.payload_begin};
        return AmountView{payload};
    }

    AmountRules::Parts
    parts() const noexcept
    {
        return AmountRules::parts(payload_);
    }

    AmountRules::Kind
    kind() const noexcept
    {
        return parts().kind;
    }

    Slice
    payload() const noexcept
    {
        return payload_;
    }

private:
    explicit AmountView(Slice payload) noexcept : payload_(payload) {}

    Slice payload_{};
};

}  // namespace catl::xdata
