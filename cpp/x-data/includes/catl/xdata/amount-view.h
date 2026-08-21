#pragma once

#include "catl/xdata/amount-rules.h"
#include "catl/xdata/certified-index.h"
#include "catl/xdata/fields.h"
#include "catl/xdata/types.h"

#include <optional>
#include <utility>

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
    // consumers use CertifiedObject / AnchoredAmount.
    static std::optional<AmountView>
    bind(CertifiedRoot&& root, size_t ordinal) = delete;
    static std::optional<AmountView>
    bind(CertifiedRoot const&& root, size_t ordinal) = delete;

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

    void
    vacate() noexcept
    {
        payload_ = {};
    }

    Slice payload_{};

    friend class AnchoredAmount;
};

// One owned certification. Bind many typed views while *this is live.
class CertifiedObject
{
public:
    CertifiedObject() = delete;
    CertifiedObject(CertifiedObject const&) = delete;
    CertifiedObject& operator=(CertifiedObject const&) = delete;
    CertifiedObject(CertifiedObject&&) noexcept = default;
    CertifiedObject& operator=(CertifiedObject&&) noexcept = default;

    explicit CertifiedObject(CertifiedRoot&& root) noexcept
        : root_(std::move(root))
    {
    }

    CertifiedRoot const&
    root() const& noexcept
    {
        return root_;
    }

    CertifiedRoot const&
    root() const&& = delete;

    size_t
    frame_count() const noexcept
    {
        return root_.frame_count();
    }

    std::optional<AmountView>
    amount(size_t ordinal) const& noexcept
    {
        return AmountView::bind(root_, ordinal);
    }

    std::optional<AmountView>
    amount(size_t ordinal) const&& = delete;

private:
    CertifiedRoot root_;
};

// One-field owned Amount. Moved-from objects are vacant. Borrowed parts
// and payload are lvalue-only so they cannot outlive a temporary owner.
class AnchoredAmount
{
public:
    AnchoredAmount(AnchoredAmount const&) = delete;
    AnchoredAmount& operator=(AnchoredAmount const&) = delete;

    AnchoredAmount(AnchoredAmount&& other) noexcept
        : root_(std::move(other.root_))
        , view_(other.view_)
    {
        other.view_.vacate();
    }

    AnchoredAmount&
    operator=(AnchoredAmount&& other) noexcept
    {
        if (this != &other)
        {
            root_ = std::move(other.root_);
            view_ = other.view_;
            other.view_.vacate();
        }
        return *this;
    }

    // Returns a vacant object on bind failure. Not optional: bind(...)->parts()
    // would be an lvalue call on a dying owner.
    static AnchoredAmount
    bind(CertifiedRoot&& root, size_t ordinal) noexcept
    {
        auto v = AmountView::bind(root, ordinal);
        if (!v)
            return AnchoredAmount{};
        return AnchoredAmount{std::move(root), *v};
    }

    explicit operator bool() const noexcept
    {
        return !view_.payload().empty();
    }

    AmountRules::Parts
    parts() const& noexcept
    {
        return view_.parts();
    }

    AmountRules::Parts
    parts() const&& = delete;

    AmountRules::Kind
    kind() const noexcept
    {
        return view_.kind();
    }

    Slice
    payload() const& noexcept
    {
        return view_.payload();
    }

    Slice
    payload() const&& = delete;

private:
    AnchoredAmount() noexcept : root_{}, view_{Slice{}} {}

    AnchoredAmount(CertifiedRoot root, AmountView view) noexcept
        : root_(std::move(root))
        , view_(view)
    {
    }

    CertifiedRoot root_;
    AmountView view_;
};

}  // namespace catl::xdata
