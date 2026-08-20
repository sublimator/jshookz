#pragma once

#include "catl/xdata/scan.h"

#include <string>
#include <string_view>
#include <vector>

namespace oracle_run {

inline std::uint8_t
hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
        return static_cast<std::uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f')
        return static_cast<std::uint8_t>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F')
        return static_cast<std::uint8_t>(c - 'A' + 10);
    return 0;
}

inline std::vector<std::uint8_t>
decode_hex(std::string_view hex)
{
    std::vector<std::uint8_t> out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
        out.push_back(static_cast<std::uint8_t>(
            (hex_nibble(hex[i]) << 4) | hex_nibble(hex[i + 1])));
    return out;
}

struct Outcomes
{
    bool locate_ok = false;
    bool certify_null_ok = false;
    bool certify_index_ok = false;
    bool sinks_agree = false;
    bool decode_frames_ok = false;
    std::string locate_err;
    std::string certify_err;
    std::size_t frame_count = 0;
};

inline Outcomes
run_stobject(catl::xdata::Protocol const& protocol, std::string_view hex)
{
    using namespace catl::xdata;
    Outcomes o;
    auto bytes = decode_hex(hex);
    Slice backing{bytes.data(), bytes.size()};

    NullSink locate_sink;
    auto loc = scan_scope<ScanMode::Locate>(backing, 0, protocol, locate_sink);
    o.locate_ok = loc.has_value();
    if (!loc)
        o.locate_err = loc.error().message;

    NullSink null_sink;
    auto cn = scan_scope<ScanMode::CertifyWire>(backing, 0, protocol, null_sink);
    o.certify_null_ok = cn.has_value();
    if (!cn)
        o.certify_err = cn.error().message;

    IndexSink index_sink;
    auto ci = scan_scope<ScanMode::CertifyWire>(backing, 0, protocol, index_sink);
    o.certify_index_ok = ci.has_value();
    o.sinks_agree = o.certify_null_ok == o.certify_index_ok;
    o.frame_count = index_sink.frames.size();

    if (o.certify_index_ok)
    {
        o.decode_frames_ok = true;
        for (auto const& f : index_sink.frames)
        {
            if (f.wire_end < f.payload_begin || f.wire_end > bytes.size())
            {
                o.decode_frames_ok = false;
                break;
            }
            Slice payload{bytes.data() + f.payload_begin, f.wire_end - f.payload_begin};
            auto type = get_field_type_code(f.field_code);
            if (type == FieldTypes::AccountID.code)
            {
                if (payload.size() != 0 && payload.size() != 20)
                    o.decode_frames_ok = false;
            }
            else if (type == FieldTypes::Amount.code)
            {
                if (!scan_detail::certify_amount(payload))
                    o.decode_frames_ok = false;
            }
        }
    }
    return o;
}

inline Outcomes
run_amount(std::string_view hex)
{
    using namespace catl::xdata;
    Outcomes o;
    auto bytes = decode_hex(hex);
    if (bytes.empty())
    {
        o.locate_err = "empty";
        return o;
    }
    size_t n = get_amount_size(bytes[0]);
    o.locate_ok = bytes.size() == n;
    auto c = scan_detail::certify_amount(Slice{bytes.data(), bytes.size()});
    o.certify_null_ok = c.has_value();
    o.certify_index_ok = o.certify_null_ok;
    o.sinks_agree = true;
    o.decode_frames_ok = o.certify_null_ok;
    if (!c)
        o.certify_err = c.error().message;
    if (!o.locate_ok)
        o.locate_err = "amount extent mismatch";
    return o;
}

inline Outcomes
run_pathset(std::string_view hex)
{
    using namespace catl::xdata;
    Outcomes o;
    auto bytes = decode_hex(hex);
    Slice backing{bytes.data(), bytes.size()};
    SliceCursor cur{backing, 0};
    auto loc = scan_detail::scan_pathset(cur, ScanMode::Locate);
    o.locate_ok = loc.has_value();
    if (!loc)
        o.locate_err = loc.error().message;
    SliceCursor cur2{backing, 0};
    auto cert = scan_detail::scan_pathset(cur2, ScanMode::CertifyWire);
    o.certify_null_ok = cert.has_value();
    o.certify_index_ok = o.certify_null_ok;
    o.sinks_agree = true;
    o.decode_frames_ok = o.certify_null_ok;
    if (!cert)
        o.certify_err = cert.error().message;
    return o;
}

}  // namespace oracle_run
