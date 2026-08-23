#pragma once

#include "catl/xdata/amount-rules.h"
#include "catl/xdata/codec-error.h"
#include "catl/xdata/codecs/account_id.h"
#include "catl/xdata/pathset-rules.h"
#include "catl/xdata/parser-context.h"
#include "catl/xdata/protocol.h"
#include "catl/xdata/types.h"
#include "catl/xdata/types/issue.h"
#include "catl/xdata/types/pathset.h"

#include <array>
#include <cstdint>
#include <expected>
#include <utility>
#include <vector>

namespace catl::xdata {

enum class ScanMode { Locate, CertifyWire };

struct FieldFrame
{
    uint32_t field_code;
    uint32_t header_begin;
    uint32_t payload_begin;
    uint32_t wire_end;
};

struct NullSink
{
    static constexpr bool kRecords = false;
    void
    emit(FieldFrame const&) const noexcept
    {
    }
};

struct IndexSink
{
    static constexpr bool kRecords = true;

    void
    emit(FieldFrame const& f)
    {
        if (spill_.empty() && inline_size_ < inline_.size())
        {
            inline_[inline_size_++] = f;
            return;
        }
        if (spill_.empty())
        {
            spill_.reserve(2 * inline_.size());
            spill_.insert(
                spill_.end(), inline_.begin(), inline_.begin() + inline_size_);
        }
        spill_.push_back(f);
    }

    size_t
    size() const noexcept
    {
        return spill_.empty() ? inline_size_ : spill_.size();
    }

    // Successful certification keeps exactly-sized storage for the common
    // 1..8-frame case. Nine or more frames reuse the single spill allocation.
    std::vector<FieldFrame>
    finish() &&
    {
        if (!spill_.empty())
            return std::move(spill_);
        std::vector<FieldFrame> frames;
        frames.reserve(inline_size_);
        frames.insert(
            frames.end(), inline_.begin(), inline_.begin() + inline_size_);
        return frames;
    }

private:
    std::array<FieldFrame, 8> inline_;
    size_t inline_size_ = 0;
    std::vector<FieldFrame> spill_;
};

namespace scan_detail {

// xahaud-vectors:src/libxrpl/protocol/STObject.cpp:60
// xahaud-vectors:src/libxrpl/protocol/STVar.cpp:117
inline constexpr int kMaxScanDepth = 10;

inline bool
fits_u32(size_t n)
{
    return n <= 0xffffffffu;
}

inline bool
all_zero20(uint8_t const* p)
{
    for (int i = 0; i < 20; ++i)
    {
        if (p[i] != 0)
            return false;
    }
    return true;
}

inline bool
is_nop(uint16_t type, uint16_t nth) noexcept
{
    // xahaud-vectors:src/libxrpl/protocol/STObject.cpp:223
    // xahaud-vectors:src/libxrpl/protocol/STArray.cpp:69
    return type == 9 && nth == 9;
}

inline bool
nop_overflows(int& nop_count) noexcept
{
    // xahaud-vectors:src/libxrpl/protocol/STObject.cpp:225
    // xahaud-vectors:src/libxrpl/protocol/STArray.cpp:71
    return ++nop_count == 64;
}

inline size_t
issue_size(ParserContext& ctx)
{
    if (ctx.failed())
        return 0;
    if (ctx.remaining() < 20)
    {
        ctx.fail("truncated Issue");
        return 0;
    }
    uint8_t const* first20 = ctx.at();
    if (is_xrp_currency(first20))
        return 20;
    if (ctx.remaining() < 40)
    {
        ctx.fail("truncated Issue issuer");
        return 0;
    }
    uint8_t const* second20 = first20 + 20;
    if (is_no_account(second20))
    {
        if (ctx.remaining() < 44)
        {
            ctx.fail("truncated MPT Issue");
            return 0;
        }
        return 44;
    }
    return 40;
}

inline char const*
certify_issue(Slice payload)
{
    // xahaud-vectors:src/libxrpl/protocol/STIssue.cpp:77
    // xahaud-vectors:src/libxrpl/protocol/Issue.cpp:67
    if (payload.size() == 20)
        return is_xrp_currency(payload.data()) ? nullptr : "invalid Issue size";
    if (payload.size() == 44)
        return is_no_account(payload.data() + 20) ? nullptr
                                                  : "invalid MPT Issue";
    if (payload.size() != 40)
        return "invalid Issue size";
    bool const native_currency = is_xrp_currency(payload.data());
    bool const native_account = all_zero20(payload.data() + 20);
    if (native_currency != native_account)
        return "invalid issue: currency and account native mismatch";
    return nullptr;
}

template <bool Track>
struct DupTracker;

template <>
struct DupTracker<false>
{
    void
    push(uint32_t) const noexcept
    {
    }
    bool
    has_duplicate() const noexcept
    {
        return false;
    }
};

// Heap-free duplicate set. Locate instantiates DupTracker<false>.
// xahaud-vectors:src/libxrpl/protocol/STObject.cpp:268
template <>
struct DupTracker<true>
{
    uint64_t bits[64]{};
    uint32_t extra[16]{};
    uint8_t extra_n = 0;
    bool dup = false;

