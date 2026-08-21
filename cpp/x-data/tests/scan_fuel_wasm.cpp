// No-exceptions wasm fuel probe. JsonVisitor cannot link under wasi-sdk
// (libc++abi has no __cxa_throw). This uses the shipped parse_with_visitor
// path and materializes every leaf with shipped decode_raw / AccountID
// decode_string_expected / Amount certify — not a skip visitor.
//
// Amount view/raw/mask loops call scan_fuel_once.cpp (separate TU, -O2).
// Export view_once_c, raw_once_c, mask_once_c. IOU 48-byte payload.

#include "catl/xdata/amount-view.h"
#include "catl/xdata/certified-index.h"
#include "scan_fuel_once.h"
#include "catl/xdata/codecs/account_id.h"
#include "catl/xdata/codecs/int.h"
#include "catl/xdata/codecs/uint.h"
#include "catl/xdata/parser-context.h"
#include "catl/xdata/parser.h"
#include "catl/xdata/protocol.h"
#include "catl/xdata/scan.h"
#include "catl/xdata/slice-visitor.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct MaterializeVisitor
{
    uint64_t acc = 0;
    std::vector<std::string> owned;

    bool
    visit_object_start(catl::xdata::FieldPath const&, catl::xdata::FieldSlice const&)
    {
        return true;
    }
    void
    visit_object_end(catl::xdata::FieldPath const&, catl::xdata::FieldSlice const&)
    {
    }
    bool
    visit_array_start(catl::xdata::FieldPath const&, catl::xdata::FieldSlice const&)
    {
        return true;
    }
    void
    visit_array_end(catl::xdata::FieldPath const&, catl::xdata::FieldSlice const&)
    {
    }
    void
    visit_field(catl::xdata::FieldPath const&, catl::xdata::FieldSlice const& fs)
    {
        using namespace catl::xdata;
        auto const& t = fs.get_field().meta.type;
        Slice const d = fs.data;
        if (t == FieldTypes::UInt8)
            acc += codecs::UInt8Codec::decode_raw(d);
        else if (t == FieldTypes::UInt16)
            acc += codecs::UInt16Codec::decode_raw(d);
        else if (t == FieldTypes::UInt32)
            acc += codecs::UInt32Codec::decode_raw(d);
        else if (t == FieldTypes::Int32)
            acc += static_cast<uint64_t>(codecs::Int32Codec::decode_raw(d));
        else if (t == FieldTypes::AccountID)
        {
            auto s = codecs::AccountIDCodec::decode_string_expected(d);
            if (s)
            {
                owned.push_back(std::move(*s));
                acc += owned.back().size();
            }
        }
        else if (t == FieldTypes::Amount)
        {
            (void)catl::xdata::AmountRules::certify(d);
            if (d.size() >= 8)
            {
                uint64_t v = 0;
                for (int i = 0; i < 8; ++i)
                    v = (v << 8) | d.data()[i];
                acc += v;
            }
            owned.emplace_back(
                reinterpret_cast<char const*>(d.data()), d.size());
        }
        else
        {
            owned.emplace_back(
                reinterpret_cast<char const*>(d.data()), d.size());
            for (size_t i = 0; i < d.size(); ++i)
                acc += d.data()[i];
        }
    }
};

constexpr uint8_t kBlob[] = {
    0x81, 0x14, 0xB5, 0xF7, 0x62, 0x79, 0x8A, 0x53, 0xD5, 0x43, 0xA0, 0x14,
    0xCA, 0xF8, 0xB2, 0x97, 0xCF, 0xF8, 0xF2, 0xF9, 0x37, 0xE8, 0xF9, 0xEA,
    0x7D, 0x02, 0xDE, 0xAD, 0xE1, 0xF1};

// Corpus stobject-iou-zero: Amount 48-byte IOU + Account. Payload starts at 1.
constexpr uint8_t kIouObj[] = {
    0x61, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x55, 0x53, 0x44,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xb5, 0xf7, 0x62, 0x79, 0x8a, 0x53, 0xd5,
    0x43, 0xa0, 0x14, 0xca, 0xf8, 0xb2, 0x97, 0xcf, 0xf8, 0xf2, 0xf9, 0x37,
    0xe8, 0x81, 0x14, 0xb5, 0xf7, 0x62, 0x79, 0x8a, 0x53, 0xd5, 0x43, 0xa0,
    0x14, 0xca, 0xf8, 0xb2, 0x97, 0xcf, 0xf8, 0xf2, 0xf9, 0x37, 0xe8};

constexpr uint8_t kIssuer[20] = {
    0xb5, 0xf7, 0x62, 0x79, 0x8a, 0x53, 0xd5, 0x43, 0xa0, 0x14,
    0xca, 0xf8, 0xb2, 0x97, 0xcf, 0xf8, 0xf2, 0xf9, 0x37, 0xe8};

void
write_iou(uint8_t* p, uint64_t mant, int32_t exp, uint8_t cur_tag)
{
    using catl::xdata::AmountRules;
    uint64_t const word = AmountRules::kIssued | AmountRules::kPositive |
        (static_cast<uint64_t>(exp + 97) << 54) | mant;
    for (int i = 0; i < 8; ++i)
        p[i] = static_cast<uint8_t>(word >> (56 - 8 * i));
    std::memset(p + 8, 0, 20);
    p[8 + 12] = 'U';
    p[8 + 13] = 'S';
    p[8 + 14] = 'D';
    p[8 + 19] = cur_tag;
    std::memcpy(p + 28, kIssuer, 20);
}

struct IouExpect
{
    uint64_t mant;
    int32_t exp;
    uint8_t tag;
};

