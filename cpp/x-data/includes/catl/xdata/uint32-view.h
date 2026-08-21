#pragma once

#include "catl/xdata/certified-index.h"
#include "catl/xdata/codecs/uint.h"
#include "catl/xdata/types.h"

#include <cstdint>
#include <optional>

namespace catl::xdata {

// Fixed-width scalar view over one already-certified UInt32 field.
// It borrows caller-owned backing through CertifiedIndex and owns nothing.
class UInt32View
{
public:
    UInt32View() = delete;

    static std::optional<UInt32View>
    bind(CertifiedIndex const& idx, size_t ordinal) noexcept
    {
        auto payload = detail::bind_certified_payload(
            idx, ordinal, FieldTypes::UInt32.code);
        if (!payload || payload->size() != FieldTypes::UInt32.fixed_size)
            return std::nullopt;
        return UInt32View{*payload};
    }

    uint32_t
    value() const noexcept
    {
        return codecs::UInt32Codec::decode_raw(payload_);
    }

    Slice
    payload() const noexcept
    {
        return payload_;
    }

private:
    explicit UInt32View(Slice payload) noexcept : payload_(payload)
    {
    }

    Slice payload_{};
};

}  // namespace catl::xdata
