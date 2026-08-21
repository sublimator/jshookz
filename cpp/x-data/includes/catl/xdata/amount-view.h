#pragma once

#include "catl/xdata/amount-rules.h"
#include "catl/xdata/certified-index.h"
#include "catl/xdata/fields.h"
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
        if (get_field_type_code(f.field_code) != FieldTypes::Amount.code)
            return std::nullopt;
        Slice payload{
            idx.backing().data() + f.payload_begin,
            f.wire_end - f.payload_begin};
        return AmountView{payload};
    }

    static std::optional<AmountView>
    bind(CertifiedRoot const& root, size_t ordinal) noexcept
    {
        return bind(root.index(), ordinal);
    }

    // Temporary root would dangle the returned Slice. Escaping
    // consumers use AnchoredAmount::bind(CertifiedRoot&&, ordinal).
    static std::optional<AmountView>
    bind(CertifiedRoot&& root, size_t ordinal) = delete;

    AmountRules::Parts
    parts() const noexcept
    {
        return AmountRules::parts(payload_);
    }

    // First-byte kind. Does not call parts().
    AmountRules::Kind
    kind() const noexcept
    {
        return AmountRules::kind(payload_);
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

// Escaping/JS aggregate: owns the certified bytes and a bound Amount view
// into them. Does not heap-allocate per view, copy bytes, or refcount.
// Native AmountView remains the nonescaping borrow.
class AnchoredAmount
{
public:
    AnchoredAmount() = delete;
    AnchoredAmount(AnchoredAmount const&) = delete;
    AnchoredAmount& operator=(AnchoredAmount const&) = delete;
    AnchoredAmount(AnchoredAmount&&) noexcept = default;
    AnchoredAmount& operator=(AnchoredAmount&&) noexcept = default;

    static std::optional<AnchoredAmount>
    bind(CertifiedRoot&& root, size_t ordinal) noexcept
    {
        auto v = AmountView::bind(root, ordinal);
        if (!v)
            return std::nullopt;
        return AnchoredAmount{std::move(root), *v};
    }

    AmountRules::Parts
    parts() const noexcept
    {
        return view_.parts();
    }

    AmountRules::Kind
    kind() const noexcept
    {
        return view_.kind();
    }

    Slice
    payload() const noexcept
    {
        return view_.payload();
    }

private:
    AnchoredAmount(CertifiedRoot root, AmountView view) noexcept
        : root_(std::move(root))
        , view_(view)
    {
    }

    CertifiedRoot root_;
    AmountView view_;
};

}  // namespace catl::xdata