IouExpect
iou_for_iter(int i)
{
    return IouExpect{
        catl::xdata::AmountRules::kMinMant + static_cast<uint64_t>(i % 9000),
        static_cast<int32_t>(i % 5) - 2,
        static_cast<uint8_t>(1 + (i % 200))};
}

}  // namespace

int
main(int argc, char** argv)
{
    using namespace catl::xdata;
    auto const protocol = Protocol::load_embedded_xahau_protocol();
    Slice backing{kBlob, sizeof(kBlob)};
    char const* which = argc > 1 ? argv[1] : "all";
    int kIters = 2000;
    if (argc > 2)
        kIters = std::atoi(argv[2]);
    if (kIters < 0)
        kIters = 0;
    uint64_t sink = 0;

    auto loc = [&] {
        for (int i = 0; i < kIters; ++i)
        {
            NullSink s;
            auto r = scan_scope<ScanMode::Locate>(backing, 0, protocol, s);
            if (r)
                sink += *r;
        }
        std::puts("locate_no_index");
    };
    auto cert = [&] {
        for (int i = 0; i < kIters; ++i)
        {
            NullSink s;
            auto r = scan_scope<ScanMode::CertifyWire>(backing, 0, protocol, s);
            if (r)
                sink += *r;
        }
        std::puts("certify_no_index");
    };
    auto idx = [&] {
        for (int i = 0; i < kIters; ++i)
        {
            IndexSink s;
            auto r = scan_scope<ScanMode::CertifyWire>(backing, 0, protocol, s);
            if (r)
                sink += *r + s.frames.size();
        }
        std::puts("certify_index");
    };
    auto eager = [&] {
        for (int i = 0; i < kIters; ++i)
        {
            ParserContext ctx{backing};
            MaterializeVisitor visitor;
            parse_with_visitor(ctx, protocol, visitor);
            sink += visitor.acc + visitor.owned.size();
        }
        std::puts("full_eager_decode");
    };
    auto view_repeat = [&] {
        uint8_t obj[sizeof(kIouObj)];
        std::memcpy(obj, kIouObj, sizeof(obj));
        auto certified = certify_indexed(Slice{obj, sizeof(obj)}, 0, protocol);
        if (!certified)
        {
            std::puts("FAIL certify_indexed");
            return;
        }
        size_t amt = certified->frame_count();
        uint32_t payload_begin = 0;
        for (size_t i = 0; i < certified->frame_count(); ++i)
        {
            auto const& fr = certified->frame(i);
            auto const* f = protocol.get_field_by_code(fr.field_code);
            if (f && f->meta.type == FieldTypes::Amount)
            {
                amt = i;
                payload_begin = fr.payload_begin;
                break;
            }
        }
        for (int i = 0; i < kIters; ++i)
        {
            auto const e = iou_for_iter(i);
            write_iou(obj + payload_begin, e.mant, e.exp, e.tag);
            int32_t got_exp = 0;
            uint8_t got_tag = 0;
            uint64_t const m =
                view_once_c(&*certified, amt, &got_exp, &got_tag);
            if (m != e.mant || got_exp != e.exp || got_tag != e.tag)
            {
                std::puts("FAIL view iou");
                return;
            }
            sink += m + static_cast<uint64_t>(got_exp) + got_tag;
        }
        std::puts("amount_view_repeat");
    };
    auto raw_repeat = [&] {
        uint8_t buf[48]{};
        for (int i = 0; i < kIters; ++i)
        {
            auto const e = iou_for_iter(i);
            write_iou(buf, e.mant, e.exp, e.tag);
            int32_t got_exp = 0;
            uint8_t got_tag = 0;
            uint64_t const m = raw_once_c(buf, sizeof(buf), &got_exp, &got_tag);
            if (m != e.mant || got_exp != e.exp || got_tag != e.tag)
            {
                std::puts("FAIL raw iou");
                return;
            }
            sink += m + static_cast<uint64_t>(got_exp) + got_tag;
        }
        std::puts("amount_raw_recertify_repeat");
    };
    auto mask_repeat = [&] {
        uint8_t buf[48]{};
        for (int i = 0; i < kIters; ++i)
        {
            auto const e = iou_for_iter(i);
            write_iou(buf, e.mant, e.exp, e.tag);
            uint8_t got_tag = 0;
            uint64_t const m = mask_once_c(buf, sizeof(buf), &got_tag);
            if (m != e.mant || got_tag != e.tag)
            {
                std::puts("FAIL mask iou");
                return;
            }
            sink += m + got_tag;
        }
        std::puts("amount_mask_only_repeat");
    };

    if (which[0] == '0' || std::strcmp(which, "locate_no_index") == 0)
        loc();
    else if (which[0] == '1' || std::strcmp(which, "certify_no_index") == 0)
        cert();
    else if (which[0] == '2' || std::strcmp(which, "certify_index") == 0)
        idx();
    else if (which[0] == '3' || std::strcmp(which, "full_eager_decode") == 0)
        eager();
    else if (which[0] == '4' || std::strcmp(which, "amount_view_repeat") == 0)
        view_repeat();
    else if (which[0] == '5' || std::strcmp(which, "amount_raw_recertify_repeat") == 0)
        raw_repeat();
    else if (which[0] == '6' || std::strcmp(which, "amount_mask_only_repeat") == 0)
        mask_repeat();
    else
    {
        loc();
        cert();
        idx();
        eager();
        view_repeat();
        raw_repeat();
        mask_repeat();
    }
    if (sink == 0xffffffffffffull)
        std::puts("never");
    return 0;
}
