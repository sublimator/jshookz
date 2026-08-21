#pragma once

#include "catl/xdata/amount-rules.h"
#include "catl/xdata/protocol.h"
#include "catl/xdata/scan.h"

#include <cstdint>
#include <expected>
#include <utility>
#include <vector>

namespace catl::xdata {

// Non-owning certificate: one successful CertifyWire+IndexSink pass.
// Frames are only bindable through this object; caller-authored FieldFrame
// values cannot mint a view.
class CertifiedIndex
{
public:
    Slice
    backing() const noexcept
    {
        return backing_;
    }

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

    Protocol const&
    protocol() const noexcept
    {
        return *protocol_;
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
    friend std::expected<CertifiedIndex, CodecErrorValue>
    certify_indexed(Slice, uint32_t, Protocol const&);
    friend std::expected<CertifiedIndex, CodecErrorValue>
    certify_amount_span(Slice, Protocol const&);

    CertifiedIndex(
        Slice backing,
        uint32_t begin,
        uint32_t end,
        Protocol const* protocol,
        std::vector<FieldFrame> frames)
        : backing_(backing)
        , begin_(begin)
        , end_(end)
        , protocol_(protocol)
        , frames_(std::move(frames))
    {
    }

    Slice backing_{};
    uint32_t begin_ = 0;
    uint32_t end_ = 0;
    Protocol const* protocol_ = nullptr;
    std::vector<FieldFrame> frames_;
};

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
    return CertifiedIndex{
        backing, begin, *end, &protocol, std::move(sink.frames)};
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
    frames.push_back(f);
    return CertifiedIndex{
        payload, 0, f.wire_end, &protocol, std::move(frames)};
}

}  // namespace catl::xdata
