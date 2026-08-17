#pragma once

#include "catl/xdata/exception_policy.h"
#include <charconv>
#include <cstdint>
#include <expected>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace catl::xdata {

enum class CodecErrorCode {
    invalid_value,  // wrong JSON type or unparseable value
    missing_field,  // required sub-field absent (e.g. IOU without issuer)
    out_of_range,   // value exceeds type bounds (e.g. negative native amount)
    invalid_encoding,  // bad hex string, bad base58, wrong length
    unknown_field,     // field name not found in Protocol
    unknown_enum,      // string enum value not found (e.g. bad TransactionType)
    malformed_data,    // binary structure corrupt (decode side)
};

struct CodecErrorValue
{
    CodecErrorCode code;
    std::string message;
};

class CodecError : public std::runtime_error
{
public:
    CodecErrorCode code;
    std::string type;
    std::string path;

protected:
    CodecError(
        std::string prefix,
        CodecErrorCode code_,
        std::string type_,
        std::string msg,
        std::string path_)
        : std::runtime_error(
              prefix + " " + type_ + ": " + msg +
              (path_.empty() ? "" : " at " + path_))
        , code(code_)
        , type(std::move(type_))
        , path(std::move(path_))
    {
    }
};

class EncodeError : public CodecError
{
public:
    EncodeError(
        CodecErrorCode code_,
        std::string type_,
        std::string msg,
        std::string path_ = {})
        : CodecError(
              "encode",
              code_,
              std::move(type_),
              std::move(msg),
              std::move(path_))
    {
    }
};

class DecodeError : public CodecError
{
public:
    DecodeError(
        CodecErrorCode code_,
        std::string type_,
        std::string msg,
        std::string path_ = {})
        : CodecError(
              "decode",
              code_,
              std::move(type_),
              std::move(msg),
              std::move(path_))
    {
    }
};

template <typename T>
inline T
decode_or_throw(std::expected<T, CodecErrorValue>&& result)
{
    if (!result)
    {
        CATL_XDATA_THROW(DecodeError(
            result.error().code, "binary", result.error().message));
    }
    return std::move(*result);
}

// Encode-side throwing facade, mirroring decode_or_throw. Converts an
// errors-as-values encode result back into an EncodeError for native /
// existing throwing call sites. The value-carrying overload is used by
// codec helpers that produce a T (e.g. a raw IOU mantissa); the void
// overload is used by the sink-writing encoders.
template <typename T>
inline T
encode_or_throw(std::expected<T, CodecErrorValue>&& result)
{
    if (!result)
    {
        CATL_XDATA_THROW(EncodeError(
            result.error().code, "binary", result.error().message));
    }
    return std::move(*result);
}

inline void
encode_or_throw(std::expected<void, CodecErrorValue>&& result)
{
    if (!result)
    {
        CATL_XDATA_THROW(EncodeError(
            result.error().code, "binary", result.error().message));
    }
}

// Compose "<type>: <msg>[ at <path>]" for an encode error value message.
inline std::string
encode_error_message(
    std::string_view type,
    std::string_view msg,
    std::string const& path)
{
    std::string out;
    out.reserve(type.size() + msg.size() + path.size() + 8);
    out.append(type);
    out.append(": ");
    out.append(msg);
    if (!path.empty())
    {
        out.append(" at ");
        out.append(path);
    }
    return out;
}

inline std::unexpected<CodecErrorValue>
encode_error(
    CodecErrorCode code,
    std::string_view type,
    std::string_view msg,
    std::string const& path = {})
{
    return std::unexpected(
        CodecErrorValue{code, encode_error_message(type, msg, path)});
}

// ---------------------------------------------------------------------------
// Parse helpers — from_chars with EncodeError on failure
// ---------------------------------------------------------------------------

inline int64_t
parse_int64(
    std::string_view sv,
    std::string const& type,
    std::string const& path = {})
{
    int64_t val = 0;
    const char* end = sv.data() + sv.size();
    auto [ptr, ec] = std::from_chars(sv.data(), end, val);
    if (ec != std::errc{} || ptr != end)
    {
        CATL_XDATA_THROW(EncodeError(
            CodecErrorCode::invalid_value,
            type,
            "invalid integer: " + std::string(sv),
            path));
    }
    return val;
}

