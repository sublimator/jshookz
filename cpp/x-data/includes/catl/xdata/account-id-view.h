#pragma once

#include "catl/xdata/certified-index.h"
#include "catl/xdata/codecs/account_id.h"
#include "catl/xdata/types.h"

#include <optional>

namespace catl::xdata {

using AccountIDBytes = codecs::AccountIDCodec::Normalized;

// VL/default identity view over one already-certified AccountID field.
// Empty payload means the default account; explicit zero20 remains distinct
// on the wire. The view borrows caller-owned backing and owns nothing.
class AccountIDView
{
public:
    AccountIDView() = delete;

    static std::optional<AccountIDView>
    bind(CertifiedIndex const& idx, size_t ordinal) noexcept
    {
        auto payload = detail::bind_certified_payload(
            idx, ordinal, FieldTypes::AccountID.code);
        if (!payload ||
            !codecs::AccountIDCodec::valid_vl_payload_size(payload->size()))
            return std::nullopt;
        return AccountIDView{*payload};
    }

    bool
    is_default() const noexcept
    {
        return payload_.empty();
    }

    Slice
    bytes() const noexcept
    {
        return payload_;
    }

    AccountIDBytes
    normalized() const noexcept
    {
        return *codecs::AccountIDCodec::normalize_vl_payload(payload_);
    }

private:
    explicit AccountIDView(Slice payload) noexcept : payload_(payload)
    {
    }

    Slice payload_{};
};

}  // namespace catl::xdata
