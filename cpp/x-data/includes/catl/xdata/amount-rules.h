#pragma once

#include "catl/core/types.h"

#include <cstdint>

namespace catl::xdata {

// One no-throw authority for Amount extent, CertifyWire representation,
// and raw part extraction. The scanner calls these statics without
// constructing a view. AmountView accessors reuse the same parts().
struct AmountRules
{
    static constexpr uint64_t kIssued = 0x8000000000000000ull;
    static constexpr uint64_t kPositive = 0x4000000000000000ull;
    static constexpr uint64_t kMpt = 0x2000000000000000ull;
    static constexpr uint64_t kValueMask = ~(kPositive | kMpt);
    static constexpr uint64_t kMinMant = 1000000000000000ull;
    static constexpr uint64_t kMaxMant = 9999999999999999ull;

    enum class Kind : uint8_t
    {
        Native,
        Iou,
        Mpt
    };

    struct Parts
    {
        Kind kind = Kind::Native;
        bool negative = false;
        bool zero = false;
        uint64_t magnitude = 0;
        int32_t exponent = 0;
        Slice currency{};
        Slice issuer{};
        Slice mpt_id{};
    };

    static size_t
    extent(uint8_t first_byte) noexcept
    {
        if (first_byte & 0x80)
            return 48;
        if (first_byte & 0x20)
            return 33;
        return 8;
    }

    // First-byte kind only. Does not decode mantissa, currency, or issuer.
    static Kind
    kind(Slice payload) noexcept
    {
        if (payload.empty())
            return Kind::Native;
        uint8_t const first = payload.data()[0];
        if (first & 0x80)
            return Kind::Iou;
        if (first & 0x20)
            return Kind::Mpt;
        return Kind::Native;
    }

    static uint64_t
    read_be64(uint8_t const* p) noexcept
    {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v = (v << 8) | p[i];
        return v;
    }

    static bool
    all_zero20(uint8_t const* p) noexcept
    {
        for (int i = 0; i < 20; ++i)
        {
            if (p[i] != 0)
                return false;
        }
        return true;
    }

    // nullptr means representation-valid. Does not throw.
    // xahaud-vectors:src/libxrpl/protocol/STAmount.cpp:117
    static char const*
    certify(Slice payload) noexcept
    {
        if (payload.size() < 8)
            return "truncated Amount";
        uint64_t const value = read_be64(payload.data());

        if ((value & kIssued) == 0)
        {
            if ((value & kMpt) != 0)
                return payload.size() != 33 ? "MPT Amount size" : nullptr;
            if (payload.size() != 8)
                return "native Amount size";
            if ((value & kPositive) != 0)
                return nullptr;
            // xahaud-vectors:src/libxrpl/protocol/STAmount.cpp:140
            if ((value & kValueMask) == 0)
                return "negative zero is not canonical";
            return nullptr;
        }

        if (payload.size() != 48)
            return "IOU Amount size";
        uint8_t const* currency = payload.data() + 8;
        uint8_t const* account = payload.data() + 28;
        // xahaud-vectors:src/libxrpl/protocol/STAmount.cpp:153
        if (all_zero20(currency))
            return "invalid native currency";
        // xahaud-vectors:src/libxrpl/protocol/STAmount.cpp:158
        if (all_zero20(account))
            return "invalid native account";

        int offset = static_cast<int>(value >> (64 - 10));
        uint64_t mant = value & ~(1023ull << (64 - 10));
        if (mant)
        {
            offset = (offset & 255) - 97;
            if (mant < kMinMant || mant > kMaxMant || offset < -96 ||
                offset > 80)
                return "invalid currency value";
            return nullptr;
        }
        return offset != 512 ? "invalid currency value" : nullptr;
    }

    // Total after a successful certify(). Does not recertify.
    static Parts
    parts(Slice payload) noexcept
    {
        Parts p;
        if (payload.size() < 8)
            return p;
        uint8_t const first = payload.data()[0];
        uint64_t const value = read_be64(payload.data());
        if ((first & 0x80) != 0)
        {
            p.kind = Kind::Iou;
            p.currency = Slice{
                payload.data() + 8, payload.size() >= 28 ? size_t{20} : size_t{0}};
            p.issuer = Slice{
                payload.data() + 28, payload.size() >= 48 ? size_t{20} : size_t{0}};
            int offset = static_cast<int>(value >> (64 - 10));
            uint64_t mant = value & ~(1023ull << (64 - 10));
            if (!mant)
            {
                p.zero = true;
                p.negative = false;
                p.magnitude = 0;
                p.exponent = 0;
                return p;
            }
            p.negative = (offset & 256) == 0;
            p.exponent = (offset & 255) - 97;
            p.magnitude = mant;
            return p;
        }
        if ((first & 0x20) != 0)
        {
            p.kind = Kind::Mpt;
            p.negative = (value & kPositive) == 0;
            if (payload.size() >= 9)
            {
                p.magnitude = 0;
                for (int i = 1; i < 9; ++i)
                    p.magnitude = (p.magnitude << 8) | payload.data()[i];
            }
            if (payload.size() >= 33)
                p.mpt_id = Slice{payload.data() + 9, 24};
            return p;
        }
        p.kind = Kind::Native;
        p.negative = (value & kPositive) == 0;
        p.magnitude = value & kValueMask;
        p.zero = p.magnitude == 0;
        return p;
    }
};

}  // namespace catl::xdata
