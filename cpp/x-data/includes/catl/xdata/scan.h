#pragma once

#include "catl/xdata/codec-error.h"
#include "catl/xdata/protocol.h"
#include "catl/xdata/slice-cursor.h"
#include "catl/xdata/types.h"
#include "catl/xdata/types/amount.h"
#include "catl/xdata/types/issue.h"
#include "catl/xdata/types/pathset.h"

#include <cstdint>
#include <expected>
#include <vector>

namespace catl::xdata {

enum class ScanMode { Locate, CertifyWire };

struct FieldFrame
{
    uint32_t field_code = 0;
    uint32_t header_begin = 0;
    uint32_t payload_begin = 0;
    uint32_t wire_end = 0;
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
    std::vector<FieldFrame> frames;
    void
    emit(FieldFrame const& f)
    {
        frames.push_back(f);
    }
};

namespace scan_detail {

// xahaud-vectors:src/libxrpl/protocol/STObject.cpp:60
// xahaud-vectors:src/libxrpl/protocol/STVar.cpp:117
inline constexpr int kMaxScanDepth = 10;

inline CodecErrorValue
err(std::string msg)
{
    return {CodecErrorCode::malformed_data, std::move(msg)};
}

inline bool
fits_u32(size_t n)
{
    return n <= 0xffffffffu;
}

inline uint64_t
read_be64(uint8_t const* p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v = (v << 8) | p[i];
    return v;
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

inline std::expected<size_t, CodecErrorValue>
try_issue_size(SliceCursor const& cursor)
{
    if (cursor.remaining_size() < 20)
        return std::unexpected(err("truncated Issue"));
    uint8_t const* first20 = cursor.data.data() + cursor.pos;
    if (is_xrp_currency(first20))
        return size_t{20};
    if (cursor.remaining_size() < 40)
        return std::unexpected(err("truncated Issue issuer"));
    uint8_t const* second20 = first20 + 20;
    if (is_no_account(second20))
    {
        if (cursor.remaining_size() < 44)
            return std::unexpected(err("truncated MPT Issue"));
        return size_t{44};
    }
    return size_t{40};
}

inline std::expected<void, CodecErrorValue>
certify_issue(Slice payload)
{
    // xahaud-vectors:src/libxrpl/protocol/STIssue.cpp:77
    // xahaud-vectors:src/libxrpl/protocol/Issue.cpp:67
    if (payload.size() == 20)
    {
        if (!is_xrp_currency(payload.data()))
            return std::unexpected(err("invalid Issue size"));
        return {};
    }
    if (payload.size() == 44)
    {
        if (payload.size() < 40 || !is_no_account(payload.data() + 20))
            return std::unexpected(err("invalid MPT Issue"));
        return {};
    }
    if (payload.size() != 40)
        return std::unexpected(err("invalid Issue size"));
    bool const native_currency = is_xrp_currency(payload.data());
    bool const native_account = all_zero20(payload.data() + 20);
    if (native_currency != native_account)
        return std::unexpected(
            err("invalid issue: currency and account native mismatch"));
    return {};
}

inline std::expected<void, CodecErrorValue>
certify_amount(Slice payload)
{
    if (payload.size() < 8)
        return std::unexpected(err("truncated Amount"));
    uint64_t const value = read_be64(payload.data());
    constexpr uint64_t kIssued = 0x8000000000000000ull;
    constexpr uint64_t kPositive = 0x4000000000000000ull;
    constexpr uint64_t kMpt = 0x2000000000000000ull;
    constexpr uint64_t kValueMask = ~(kPositive | kMpt);
    constexpr uint64_t kMinMant = 1000000000000000ull;
    constexpr uint64_t kMaxMant = 9999999999999999ull;

    // xahaud-vectors:src/libxrpl/protocol/STAmount.cpp:117
    if ((value & kIssued) == 0)
    {
        if ((value & kMpt) != 0)
        {
            if (payload.size() != 33)
                return std::unexpected(err("MPT Amount size"));
            return {};
        }
        if (payload.size() != 8)
            return std::unexpected(err("native Amount size"));
        if ((value & kPositive) != 0)
            return {};
        // xahaud-vectors:src/libxrpl/protocol/STAmount.cpp:140
        if ((value & kValueMask) == 0)
            return std::unexpected(err("negative zero is not canonical"));
        return {};
    }

    if (payload.size() != 48)
        return std::unexpected(err("IOU Amount size"));
    uint8_t const* currency = payload.data() + 8;
    uint8_t const* account = payload.data() + 28;
    // xahaud-vectors:src/libxrpl/protocol/STAmount.cpp:153
    if (all_zero20(currency))
        return std::unexpected(err("invalid native currency"));
    // xahaud-vectors:src/libxrpl/protocol/STAmount.cpp:158
    if (all_zero20(account))
        return std::unexpected(err("invalid native account"));

    int offset = static_cast<int>(value >> (64 - 10));
    uint64_t mant = value & ~(1023ull << (64 - 10));
    if (mant)
    {
        offset = (offset & 255) - 97;
        if (mant < kMinMant || mant > kMaxMant || offset < -96 || offset > 80)
            return std::unexpected(err("invalid currency value"));
        return {};
    }
    if (offset != 512)
        return std::unexpected(err("invalid currency value"));
    return {};
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

inline std::expected<uint32_t, CodecErrorValue>
scan_pathset(SliceCursor& cursor, ScanMode mode)
{
    bool saw_hop = false;
    bool empty_path = true;
    while (!cursor.empty())
    {
        auto t = cursor.try_read_u8();
        if (!t)
            return std::unexpected(t.error());
        if (*t == PathSet::END_BYTE)
        {
            if (mode == ScanMode::CertifyWire && !saw_hop)
                return std::unexpected(err("empty path"));
            if (mode == ScanMode::CertifyWire && empty_path && saw_hop)
                return std::unexpected(err("empty path"));
            if (!fits_u32(cursor.pos))
                return std::unexpected(err("offset overflow"));
            return static_cast<uint32_t>(cursor.pos);
        }
        if (*t == PathSet::PATH_SEPARATOR)
        {
            if (mode == ScanMode::CertifyWire && empty_path)
                return std::unexpected(err("empty path"));
            empty_path = true;
            continue;
        }
        unsigned extra = 0;
        if (*t & PathSet::TYPE_ACCOUNT)
            extra += 20;
        if (*t & PathSet::TYPE_CURRENCY)
            extra += 20;
        if (*t & PathSet::TYPE_ISSUER)
            extra += 20;
        if (mode == ScanMode::CertifyWire)
        {
            uint8_t const legal = PathSet::TYPE_ACCOUNT | PathSet::TYPE_CURRENCY |
                PathSet::TYPE_ISSUER;
            if ((*t & ~legal) != 0)
                return std::unexpected(err("unknown PathSet type bits"));
            if (extra == 0)
                return std::unexpected(err("empty PathSet hop"));
        }
        auto adv = cursor.try_advance(extra);
        if (!adv)
            return std::unexpected(adv.error());
        saw_hop = true;
        empty_path = false;
    }
    // xahaud-vectors:src/libxrpl/protocol/STPathSet.cpp:57
    // xahaud-vectors:src/libxrpl/protocol/Serializer.cpp:342
    return std::unexpected(err("truncated PathSet"));
}

inline std::expected<void, CodecErrorValue>
skip_xchain_bridge(SliceCursor& cursor, ScanMode mode)
{
    // xahaud-vectors:src/libxrpl/protocol/STXChainBridge.cpp:139
    for (int door = 0; door < 2; ++door)
    {
        auto len = try_read_vl_length(cursor);
        if (!len)
            return std::unexpected(len.error());
        if (mode == ScanMode::CertifyWire && *len != 0 && *len != 20)
        {
            // xahaud-vectors:src/libxrpl/protocol/STAccount.cpp:45
            return std::unexpected(err("Invalid STAccount size"));
        }
        auto adv = cursor.try_advance(*len);
        if (!adv)
            return std::unexpected(adv.error());
        auto n = try_issue_size(cursor);
        if (!n)
            return std::unexpected(n.error());
        if (mode == ScanMode::CertifyWire)
        {
            Slice issue{cursor.data.data() + cursor.pos, *n};
            auto c = certify_issue(issue);
            if (!c)
                return std::unexpected(c.error());
        }
        auto adv2 = cursor.try_advance(*n);
        if (!adv2)
            return std::unexpected(adv2.error());
    }
    return {};
}

enum class FieldScope { Object, Array };
enum class Admit { Nop, EndScope, Field };

struct AdmitResult
{
    Admit kind = Admit::Field;
    FieldDef const* field = nullptr;
};

inline std::expected<AdmitResult, CodecErrorValue>
admit_field(
    FieldScope scope,
    uint32_t field_code,
    Protocol const& protocol)
{
    uint16_t const type = get_field_type_code(field_code);
    uint16_t const nth = get_field_id(field_code);
    if (is_nop(type, nth))
        return AdmitResult{Admit::Nop, nullptr};

    bool const obj_end = type == FieldTypes::STObject.code && nth == 1;
    bool const arr_end = type == FieldTypes::STArray.code && nth == 1;

    if (scope == FieldScope::Array)
    {
        // xahaud-vectors:src/libxrpl/protocol/STArray.cpp:79
        if (arr_end)
            return AdmitResult{Admit::EndScope, nullptr};
        // xahaud-vectors:src/libxrpl/protocol/STArray.cpp:82
        if (obj_end)
            return std::unexpected(err("Illegal terminator in array"));
        FieldDef const* field = protocol.get_field_by_code(field_code);
        if (!field)
            return std::unexpected(err("Unknown field - In Array"));
        if (field->meta.type != FieldTypes::STObject)
            return std::unexpected(err("Non-object in array"));
        return AdmitResult{Admit::Field, field};
    }

    // xahaud-vectors:src/libxrpl/protocol/STObject.cpp:235
    if (obj_end)
        return AdmitResult{Admit::EndScope, nullptr};
    if (arr_end)
        return std::unexpected(err("Illegal end-of-array marker in object"));
    FieldDef const* field = protocol.get_field_by_code(field_code);
    bool const inferred_vl = !field && protocol.is_inferred_vl_type(type);
    if (!field && !inferred_vl)
        return std::unexpected(
            err("Unknown field code: " + std::to_string(field_code)));
    return AdmitResult{Admit::Field, field};
}

template <ScanMode M, class Sink>
std::expected<uint32_t, CodecErrorValue>
scan_object(
    SliceCursor& cursor,
    Protocol const& protocol,
    Sink& sink,
    int depth,
    bool top_level);

template <ScanMode M, class Sink>
std::expected<uint32_t, CodecErrorValue>
scan_array(
    SliceCursor& cursor,
    Protocol const& protocol,
    Sink& sink,
    int depth)
{
    if (depth > kMaxScanDepth)
        return std::unexpected(err("nesting exceeds maximum depth"));
    int nop_count = 0;
    while (!cursor.empty())
    {
        size_t const header_begin = cursor.pos;
        auto hdr = try_read_field_header(cursor);
        if (!hdr)
            return std::unexpected(hdr.error());
        auto const field_code = hdr->second;
        if (field_code == 0)
            break;
        auto admitted = admit_field(FieldScope::Array, field_code, protocol);
        if (!admitted)
            return std::unexpected(admitted.error());
        if (admitted->kind == Admit::Nop)
        {
            if (nop_overflows(nop_count))
                return std::unexpected(err("Too many NOPS"));
            continue;
        }
        if (admitted->kind == Admit::EndScope)
        {
            if (!fits_u32(cursor.pos))
                return std::unexpected(err("offset overflow"));
            return static_cast<uint32_t>(cursor.pos);
        }
        size_t const payload_begin = cursor.pos;
        auto inner = scan_object<M, Sink>(cursor, protocol, sink, depth + 1, false);
        if (!inner)
            return inner;
        size_t const wire_end = cursor.pos;
        if (!fits_u32(header_begin) || !fits_u32(payload_begin) ||
            !fits_u32(wire_end))
            return std::unexpected(err("offset overflow"));
        if constexpr (Sink::kRecords)
        {
            sink.emit(FieldFrame{
                field_code,
                static_cast<uint32_t>(header_begin),
                static_cast<uint32_t>(payload_begin),
                static_cast<uint32_t>(wire_end)});
        }
    }
    if (!fits_u32(cursor.pos))
        return std::unexpected(err("offset overflow"));
    return static_cast<uint32_t>(cursor.pos);
}

template <ScanMode M, class Sink>
std::expected<uint32_t, CodecErrorValue>
scan_object(
    SliceCursor& cursor,
    Protocol const& protocol,
    Sink& sink,
    int depth,
    bool top_level)
{
    if (depth > kMaxScanDepth)
        return std::unexpected(err("nesting exceeds maximum depth"));
    (void)top_level;

    int nop_count = 0;
    DupTracker<M == ScanMode::CertifyWire> seen;

    while (!cursor.empty())
    {
        size_t const header_begin = cursor.pos;
        auto hdr = try_read_field_header(cursor);
        if (!hdr)
            return std::unexpected(hdr.error());
        uint32_t const field_code = hdr->second;
        if (field_code == 0)
            break;

        auto admitted = admit_field(FieldScope::Object, field_code, protocol);
        if (!admitted)
            return std::unexpected(admitted.error());
        if (admitted->kind == Admit::Nop)
        {
            if (nop_overflows(nop_count))
                return std::unexpected(err("Too many NOPS"));
            continue;
        }
        if (admitted->kind == Admit::EndScope)
        {
            if (!fits_u32(cursor.pos))
                return std::unexpected(err("offset overflow"));
            break;
        }

        FieldDef const* field = admitted->field;
        uint16_t const type = get_field_type_code(field_code);
        bool const inferred_vl =
            !field && protocol.is_inferred_vl_type(type);
        bool const vl = field ? field->meta.is_vl_encoded : inferred_vl;
        size_t payload_begin = cursor.pos;

        if (vl)
        {
            auto len = try_read_vl_length(cursor);
            if (!len)
                return std::unexpected(len.error());
            payload_begin = cursor.pos;
            if constexpr (M == ScanMode::CertifyWire)
            {
                if (type == FieldTypes::AccountID.code &&
                    *len != 0 && *len != 20)
                {
                    // xahaud-vectors:src/libxrpl/protocol/STAccount.cpp:38
                    // xahaud-vectors:src/libxrpl/protocol/STAccount.cpp:45
                    return std::unexpected(err("Invalid STAccount size"));
                }
            }
            auto adv = cursor.try_advance(*len);
            if (!adv)
                return std::unexpected(adv.error());
        }
        else if (type == FieldTypes::Amount.code)
        {
            auto first = cursor.try_peek_u8();
            if (!first)
                return std::unexpected(first.error());
            size_t const n = get_amount_size(*first);
            if (cursor.remaining_size() < n)
                return std::unexpected(err("truncated Amount"));
            Slice payload{cursor.data.data() + cursor.pos, n};
            if constexpr (M == ScanMode::CertifyWire)
            {
                auto c = certify_amount(payload);
                if (!c)
                    return std::unexpected(c.error());
            }
            auto adv = cursor.try_advance(n);
            if (!adv)
                return std::unexpected(adv.error());
        }
        else if (type == FieldTypes::PathSet.code)
        {
            auto end = scan_pathset(cursor, M);
            if (!end)
                return std::unexpected(end.error());
        }
        else if (type == FieldTypes::STObject.code)
        {
            auto inner =
                scan_object<M, Sink>(cursor, protocol, sink, depth + 1, false);
            if (!inner)
                return inner;
        }
        else if (type == FieldTypes::STArray.code)
        {
            auto inner = scan_array<M, Sink>(cursor, protocol, sink, depth + 1);
            if (!inner)
                return inner;
        }
        else if (type == FieldTypes::Issue.code)
        {
            auto n = try_issue_size(cursor);
            if (!n)
                return std::unexpected(n.error());
            if constexpr (M == ScanMode::CertifyWire)
            {
                Slice issue{cursor.data.data() + cursor.pos, *n};
                auto c = certify_issue(issue);
                if (!c)
                    return std::unexpected(c.error());
            }
            auto adv = cursor.try_advance(*n);
            if (!adv)
                return std::unexpected(adv.error());
        }
        else if (type == FieldTypes::XChainBridge.code)
        {
            auto skip = skip_xchain_bridge(cursor, M);
            if (!skip)
                return std::unexpected(skip.error());
        }
        else
        {
            size_t fixed = field ? field->meta.type.fixed_size : 0;
            if (fixed == 0)
                return std::unexpected(err("Unknown field type size"));
            auto adv = cursor.try_advance(fixed);
            if (!adv)
                return std::unexpected(adv.error());
        }

        size_t const wire_end = cursor.pos;
        if (!fits_u32(header_begin) || !fits_u32(payload_begin) ||
            !fits_u32(wire_end))
            return std::unexpected(err("offset overflow"));

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
        if (seen.has_duplicate())
            return std::unexpected(err("Duplicate field detected"));
    }

    if (!fits_u32(cursor.pos))
        return std::unexpected(err("offset overflow"));
    return static_cast<uint32_t>(cursor.pos);
}

}  // namespace scan_detail

template <ScanMode M, class Sink>
std::expected<uint32_t, CodecErrorValue>
scan_scope(
    Slice backing,
    uint32_t begin,
    Protocol const& protocol,
    Sink& sink)
{
    if (begin > backing.size())
        return std::unexpected(scan_detail::err("begin past end"));
    SliceCursor cursor{backing, begin};
    return scan_detail::scan_object<M, Sink>(
        cursor, protocol, sink, 0, true);
}

}  // namespace catl::xdata
