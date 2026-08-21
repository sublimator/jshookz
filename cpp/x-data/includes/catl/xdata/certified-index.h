#pragma once

#include "catl/xdata/amount-rules.h"
#include "catl/xdata/protocol.h"
#include "catl/xdata/scan.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <utility>
#include <vector>

namespace catl::xdata {

class CertifiedObject;

// Non-owning certificate: one successful CertifyWire+IndexSink pass.
// Frames are only bindable through this object; caller-authored FieldFrame
// values cannot mint a view.
class CertifiedIndex
{
public:
    Slice
    backing() const& noexcept
    {
        return backing_;
    }

    Slice
    backing() const&& = delete;

    uint32_t
    begin() const noexcept
    {
        return begin_;
    }

    uint32_t
    end() const noexcept
    {
        return end_;
    }

    size_t
    frame_count() const noexcept
    {
        return frames_.size();
    }

    FieldFrame const&
    frame(size_t i) const noexcept
    {
        return frames_[i];
    }

private:
    CertifiedIndex() = default;

    friend std::expected<CertifiedIndex, CodecErrorValue>
    certify_indexed(Slice, uint32_t, Protocol const&);
    friend std::expected<CertifiedIndex, CodecErrorValue>
    certify_amount_span(Slice, Protocol const&);
    friend class CertifiedRoot;
    CertifiedIndex(
        Slice backing,
        uint32_t begin,
        uint32_t end,
        std::vector<FieldFrame> frames)
        : backing_(backing)
        , begin_(begin)
        , end_(end)
        , frames_(std::move(frames))
    {
    }

    Slice backing_{};
    uint32_t begin_ = 0;
    uint32_t end_ = 0;
    std::vector<FieldFrame> frames_;
};

namespace detail {

// Shared native typed-view binder. The certificate proves the frame came from
// one successful CertifyWire pass; this performs capability/range/type checks
// only and deliberately does not recertify caller-owned bytes.
inline std::optional<Slice>
bind_certified_payload(
    CertifiedIndex const& idx,
    size_t ordinal,
    uint16_t expected_type) noexcept
{
    if (ordinal >= idx.frame_count())
        return std::nullopt;
    FieldFrame const& f = idx.frame(ordinal);
    if (f.header_begin < idx.begin() || f.wire_end > idx.end())
        return std::nullopt;
    if (f.payload_begin < f.header_begin || f.wire_end < f.payload_begin)
        return std::nullopt;
    Slice const backing = idx.backing();
    if (f.wire_end > backing.size())
        return std::nullopt;
    if (get_field_type_code(f.field_code) != expected_type)
        return std::nullopt;
    return Slice{
        backing.data() + f.payload_begin, f.wire_end - f.payload_begin};
}

}  // namespace detail

inline std::expected<CertifiedIndex, CodecErrorValue>
certify_indexed(
    Slice backing,
    uint32_t begin,
    Protocol const& protocol)
{
    IndexSink sink;
    auto end = scan_scope<ScanMode::CertifyWire>(backing, begin, protocol, sink);
    if (!end)
        return std::unexpected(end.error());
    return CertifiedIndex{backing, begin, *end, std::move(sink).finish()};
}

// Standalone Amount payload (no STObject header). One synthetic Amount frame.
inline std::expected<CertifiedIndex, CodecErrorValue>
certify_amount_span(Slice payload, Protocol const& protocol)
{
    if (payload.empty())
    {
        return std::unexpected(CodecErrorValue{
            CodecErrorCode::malformed_data, "empty Amount"});
    }
    size_t const n = AmountRules::extent(payload.data()[0]);
    if (payload.size() != n)
    {
        return std::unexpected(CodecErrorValue{
            CodecErrorCode::malformed_data, "amount extent mismatch"});
    }
    if (char const* e = AmountRules::certify(payload))
    {
        return std::unexpected(
            CodecErrorValue{CodecErrorCode::malformed_data, e});
    }
    auto amt = protocol.find_field("Amount");
    if (!amt)
    {
        return std::unexpected(CodecErrorValue{
            CodecErrorCode::unknown_field, "Amount field missing from protocol"});
    }
    FieldFrame f;
    f.field_code = amt->code;
    f.header_begin = 0;
    f.payload_begin = 0;
    f.wire_end = static_cast<uint32_t>(payload.size());
    std::vector<FieldFrame> frames;
    frames.reserve(1);
    frames.push_back(f);
    return CertifiedIndex{payload, 0, f.wire_end, std::move(frames)};
}

// Owns an immutable copy of the certified bytes plus the frame index.
// AmountView borrows a payload Slice from this object; it does not own,
// copy, or refcount the backing. Native scoped CertifiedIndex remains
// valid for caller-owned Slices.
class CertifiedRoot
{
public:
    CertifiedRoot(CertifiedRoot const&) = delete;
    CertifiedRoot& operator=(CertifiedRoot const&) = delete;
    CertifiedRoot(CertifiedRoot&&) noexcept = default;
    CertifiedRoot& operator=(CertifiedRoot&&) noexcept = default;

    static std::expected<CertifiedRoot, CodecErrorValue>
    copy_and_certify(
        Slice src,
        uint32_t begin,
        Protocol const& protocol)
    {
        std::vector<uint8_t> bytes(src.data(), src.data() + src.size());
        auto idx = certify_indexed(
            Slice{bytes.data(), bytes.size()}, begin, protocol);
        if (!idx)
            return std::unexpected(idx.error());
        return CertifiedRoot{std::move(bytes), std::move(*idx)};
    }

    static std::expected<CertifiedRoot, CodecErrorValue>
    copy_and_certify_amount(Slice src, Protocol const& protocol)
    {
        std::vector<uint8_t> bytes(src.data(), src.data() + src.size());
        auto idx =
            certify_amount_span(Slice{bytes.data(), bytes.size()}, protocol);
        if (!idx)
            return std::unexpected(idx.error());
        return CertifiedRoot{std::move(bytes), std::move(*idx)};
    }

    size_t
    frame_count() const noexcept
    {
        return index_.frame_count();
    }

private:
    CertifiedRoot() = default;

    CertifiedRoot(std::vector<uint8_t> bytes, CertifiedIndex idx)
        : bytes_(std::move(bytes))
        , index_(std::move(idx))
    {
    }

    friend class CertifiedObject;

    // Private: copy_and_certify(...)->index() would expose a non-owning index
    // into a dying expected. CertifiedObject consumes views synchronously.
    CertifiedIndex const&
    index() const& noexcept
    {
        return index_;
    }

    CertifiedIndex const&
    index() const&& = delete;

    std::vector<uint8_t> bytes_;
    CertifiedIndex index_;
};

}  // namespace catl::xdata