inline uint64_t
parse_uint64(
    std::string_view sv,
    std::string const& type,
    std::string const& path = {})
{
    uint64_t val = 0;
    const char* end = sv.data() + sv.size();
    auto [ptr, ec] = std::from_chars(sv.data(), end, val);
    if (ec != std::errc{} || ptr != end)
    {
        CATL_XDATA_THROW(EncodeError(
            CodecErrorCode::invalid_value,
            type,
            "invalid integer: " + std::string(sv),
            path));
    }
    return val;
}

inline uint64_t
parse_hex_uint64(
    std::string_view sv,
    std::string const& type,
    std::string const& path = {})
{
    // Mirrors try_parse_hex_uint64 exactly. These two must not drift: when
    // they did, the CLI encoder accepted "1234GG" as 0x1234 and a 17-char
    // "00000000000000001" as 0x1 while the JS encoder rejected both
    // (issue 0010).
    if (sv.size() > 16)
    {
        CATL_XDATA_THROW(EncodeError(
            CodecErrorCode::invalid_value,
            type,
            "expected at most 16 hex chars, got " + std::to_string(sv.size()),
            path));
    }
    uint64_t val = 0;
    const char* end = sv.data() + sv.size();
    auto [ptr, ec] = std::from_chars(sv.data(), end, val, 16);
    if (ec != std::errc{} || ptr != end)
    {
        CATL_XDATA_THROW(EncodeError(
            CodecErrorCode::invalid_value,
            type,
            "invalid hex integer: " + std::string(sv),
            path));
    }
    return val;
}

// Validate hex string length. Throws EncodeError if wrong.
inline void
require_hex_length(
    std::string_view hex,
    size_t expected_chars,
    std::string const& type,
    std::string const& path = {})
{
    if (hex.size() != expected_chars)
    {
        CATL_XDATA_THROW(EncodeError(
            CodecErrorCode::invalid_encoding,
            type,
            "expected " + std::to_string(expected_chars) + " hex chars, got " +
                std::to_string(hex.size()),
            path));
    }
}

// ---------------------------------------------------------------------------
// Non-throwing parse helpers — errors-as-values twins of the parse_* above.
// Used by the errors-as-values encode path so no C++ throw is reachable.
// ---------------------------------------------------------------------------

inline std::expected<int64_t, CodecErrorValue>
try_parse_int64(
    std::string_view sv,
    std::string_view type,
    std::string const& path = {})
{
    int64_t val = 0;
    const char* end = sv.data() + sv.size();
    auto [ptr, ec] = std::from_chars(sv.data(), end, val);
    if (ec != std::errc{} || ptr != end)
        return encode_error(
            CodecErrorCode::invalid_value,
            type,
            "invalid integer: " + std::string(sv),
            path);
    return val;
}

inline std::expected<uint64_t, CodecErrorValue>
try_parse_hex_uint64(
    std::string_view sv,
    std::string_view type,
    std::string const& path = {})
{
    // A uint64 is at most 16 hex chars; reject longer (even all-leading-zero,
    // value-fits) inputs so encode matches the canonical form the pre-M6
    // prevalidation required. Both callers (UInt64, MPT amount value) are u64.
    if (sv.size() > 16)
        return encode_error(
            CodecErrorCode::invalid_value,
            type,
            "expected at most 16 hex chars, got " + std::to_string(sv.size()),
            path);
    uint64_t val = 0;
    const char* end = sv.data() + sv.size();
    auto [ptr, ec] = std::from_chars(sv.data(), end, val, 16);
    if (ec != std::errc{} || ptr != end)
        return encode_error(
            CodecErrorCode::invalid_value,
            type,
            "invalid hex integer: " + std::string(sv),
            path);
    return val;
}

inline std::expected<void, CodecErrorValue>
try_require_hex_length(
    std::string_view hex,
    size_t expected_chars,
    std::string_view type,
    std::string const& path = {})
{
    if (hex.size() != expected_chars)
        return encode_error(
            CodecErrorCode::invalid_encoding,
            type,
            "expected " + std::to_string(expected_chars) + " hex chars, got " +
                std::to_string(hex.size()),
            path);
    for (size_t i = 0; i < hex.size(); ++i)
    {
        char c = hex[i];
        bool valid = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
            (c >= 'A' && c <= 'F');
        if (!valid)
            return encode_error(
                CodecErrorCode::invalid_encoding,
                type,
                "invalid hex character at offset " + std::to_string(i),
                path);
    }
    return {};
}

}  // namespace catl::xdata
