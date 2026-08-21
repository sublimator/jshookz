#pragma once

#include "catl/xdata/amount-rules.h"
#include "catl/xdata/certified-index.h"
#include "catl/xdata/fields.h"
#include "catl/xdata/types.h"

#include <array>
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
    explicit AmountView(Slice payload) noexcept : payload_(payload)
    {
    }

    static std::optional<AmountView>
    bind(CertifiedRoot const& root, size_t ordinal) noexcept
    {
        return bind(root.index(), ordinal);
    }

    Slice payload_{};

    friend class CertifiedObject;
};

// Owned adapter result. It deliberately contains no Slice: currency, issuer,
// and MPT identity remain valid after any owner, optional, or expected dies.
class OwnedAmountParts
{
public:
    struct IouIdentity
    {
        std::array<uint8_t, 20> currency{};
        std::array<uint8_t, 20> issuer{};
    };

    std::optional<IouIdentity>
    iou_identity() const noexcept
    {
        if (kind != AmountRules::Kind::Iou)
            return std::nullopt;
        IouIdentity out;
        for (size_t i = 0; i < out.currency.size(); ++i)
            out.currency[i] = identity_[i];
        for (size_t i = 0; i < out.issuer.size(); ++i)
            out.issuer[i] = identity_[out.currency.size() + i];
        return out;
    }

    std::optional<std::array<uint8_t, 24>>
    mpt_id() const noexcept
    {
        if (kind != AmountRules::Kind::Mpt)
            return std::nullopt;
        std::array<uint8_t, 24> out{};
        for (size_t i = 0; i < out.size(); ++i)
            out[i] = identity_[i];
        return out;
    }

    AmountRules::Kind kind = AmountRules::Kind::Native;
    bool negative = false;
    bool zero = false;
    uint64_t magnitude = 0;
    int32_t exponent = 0;

private:
    std::array<uint8_t, 40> identity_{};

    friend class CertifiedObject;
};

// One owned certification for an escaping object. Typed views are bound and
// consumed synchronously inside this owner; only owned values cross its public
// boundary. Native scoped callers may bind AmountView to CertifiedIndex.
class CertifiedObject
{
public:
    CertifiedObject() = delete;
    CertifiedObject(CertifiedObject const&) = delete;
    CertifiedObject&
    operator=(CertifiedObject const&) = delete;
    CertifiedObject(CertifiedObject&&) noexcept = default;
    CertifiedObject&
    operator=(CertifiedObject&&) noexcept = default;

    explicit CertifiedObject(CertifiedRoot&& root) noexcept
        : root_(std::move(root))
    {
    }

    size_t
    frame_count() const noexcept
    {
        return root_.frame_count();
    }

    std::optional<AmountRules::Kind>
    amount_kind(size_t ordinal) const noexcept
    {
        auto view = AmountView::bind(root_, ordinal);
        if (!view)
            return std::nullopt;
        return view->kind();
    }

    std::optional<OwnedAmountParts>
    materialize_amount(size_t ordinal) const noexcept
    {
        auto view = AmountView::bind(root_, ordinal);
        if (!view)
            return std::nullopt;
        auto const parts = view->parts();
        OwnedAmountParts out;
        out.kind = parts.kind;
        out.negative = parts.negative;
        out.zero = parts.zero;
        out.magnitude = parts.magnitude;
        out.exponent = parts.exponent;
        for (size_t i = 0; i < parts.currency.size() && i < 20; ++i)
            out.identity_[i] = parts.currency.data()[i];
        for (size_t i = 0; i < parts.issuer.size() && i < 20; ++i)
            out.identity_[20 + i] = parts.issuer.data()[i];
        for (size_t i = 0; i < parts.mpt_id.size() && i < 24; ++i)
            out.identity_[i] = parts.mpt_id.data()[i];
        return out;
    }

private:
    CertifiedRoot root_;
};

}  // namespace catl::xdata