    void
    push(uint32_t c) noexcept
    {
        uint16_t const type = get_field_type_code(c);
        uint16_t const nth = get_field_id(c);
        if (type < 32 && nth < 128)
        {
            unsigned const idx = static_cast<unsigned>(type) * 2u + (nth >> 6);
            uint64_t const mask = 1ull << (nth & 63);
            if (bits[idx] & mask)
                dup = true;
            bits[idx] |= mask;
            return;
        }
        for (uint8_t i = 0; i < extra_n; ++i)
        {
            if (extra[i] == c)
            {
                dup = true;
                return;
            }
        }
        if (extra_n < 16)
            extra[extra_n++] = c;
        else
            dup = true;
    }

    bool
    has_duplicate() const noexcept
    {
        return dup;
    }
};

inline void
skip_xchain_bridge(ParserContext& ctx, ScanMode mode)
{
    // xahaud-vectors:src/libxrpl/protocol/STXChainBridge.cpp:139
    for (int door = 0; door < 2 && !ctx.failed(); ++door)
    {
        size_t len = 0;
        if (!ctx.read_vl_length(len))
            return;
        if (mode == ScanMode::CertifyWire &&
            !codecs::AccountIDCodec::valid_vl_payload_size(len))
        {
            // xahaud-vectors:src/libxrpl/protocol/STAccount.cpp:45
            ctx.fail("Invalid STAccount size");
            return;
        }
        if (!ctx.advance(len))
            return;
        size_t n = issue_size(ctx);
        if (ctx.failed())
            return;
        if (mode == ScanMode::CertifyWire)
        {
            if (char const* e = certify_issue(Slice{ctx.at(), n}))
            {
                ctx.fail(e);
                return;
            }
        }
        if (!ctx.advance(n))
            return;
    }
}

enum class FieldScope { Object, Array };
enum class Admit { Nop, EndScope, Field };

struct AdmitResult
{
    Admit kind = Admit::Field;
    FieldDef const* field = nullptr;
};

inline AdmitResult
admit_field(
    ParserContext& ctx,
    FieldScope scope,
    uint32_t field_code,
    Protocol const& protocol)
{
    if (ctx.failed())
        return {};
    uint16_t const type = get_field_type_code(field_code);
    uint16_t const nth = get_field_id(field_code);
    if (is_nop(type, nth))
        return {Admit::Nop, nullptr};

    bool const obj_end = type == FieldTypes::STObject.code && nth == 1;
    bool const arr_end = type == FieldTypes::STArray.code && nth == 1;

    if (scope == FieldScope::Array)
    {
        // xahaud-vectors:src/libxrpl/protocol/STArray.cpp:79
        if (arr_end)
            return {Admit::EndScope, nullptr};
        // xahaud-vectors:src/libxrpl/protocol/STArray.cpp:82
        if (obj_end)
        {
            ctx.fail("Illegal terminator in array");
            return {};
        }
        FieldDef const* field = protocol.get_field_by_code(field_code);
        if (!field)
        {
            ctx.fail("Unknown field - In Array");
            return {};
        }
        if (field->meta.type != FieldTypes::STObject)
        {
            ctx.fail("Non-object in array");
            return {};
        }
        return {Admit::Field, field};
    }

    // xahaud-vectors:src/libxrpl/protocol/STObject.cpp:235
    if (obj_end)
        return {Admit::EndScope, nullptr};
    if (arr_end)
    {
        ctx.fail("Illegal end-of-array marker in object");
        return {};
    }
    FieldDef const* field = protocol.get_field_by_code(field_code);
    bool const inferred_vl = !field && protocol.is_inferred_vl_type(type);
    if (!field && !inferred_vl)
    {
        ctx.fail("Unknown field code: ", field_code, true);
        return {};
    }
    return {Admit::Field, field};
}

template <ScanMode M, class Sink>
void
scan_object(
    ParserContext& ctx,
    Protocol const& protocol,
    Sink& sink,
    int depth,
    bool top_level);

template <ScanMode M, class Sink>
void
scan_array(ParserContext& ctx, Protocol const& protocol, Sink& sink, int depth)
{
    if (ctx.failed())
        return;
    if (depth > kMaxScanDepth)
    {
        ctx.fail("nesting exceeds maximum depth");
        return;
    }
    int nop_count = 0;
    while (!ctx.failed() && !ctx.empty())
    {
        size_t const header_begin = ctx.pos();
        uint32_t const field_code = ctx.read_field_code();
        if (ctx.failed())
            return;
        if (field_code == 0)
            break;
        auto admitted =
            admit_field(ctx, FieldScope::Array, field_code, protocol);
        if (ctx.failed())
            return;
        if (admitted.kind == Admit::Nop)
        {
            if (nop_overflows(nop_count))
            {
                ctx.fail("Too many NOPS");
                return;
            }
            continue;
        }
        if (admitted.kind == Admit::EndScope)
        {
            if (!fits_u32(ctx.pos()))
                ctx.fail("offset overflow");
            return;
        }
        size_t const payload_begin = ctx.pos();
        scan_object<M, Sink>(ctx, protocol, sink, depth + 1, false);
        if (ctx.failed())
            return;
        size_t const wire_end = ctx.pos();
        if (!fits_u32(header_begin) || !fits_u32(payload_begin) ||
            !fits_u32(wire_end))
        {
            ctx.fail("offset overflow");
            return;
        }
        if constexpr (Sink::kRecords)
        {
            sink.emit(FieldFrame{
                field_code,
                static_cast<uint32_t>(header_begin),
                static_cast<uint32_t>(payload_begin),
                static_cast<uint32_t>(wire_end)});
        }
    }
    if (!ctx.failed() && !fits_u32(ctx.pos()))
        ctx.fail("offset overflow");
}

template <ScanMode M, class Sink>
void
scan_object(
    ParserContext& ctx,
    Protocol const& protocol,
    Sink& sink,
    int depth,
    bool top_level)
{
    if (ctx.failed())
        return;
    if (depth > kMaxScanDepth)
    {
        ctx.fail("nesting exceeds maximum depth");
        return;
    }
    (void)top_level;

    int nop_count = 0;
    DupTracker<M == ScanMode::CertifyWire> seen;

    while (!ctx.failed() && !ctx.empty())
    {
        size_t const header_begin = ctx.pos();
        uint32_t const field_code = ctx.read_field_code();
        if (ctx.failed())
            return;
        if (field_code == 0)
            break;

        auto admitted =
            admit_field(ctx, FieldScope::Object, field_code, protocol);
        if (ctx.failed())
            return;
        if (admitted.kind == Admit::Nop)
        {
            if (nop_overflows(nop_count))
            {
                ctx.fail("Too many NOPS");
                return;
            }
            continue;
        }
        if (admitted.kind == Admit::EndScope)
        {
            if (!fits_u32(ctx.pos()))
                ctx.fail("offset overflow");
            break;
        }

        FieldDef const* field = admitted.field;
        uint16_t const type = get_field_type_code(field_code);
        bool const inferred_vl = !field && protocol.is_inferred_vl_type(type);
        bool const vl = field ? field->meta.is_vl_encoded : inferred_vl;
        size_t payload_begin = ctx.pos();

        if (vl)
        {
            size_t len = 0;
            if (!ctx.read_vl_length(len))
                return;
            payload_begin = ctx.pos();
            if constexpr (M == ScanMode::CertifyWire)
            {
                if (type == FieldTypes::AccountID.code &&
                    !codecs::AccountIDCodec::valid_vl_payload_size(len))
                {
                    // xahaud-vectors:src/libxrpl/protocol/STAccount.cpp:38
                    // xahaud-vectors:src/libxrpl/protocol/STAccount.cpp:45
                    ctx.fail("Invalid STAccount size");
                    return;
                }
            }
            if (!ctx.advance(len))
                return;
        }
        else if (type == FieldTypes::Amount.code)
        {
            uint8_t first = 0;
            if (!ctx.peek_u8(first))
                return;
            size_t const n = AmountRules::extent(first);
            if (ctx.remaining() < n)
            {
                ctx.fail("truncated Amount");
                return;
            }
            Slice payload{ctx.at(), n};
            if constexpr (M == ScanMode::CertifyWire)
            {
                if (char const* e = AmountRules::certify(payload))
                {
                    ctx.fail(e);
                    return;
                }
            }
            if (!ctx.advance(n))
                return;
        }
        else if (type == FieldTypes::PathSet.code)
        {
            PathSetNullSink pathset_sink;
            if constexpr (M == ScanMode::Locate)
                PathSetRules::walk<PathSetRuleMode::Locate>(ctx, pathset_sink);
            else
                PathSetRules::walk<PathSetRuleMode::CertifyWire>(
                    ctx, pathset_sink);
            if (ctx.failed())
                return;
        }
        else if (type == FieldTypes::STObject.code)
        {
            scan_object<M, Sink>(ctx, protocol, sink, depth + 1, false);
            if (ctx.failed())
                return;
        }
        else if (type == FieldTypes::STArray.code)
        {
            scan_array<M, Sink>(ctx, protocol, sink, depth + 1);
            if (ctx.failed())
                return;
        }
        else if (type == FieldTypes::Issue.code)
        {
            size_t n = issue_size(ctx);
            if (ctx.failed())
                return;
            if constexpr (M == ScanMode::CertifyWire)
            {
                if (char const* e = certify_issue(Slice{ctx.at(), n}))
                {
                    ctx.fail(e);
                    return;
                }
            }
            if (!ctx.advance(n))
                return;
        }
        else if (type == FieldTypes::XChainBridge.code)
        {
            skip_xchain_bridge(ctx, M);
            if (ctx.failed())
                return;
        }
        else
        {
            size_t fixed = field ? field->meta.type.fixed_size : 0;
            if (fixed == 0)
            {
                ctx.fail("Unknown field type size");
                return;
            }
            if (!ctx.advance(fixed))
                return;
        }

        size_t const wire_end = ctx.pos();
        if (!fits_u32(header_begin) || !fits_u32(payload_begin) ||
            !fits_u32(wire_end))
        {
            ctx.fail("offset overflow");
            return;
        }

        if constexpr (Sink::kRecords)
        {
            sink.emit(FieldFrame{
                field_code,
                static_cast<uint32_t>(header_begin),
                static_cast<uint32_t>(payload_begin),
                static_cast<uint32_t>(wire_end)});
        }
        seen.push(field_code);
    }

    if constexpr (M == ScanMode::CertifyWire)
    {
        if (!ctx.failed() && seen.has_duplicate())
            ctx.fail("Duplicate field detected");
    }

    if (!ctx.failed() && !fits_u32(ctx.pos()))
        ctx.fail("offset overflow");
}

}  // namespace scan_detail

template <ScanMode M, class Sink>
std::expected<uint32_t, CodecErrorValue>
scan_scope(Slice backing, uint32_t begin, Protocol const& protocol, Sink& sink)
{
    if (begin > backing.size())
        return std::unexpected(
            CodecErrorValue{CodecErrorCode::malformed_data, "begin past end"});
    ParserContext ctx{backing};
    ctx.cursor.pos = begin;
    scan_detail::scan_object<M, Sink>(ctx, protocol, sink, 0, true);
    if (ctx.failed())
        return std::unexpected(ctx.as_error());
    return static_cast<uint32_t>(ctx.pos());
}

}  // namespace catl::xdata
