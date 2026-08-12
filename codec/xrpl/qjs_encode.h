// qjs_encode.h — QuickJS encoder: JSValue → binary directly (no JSON roundtrip)
//
// Drop-in replacement for encode_field_value / STObjectCodec::encode / etc.
// Instead of reading from boost::json::value, reads from QuickJS JSValue
// and feeds extracted C++ values to the existing typed codec encode methods.
//
// Eliminates: boost::json from the encode path, JSON.stringify, boost::json::parse.

#pragma once

extern "C" {
#include "../../engine/quickjs/quickjs.h"
}

#include "catl/xdata/codec-error.h"
#include "catl/xdata/codecs/codecs.h"
#include "catl/xdata/protocol.h"
#include <algorithm>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace catl::xdata {

// The JS encode path is errors-as-values: every encoder returns
// std::expected<void, CodecErrorValue>. On the wasm build (no C++ EH) this
// keeps invalid input from ever reaching a throw — it surfaces as a clean
// error the bridge converts to a JS TypeError. Native / existing callers use
// the throwing serialize_object_js_or_throw facade at the bottom of the file.

// Forward declarations for mutual recursion
template <ByteSink Sink>
std::expected<void, CodecErrorValue> encode_stobject_js(
    Serializer<Sink>& s,
    JSContext* ctx,
    JSValue obj,
    Protocol const& protocol,
    bool only_signing = false,
    std::string const& path = {});

template <ByteSink Sink>
std::expected<void, CodecErrorValue> encode_starray_js(
    Serializer<Sink>& s,
    JSContext* ctx,
    JSValue arr,
    Protocol const& protocol,
    std::string const& path = {});

// ---------------------------------------------------------------------------
// Helper: get JS string as std::string_view (caller must JS_FreeCString)
// ---------------------------------------------------------------------------

// RAII wrapper for JS_ToCString to avoid leaks
struct JsCString {
    JSContext* ctx;
    const char* ptr;

    JsCString(JSContext* c, JSValue v) : ctx(c), ptr(JS_ToCString(c, v)) {}
    ~JsCString() { if (ptr) JS_FreeCString(ctx, ptr); }

    JsCString(JsCString const&) = delete;
    JsCString& operator=(JsCString const&) = delete;
    JsCString(JsCString&& o) noexcept : ctx(o.ctx), ptr(o.ptr) { o.ptr = nullptr; }

    explicit operator bool() const { return ptr != nullptr; }
    const char* c_str() const { return ptr; }
    std::string_view sv() const { return ptr ? std::string_view(ptr) : std::string_view(); }
};

inline std::expected<void, CodecErrorValue>
validate_hex_string_js(
    std::string_view hex,
    std::string_view type,
    std::string const& path,
    size_t expected_chars = std::string_view::npos)
{
    if (expected_chars != std::string_view::npos && hex.size() != expected_chars)
        return encode_error(
            CodecErrorCode::invalid_encoding,
            type,
            "expected " + std::to_string(expected_chars) + " hex chars, got " +
                std::to_string(hex.size()),
            path);
    if (hex.size() % 2 != 0)
        return encode_error(
            CodecErrorCode::invalid_encoding,
            type,
            "expected even-length hex string",
            path);
    for (size_t i = 0; i < hex.size(); ++i) {
        if (hex_nibble(hex[i]) == 0xFF)
            return encode_error(
                CodecErrorCode::invalid_encoding,
                type,
                "invalid hex character at offset " + std::to_string(i),
                path);
    }
    return {};
}

template <ByteSink Sink>
std::expected<void, CodecErrorValue>
add_hex_validated_js(
    Serializer<Sink>& s,
    std::string_view hex,
    std::string_view type,
    std::string const& path,
    size_t expected_chars = std::string_view::npos)
{
    if (auto valid = validate_hex_string_js(hex, type, path, expected_chars); !valid)
        return valid;
    s.add_hex(hex);
    return {};
}

inline std::expected<void, CodecErrorValue>
validate_iou_decimal_string_js(
    std::string_view value,
    std::string_view type,
    std::string const& path)
{
    static constexpr uint64_t max_mantissa = 9999999999999999ULL;
    static constexpr uint64_t min_mantissa = 1000000000000000ULL;

    if (value == "0" || value == "0.0" || value.empty())
        return {};

    if (value.front() == '-')
        value.remove_prefix(1);
    if (value.empty())
        return encode_error(
            CodecErrorCode::invalid_value, type,
            "invalid decimal string", path);

    auto e_pos = value.find_first_of("eE");
    std::string_view digits_part = e_pos == std::string_view::npos
        ? value
        : value.substr(0, e_pos);
    int e_exponent = 0;
    if (e_pos != std::string_view::npos) {
        auto exp_str = value.substr(e_pos + 1);
        if (exp_str.empty())
            return encode_error(
                CodecErrorCode::invalid_value, type,
                "invalid decimal exponent", path);
        bool exp_neg = false;
        if (exp_str.front() == '-' || exp_str.front() == '+') {
            exp_neg = exp_str.front() == '-';
            exp_str.remove_prefix(1);
        }
        if (exp_str.empty())
            return encode_error(
                CodecErrorCode::invalid_value, type,
                "invalid decimal exponent", path);
        for (char c : exp_str) {
            if (c < '0' || c > '9')
                return encode_error(
                    CodecErrorCode::invalid_value, type,
                    "invalid decimal exponent", path);
            if (e_exponent > 100000)
                return encode_error(
                    CodecErrorCode::out_of_range, type,
                    "decimal exponent is too large", path);
            e_exponent = e_exponent * 10 + (c - '0');
        }
        if (exp_neg)
            e_exponent = -e_exponent;
    }

    uint64_t mantissa = 0;
    int exponent = 0;
    int decimals = 0;
    bool seen_dot = false;
    bool seen_digit = false;
    bool in_trailing_digits = false;

    for (char c : digits_part) {
        if (c == '.') {
            if (seen_dot)
                return encode_error(
                    CodecErrorCode::invalid_value, type,
                    "invalid decimal string", path);
            seen_dot = true;
            continue;
        }
        if (c < '0' || c > '9')
            return encode_error(
                CodecErrorCode::invalid_value, type,
                "invalid decimal string", path);

        seen_digit = true;
        if (in_trailing_digits) {
            if (!seen_dot)
                ++exponent;
            continue;
        }

        uint64_t next = mantissa * 10 + static_cast<uint64_t>(c - '0');
        if (next > max_mantissa) {
            if (c >= '5' && mantissa < max_mantissa)
                ++mantissa;
            in_trailing_digits = true;
            if (!seen_dot)
                ++exponent;
            continue;
        }

        mantissa = next;
        if (seen_dot)
            ++decimals;
    }
    if (!seen_digit)
        return encode_error(
            CodecErrorCode::invalid_value, type,
            "invalid decimal string", path);

    exponent -= decimals;
    exponent += e_exponent;

    if (mantissa == 0)
        return {};

    while (mantissa < min_mantissa) {
        mantissa *= 10;
        --exponent;
    }
    while (mantissa > max_mantissa) {
        mantissa /= 10;
        ++exponent;
    }

    if (exponent < -96 || exponent > 80)
        return encode_error(
            CodecErrorCode::out_of_range,
            type,
            "decimal exponent out of range: " + std::to_string(exponent),
            path);
    return {};
}

// Helper: check if JSValue has a named property
inline bool
js_has_prop(JSContext* ctx, JSValue obj, const char* name)
{
    JSAtom atom = JS_NewAtom(ctx, name);
    int has = JS_HasProperty(ctx, obj, atom);
    JS_FreeAtom(ctx, atom);
    return has > 0;
}

// Helper: get a string property from a JS object
// Returns empty string_view if property doesn't exist or isn't a string.
// Caller must keep the returned JsCString alive while using the string_view.
inline JsCString
js_get_string_prop(JSContext* ctx, JSValue obj, const char* name)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    JsCString result(ctx, v);
    JS_FreeValue(ctx, v);
    return result;
}

// ---------------------------------------------------------------------------
// VL size computation for JS values (needed for VL prefix before encoding)
// ---------------------------------------------------------------------------

// Compute encoded size of a field value from JSValue.
// This mirrors codecs::field_value_encoded_size but reads from JS.
inline std::expected<size_t, CodecErrorValue>
field_value_encoded_size_js(
    JSContext* ctx,
    FieldDef const& field,
    JSValue v,
    Protocol const& protocol,
    std::string const& path = {})
{
    using namespace codecs;
    auto const& t = field.meta.type;

    // Fixed-size types
    if (t == FieldTypes::UInt8)   return UInt8Codec::fixed_size;
    if (t == FieldTypes::UInt16)  return UInt16Codec::fixed_size;
    if (t == FieldTypes::UInt32)  return UInt32Codec::fixed_size;
    if (t == FieldTypes::UInt64)  return UInt64Codec::fixed_size;
    if (t == FieldTypes::Int32)   return Int32Codec::fixed_size;
    if (t == FieldTypes::Int64)   return Int64Codec::fixed_size;
    if (t == FieldTypes::Hash128) return Hash128Codec::fixed_size;
    if (t == FieldTypes::Hash160) return Hash160Codec::fixed_size;
    if (t == FieldTypes::Hash192) return Hash192Codec::fixed_size;
    if (t == FieldTypes::Hash256) return Hash256Codec::fixed_size;
    if (t == FieldTypes::UInt96)  return UInt96Codec::fixed_size;
    if (t == FieldTypes::UInt384) return UInt384Codec::fixed_size;
    if (t == FieldTypes::UInt512) return UInt512Codec::fixed_size;
    if (t == FieldTypes::Currency) return CurrencyCodec::fixed_size;
    if (t == FieldTypes::Number)  return NumberCodec::fixed_size;

    // Enum fields — fixed size based on underlying type
    if (field.code == EnumFieldCodes::TransactionType)  return 2;
    if (field.code == EnumFieldCodes::LedgerEntryType)  return 2;
    if (field.code == EnumFieldCodes::TransactionResult) return 1;
    if (field.code == EnumFieldCodes::PermissionValue)  return 4;

    // Amount: string → 8 (native), object with mpt_issuance_id → 33, else → 48
    if (t == FieldTypes::Amount) {
        if (JS_IsString(v))
            return AmountCodec::native_size;
        if (JS_IsObject(v)) {
            if (js_has_prop(ctx, v, "mpt_issuance_id"))
                return AmountCodec::mpt_size;
            return AmountCodec::iou_size;
        }
        return AmountCodec::native_size;
    }

    // AccountID: base58 string → 20 (or 0 for zero account)
    if (t == FieldTypes::AccountID) {
        JsCString str(ctx, v);
        if (str && str.sv() == AccountIDCodec::ZERO_ACCOUNT_B58)
            return 0;
        return 20;
    }

    // Issue: string → 20 (native), object with mpt_issuance_id → 44, object with issuer → 40, else → 20
    if (t == FieldTypes::Issue) {
        if (JS_IsString(v))
            return 20;
        if (JS_IsObject(v)) {
            if (js_has_prop(ctx, v, "mpt_issuance_id"))
                return 44;
            if (js_has_prop(ctx, v, "issuer"))
                return 40;
            return 20;
        }
        return 20;
    }

    // XChainBridge: complex — compute by summing sub-fields
    if (t == FieldTypes::XChainBridge) {
        auto lcd = js_get_string_prop(ctx, v, "LockingChainDoor");
        auto icd = js_get_string_prop(ctx, v, "IssuingChainDoor");
        size_t lcd_size = (lcd && lcd.sv() == AccountIDCodec::ZERO_ACCOUNT_B58) ? 0 : 20;
        size_t icd_size = (icd && icd.sv() == AccountIDCodec::ZERO_ACCOUNT_B58) ? 0 : 20;

        // LockingChainIssue
        JSValue lci = JS_GetPropertyStr(ctx, v, "LockingChainIssue");
        size_t lci_size = 20;
        if (JS_IsObject(lci)) {
            if (js_has_prop(ctx, lci, "mpt_issuance_id"))
                lci_size = 44;
            else if (js_has_prop(ctx, lci, "issuer"))
                lci_size = 40;
        }
        JS_FreeValue(ctx, lci);

        // IssuingChainIssue
        JSValue ici = JS_GetPropertyStr(ctx, v, "IssuingChainIssue");
        size_t ici_size = 20;
        if (JS_IsObject(ici)) {
            if (js_has_prop(ctx, ici, "mpt_issuance_id"))
                ici_size = 44;
            else if (js_has_prop(ctx, ici, "issuer"))
                ici_size = 40;
        }
        JS_FreeValue(ctx, ici);

        return vl_prefix_size(lcd_size) + lcd_size + lci_size +
               vl_prefix_size(icd_size) + icd_size + ici_size;
    }

    // Vector256: array of hex strings, each 32 bytes
    if (t == FieldTypes::Vector256) {
        JSValue len_val = JS_GetPropertyStr(ctx, v, "length");
        uint32_t len = 0;
        JS_ToUint32(ctx, &len, len_val);
        JS_FreeValue(ctx, len_val);
        return len * 32;
    }

    // Blob: hex string, size = strlen / 2
    if (t == FieldTypes::Blob) {
        JsCString str(ctx, v);
        if (str)
            return str.sv().size() / 2;
        return 0;
    }

    // PathSet: walk array to compute size
    if (t == FieldTypes::PathSet) {
        size_t size = 0;
        JSValue len_val = JS_GetPropertyStr(ctx, v, "length");
        uint32_t num_paths = 0;
        JS_ToUint32(ctx, &num_paths, len_val);
        JS_FreeValue(ctx, len_val);

        for (uint32_t p = 0; p < num_paths; ++p) {
            if (p > 0) ++size;  // PATH_SEPARATOR
            JSValue path_arr = JS_GetPropertyUint32(ctx, v, p);
            JSValue hops_len_val = JS_GetPropertyStr(ctx, path_arr, "length");
            uint32_t num_hops = 0;
            JS_ToUint32(ctx, &num_hops, hops_len_val);
            JS_FreeValue(ctx, hops_len_val);

            for (uint32_t h = 0; h < num_hops; ++h) {
                ++size;  // type byte
                JSValue hop = JS_GetPropertyUint32(ctx, path_arr, h);
                if (js_has_prop(ctx, hop, "account"))  size += 20;
                if (js_has_prop(ctx, hop, "currency")) size += 20;
                if (js_has_prop(ctx, hop, "issuer"))   size += 20;
                JS_FreeValue(ctx, hop);
            }
            JS_FreeValue(ctx, path_arr);
        }
        ++size;  // END_BYTE
        return size;
    }

    // STObject: walk fields recursively
    if (t == FieldTypes::STObject) {
        if (!JS_IsObject(v))
            return encode_error(
                CodecErrorCode::invalid_value,
                "STObject",
                "must be an object",
                path);

        // We need to walk the JS object and sum up all field sizes.
        // This mirrors STObjectCodec::encoded_size logic.
        size_t size = 0;

        JSPropertyEnum* tab = nullptr;
        uint32_t tab_len = 0;
        if (JS_GetOwnPropertyNames(ctx, &tab, &tab_len, v,
                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) != 0) {
            JSValue exc = JS_GetException(ctx);
            JS_FreeValue(ctx, exc);
            return encode_error(
                CodecErrorCode::invalid_value,
                "STObject",
                "must be an object",
                path);
        }

        for (uint32_t i = 0; i < tab_len; ++i) {
            const char* key = JS_AtomToCString(ctx, tab[i].atom);
            if (!key) continue;

            auto field_opt = protocol.find_field(std::string(key));
            JS_FreeCString(ctx, key);
            if (!field_opt || !field_opt->meta.is_serialized)
                continue;

            // Field header size
            auto type_code = get_field_type_code(field_opt->code);
            auto field_id = get_field_id(field_opt->code);
            if (type_code < 16 && field_id < 16)     size += 1;
            else if (type_code < 16 || field_id < 16) size += 2;
            else                                       size += 3;

            JSValue val = JS_GetProperty(ctx, v, tab[i].atom);
            auto field_path = path.empty() ? field_opt->name : path + "." + field_opt->name;
            auto val_size = field_value_encoded_size_js(
                ctx, *field_opt, val, protocol, field_path);
            JS_FreeValue(ctx, val);
            if (!val_size) {
                for (uint32_t j = 0; j < tab_len; ++j)
                    JS_FreeAtom(ctx, tab[j].atom);
                js_free(ctx, tab);
                return std::unexpected(std::move(val_size.error()));
            }

            if (field_opt->meta.is_vl_encoded)
                size += vl_prefix_size(*val_size);
            size += *val_size;

            if (field_opt->meta.type == FieldTypes::STObject)
                size += 1;  // end marker
            else if (field_opt->meta.type == FieldTypes::STArray)
                size += 1;  // end marker
        }

        for (uint32_t i = 0; i < tab_len; ++i)
            JS_FreeAtom(ctx, tab[i].atom);
        js_free(ctx, tab);
        return size;
    }

    // STArray: walk array elements
    if (t == FieldTypes::STArray) {
        if (!JS_IsObject(v))
            return encode_error(
                CodecErrorCode::invalid_value,
                "STArray",
                "must be an object",
                path);

        size_t size = 0;
        JSValue len_val = JS_GetPropertyStr(ctx, v, "length");
        uint32_t arr_len = 0;
        JS_ToUint32(ctx, &arr_len, len_val);
        JS_FreeValue(ctx, len_val);

        for (uint32_t i = 0; i < arr_len; ++i) {
            JSValue elem = JS_GetPropertyUint32(ctx, v, i);
            auto elem_path = path + "[" + std::to_string(i) + "]";
            // Each elem is {FieldName: inner_object}
            JSPropertyEnum* etab = nullptr;
            uint32_t etab_len = 0;
            if (JS_GetOwnPropertyNames(ctx, &etab, &etab_len, elem,
                    JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) != 0) {
                JSValue exc = JS_GetException(ctx);
                JS_FreeValue(ctx, exc);
                JS_FreeValue(ctx, elem);
                return encode_error(
                    CodecErrorCode::invalid_value,
                    "STArray",
                    "element must be an object",
                    elem_path);
            }

            for (uint32_t j = 0; j < etab_len; ++j) {
                const char* key = JS_AtomToCString(ctx, etab[j].atom);
                if (!key) continue;

                auto field_opt = protocol.find_field(std::string(key));
                JS_FreeCString(ctx, key);
                if (!field_opt) continue;

                // Field header
                auto type_code = get_field_type_code(field_opt->code);
                auto field_id = get_field_id(field_opt->code);
                if (type_code < 16 && field_id < 16)     size += 1;
                else if (type_code < 16 || field_id < 16) size += 2;
                else                                       size += 3;

                // Inner object encoded size (as STObject, no end marker in its own size)
                JSValue inner = JS_GetProperty(ctx, elem, etab[j].atom);
                auto field_path = elem_path + "." + field_opt->name;
                auto inner_size = field_value_encoded_size_js(
                    ctx, *field_opt, inner, protocol, field_path);
                JS_FreeValue(ctx, inner);
                if (!inner_size) {
                    for (uint32_t k = 0; k < etab_len; ++k)
                        JS_FreeAtom(ctx, etab[k].atom);
                    js_free(ctx, etab);
                    JS_FreeValue(ctx, elem);
                    return std::unexpected(std::move(inner_size.error()));
                }
                size += *inner_size;

                size += 1;  // ObjectEndMarker per element
            }
            for (uint32_t j = 0; j < etab_len; ++j)
                JS_FreeAtom(ctx, etab[j].atom);
            js_free(ctx, etab);
            JS_FreeValue(ctx, elem);
        }
        return size;
    }

    // Fallback: fixed-size types
    if (field.meta.type.fixed_size > 0)
        return field.meta.type.fixed_size;

    // VL-encoded unknown types: treat as hex blob
    if (field.meta.is_vl_encoded) {
        JsCString str(ctx, v);
        if (str)
            return str.sv().size() / 2;
        return 0;
    }

    // Unreachable for real protocol fields (every serialized type is handled
    // above). Return 0 rather than throw; encode_field_value_js reports the
    // unknown-type error as a value on the actual encode pass.
    return 0;
}

// ---------------------------------------------------------------------------
// encode_field_value_js: JSValue → binary (value bytes only, no header/VL)
// ---------------------------------------------------------------------------

// Propagate an std::expected<void, CodecErrorValue> failure as an early return.
// Scoped to this file; undefined after the traversal functions below.
#define XDATA_TRY(expr)                                                      \
    do {                                                                     \
        if (auto _xtry = (expr); !_xtry)                                     \
            return std::unexpected(std::move(_xtry.error()));                \
    } while (0)

template <ByteSink Sink>
std::expected<void, CodecErrorValue>
encode_field_value_js(
    Serializer<Sink>& s,
    JSContext* ctx,
    FieldDef const& field,
    JSValue v,
    Protocol const& protocol,
    std::string const& path)
{
    using namespace codecs;
    auto const& t = field.meta.type;

    // Enum fields first (integer code compare, no string matching)
    if (field.code == EnumFieldCodes::TransactionType)
    {
        if (JS_IsString(v)) {
            JsCString str(ctx, v);
            auto const& tx_types = protocol.transactionTypes();
            auto it = tx_types.find(std::string(str.sv()));
            if (it != tx_types.end()) {
                s.add_u16(it->second);
                return {};
            }
            return encode_error(
                CodecErrorCode::unknown_enum,
                "TransactionType",
                "unknown: " + std::string(str.sv()),
                path);
        }
        // Numeric fallback
        int64_t val;
        JS_ToInt64(ctx, &val, v);
        s.add_u16(static_cast<uint16_t>(val));
        return {};
    }

    if (field.code == EnumFieldCodes::LedgerEntryType)
    {
        if (JS_IsString(v)) {
            JsCString str(ctx, v);
            auto const& le_types = protocol.ledgerEntryTypes();
            auto it = le_types.find(std::string(str.sv()));
            if (it != le_types.end()) {
                s.add_u16(it->second);
                return {};
            }
            return encode_error(
                CodecErrorCode::unknown_enum,
                "LedgerEntryType",
                "unknown: " + std::string(str.sv()),
                path);
        }
        int64_t val;
        JS_ToInt64(ctx, &val, v);
        s.add_u16(static_cast<uint16_t>(val));
        return {};
    }

    if (field.code == EnumFieldCodes::TransactionResult)
    {
        if (JS_IsString(v)) {
            JsCString str(ctx, v);
            auto const& results = protocol.transactionResults();
            auto it = results.find(std::string(str.sv()));
            if (it != results.end()) {
                s.add_u8(static_cast<uint8_t>(it->second));
                return {};
            }
            return encode_error(
                CodecErrorCode::unknown_enum,
                "TransactionResult",
                "unknown: " + std::string(str.sv()),
                path);
        }
        int64_t val;
        JS_ToInt64(ctx, &val, v);
        s.add_u8(static_cast<uint8_t>(val));
        return {};
    }

    if (field.code == EnumFieldCodes::PermissionValue)
    {
        if (JS_IsString(v)) {
            JsCString str(ctx, v);
            auto const& perms = protocol.permissions();
            auto it = perms.find(std::string(str.sv()));
            if (it != perms.end()) {
                s.add_u32(it->second);
                return {};
            }
            return encode_error(
                CodecErrorCode::unknown_enum,
                "PermissionValue",
                "unknown: " + std::string(str.sv()),
                path);
        }
        int64_t val;
        JS_ToInt64(ctx, &val, v);
        s.add_u32(static_cast<uint32_t>(val));
        return {};
    }

    // UInt8
    if (t == FieldTypes::UInt8)
    {
        int64_t val;
        JS_ToInt64(ctx, &val, v);
        UInt8Codec::encode(s, static_cast<uint8_t>(val));
        return {};
    }

    // UInt16
    if (t == FieldTypes::UInt16)
    {
        int64_t val;
        JS_ToInt64(ctx, &val, v);
        UInt16Codec::encode(s, static_cast<uint16_t>(val));
        return {};
    }

    // UInt32
    if (t == FieldTypes::UInt32)
    {
        int64_t val;
        JS_ToInt64(ctx, &val, v);
        UInt32Codec::encode(s, static_cast<uint32_t>(val));
        return {};
    }

    // UInt64 — hex string in JSON (too large for JS number)
    if (t == FieldTypes::UInt64)
    {
        JsCString str(ctx, v);
        if (!str) {
            return encode_error(
                CodecErrorCode::invalid_value, "UInt64",
                "expected hex string", path);
        }
        XDATA_TRY(UInt64Codec::encode_hex_expected(s, str.sv()));
        return {};
    }

    // Int32
    if (t == FieldTypes::Int32)
    {
        int64_t val;
        JS_ToInt64(ctx, &val, v);
        Int32Codec::encode(s, static_cast<int32_t>(val));
        return {};
    }

    // Int64
    if (t == FieldTypes::Int64)
    {
        int64_t val;
        JS_ToInt64(ctx, &val, v);
        Int64Codec::encode(s, val);
        return {};
    }

    // Hash/UInt types (hex strings)
    if (t == FieldTypes::Hash128 || t == FieldTypes::Hash160 ||
        t == FieldTypes::Hash192 || t == FieldTypes::Hash256 ||
        t == FieldTypes::UInt96  || t == FieldTypes::UInt384 ||
        t == FieldTypes::UInt512)
    {
        JsCString str(ctx, v);
        if (!str) {
            return encode_error(
                CodecErrorCode::invalid_value,
                std::string(t.name),
                "expected hex string", path);
        }
        XDATA_TRY(add_hex_validated_js(
            s, str.sv(), std::string(t.name), path, t.fixed_size * 2));
        return {};
    }

    // Amount: string → native, object → IOU or MPT
    if (t == FieldTypes::Amount)
    {
        if (JS_IsString(v)) {
            JsCString str(ctx, v);
            XDATA_TRY(AmountCodec::encode_native_expected(s, str.sv(), path));
            return {};
        }
        if (JS_IsObject(v)) {
            if (js_has_prop(ctx, v, "mpt_issuance_id")) {
                auto val_str = js_get_string_prop(ctx, v, "value");
                auto mptid_str = js_get_string_prop(ctx, v, "mpt_issuance_id");
                if (!val_str || !mptid_str) {
                    return encode_error(
                        CodecErrorCode::missing_field, "Amount",
                        "MPT missing 'value' or 'mpt_issuance_id'", path);
                }
                XDATA_TRY(AmountCodec::encode_mpt_expected(
                    s, val_str.sv(), mptid_str.sv(), path));
                return {};
            }
            // IOU
            auto val_str = js_get_string_prop(ctx, v, "value");
            auto cur_str = js_get_string_prop(ctx, v, "currency");
            auto iss_str = js_get_string_prop(ctx, v, "issuer");
            if (!val_str || !cur_str || !iss_str) {
                return encode_error(
                    CodecErrorCode::missing_field, "Amount",
                    "IOU requires 'value', 'currency', and 'issuer'", path);
            }
            XDATA_TRY(validate_iou_decimal_string_js(
                val_str.sv(), "Amount", path));
            XDATA_TRY(AmountCodec::encode_iou_expected(
                s, val_str.sv(), cur_str.sv(), iss_str.sv(), path));
            return {};
        }
        return encode_error(
            CodecErrorCode::invalid_value, "Amount",
            "expected string or object", path);
    }

    // AccountID: base58 string
    if (t == FieldTypes::AccountID)
    {
        JsCString str(ctx, v);
        if (!str) {
            return encode_error(
                CodecErrorCode::invalid_value, "AccountID",
                "expected string", path);
        }
        XDATA_TRY(AccountIDCodec::encode_expected(s, str.sv(), path));
        return {};
    }

    // Blob: hex string
    if (t == FieldTypes::Blob)
    {
        JsCString str(ctx, v);
        if (!str) {
            return encode_error(
                CodecErrorCode::invalid_value, "Blob",
                "expected hex string", path);
        }
        XDATA_TRY(add_hex_validated_js(s, str.sv(), "Blob", path));
        return {};
    }

    // Currency: string
    if (t == FieldTypes::Currency)
    {
        JsCString str(ctx, v);
        if (!str) {
            return encode_error(
                CodecErrorCode::invalid_value, "Currency",
                "expected string", path);
        }
        XDATA_TRY(CurrencyCodec::encode_expected(s, str.sv(), path));
        return {};
    }

    // Issue: string or object
    if (t == FieldTypes::Issue)
    {
        if (JS_IsString(v)) {
            JsCString str(ctx, v);
            auto sv = str.sv();
            if (sv == "XRP" || sv == "XAH" || sv.empty()) {
                IssueCodec::encode_native(s);
            } else {
                return encode_error(
                    CodecErrorCode::invalid_value, "Issue",
                    "string value must be XRP or XAH, got: " + std::string(sv), path);
            }
            return {};
        }
        if (JS_IsObject(v)) {
            if (js_has_prop(ctx, v, "mpt_issuance_id")) {
                auto mptid = js_get_string_prop(ctx, v, "mpt_issuance_id");
                if (!mptid) {
                    return encode_error(
                        CodecErrorCode::missing_field, "Issue",
                        "missing 'mpt_issuance_id'", path);
                }
                XDATA_TRY(IssueCodec::encode_mpt_expected(s, mptid.sv(), path));
                return {};
            }
            if (!js_has_prop(ctx, v, "currency")) {
                return encode_error(
                    CodecErrorCode::missing_field, "Issue",
                    "missing 'currency' or 'mpt_issuance_id'", path);
            }
            if (!js_has_prop(ctx, v, "issuer")) {
                IssueCodec::encode_native(s);
                return {};
            }
            auto cur = js_get_string_prop(ctx, v, "currency");
            auto iss = js_get_string_prop(ctx, v, "issuer");
            XDATA_TRY(IssueCodec::encode_iou_expected(s, cur.sv(), iss.sv(), path));
            return {};
        }
        return encode_error(
            CodecErrorCode::invalid_value, "Issue",
            "expected string or object", path);
    }

    // Number: string or integer
    if (t == FieldTypes::Number)
    {
        if (JS_IsString(v)) {
            // Use boost::json path for string parsing — NumberCodec::encode
            // handles full decimal + scientific notation parsing
            JsCString str(ctx, v);
            XDATA_TRY(validate_iou_decimal_string_js(
                str.sv(), "Number", path));
            boost::json::value bjv(boost::json::string(str.sv()));
            XDATA_TRY(NumberCodec::encode_expected(s, bjv));
            return {};
        }
        // JS number → int64 mantissa with exponent 0
        int64_t val;
        JS_ToInt64(ctx, &val, v);
        NumberCodec::encode(s, val, 0);
        return {};
    }

    // XChainBridge: object with 4 sub-fields
    if (t == FieldTypes::XChainBridge)
    {
        // LockingChainDoor (AccountID, VL-encoded)
        auto lcd = js_get_string_prop(ctx, v, "LockingChainDoor");
        if (!lcd) {
            return encode_error(
                CodecErrorCode::missing_field, "XChainBridge",
                "missing 'LockingChainDoor'", path);
        }
        size_t lcd_size = (lcd.sv() == AccountIDCodec::ZERO_ACCOUNT_B58) ? 0 : 20;
        s.add_vl_prefix(lcd_size);
        XDATA_TRY(AccountIDCodec::encode_expected(s, lcd.sv(), path));

        // LockingChainIssue (raw, no VL)
        JSValue lci_val = JS_GetPropertyStr(ctx, v, "LockingChainIssue");
        // Build a temporary FieldDef to reuse our Issue encoding
        FieldDef issue_field;
        issue_field.name = "LockingChainIssue";
        issue_field.meta.type = FieldTypes::Issue;
        issue_field.meta.is_serialized = true;
        issue_field.meta.is_signing_field = true;
        issue_field.meta.is_vl_encoded = false;
        issue_field.meta.nth = 0;
        issue_field.code = 0;
        auto lci_res = encode_field_value_js(
            s, ctx, issue_field, lci_val, protocol, path + ".LockingChainIssue");
        JS_FreeValue(ctx, lci_val);
        if (!lci_res)
            return std::unexpected(std::move(lci_res.error()));

        // IssuingChainDoor (AccountID, VL-encoded)
        auto icd = js_get_string_prop(ctx, v, "IssuingChainDoor");
        if (!icd) {
            return encode_error(
                CodecErrorCode::missing_field, "XChainBridge",
                "missing 'IssuingChainDoor'", path);
        }
        size_t icd_size = (icd.sv() == AccountIDCodec::ZERO_ACCOUNT_B58) ? 0 : 20;
        s.add_vl_prefix(icd_size);
        XDATA_TRY(AccountIDCodec::encode_expected(s, icd.sv(), path));

        // IssuingChainIssue (raw, no VL)
        JSValue ici_val = JS_GetPropertyStr(ctx, v, "IssuingChainIssue");
        issue_field.name = "IssuingChainIssue";
        auto ici_res = encode_field_value_js(
            s, ctx, issue_field, ici_val, protocol, path + ".IssuingChainIssue");
        JS_FreeValue(ctx, ici_val);
        if (!ici_res)
            return std::unexpected(std::move(ici_res.error()));
        return {};
    }

    // Vector256: array of hex strings
    if (t == FieldTypes::Vector256)
    {
        JSValue len_val = JS_GetPropertyStr(ctx, v, "length");
        uint32_t len = 0;
        JS_ToUint32(ctx, &len, len_val);
        JS_FreeValue(ctx, len_val);

        for (uint32_t i = 0; i < len; ++i) {
            JSValue elem = JS_GetPropertyUint32(ctx, v, i);
            JsCString str(ctx, elem);
            JS_FreeValue(ctx, elem);
            if (!str) {
                return encode_error(
                    CodecErrorCode::invalid_value,
                    "Vector256",
                    "element must be a hex string",
                    path + "[" + std::to_string(i) + "]");
            }
            XDATA_TRY(add_hex_validated_js(
                s, str.sv(), "Vector256",
                path + "[" + std::to_string(i) + "]", 64));
        }
        return {};
    }

    // PathSet: array of arrays of hop objects
    if (t == FieldTypes::PathSet)
    {
        JSValue len_val = JS_GetPropertyStr(ctx, v, "length");
        uint32_t num_paths = 0;
        JS_ToUint32(ctx, &num_paths, len_val);
        JS_FreeValue(ctx, len_val);

        std::expected<void, CodecErrorValue> status;
        for (uint32_t p = 0; p < num_paths && status; ++p) {
            if (p > 0)
                s.add_path_separator();

            JSValue path_arr = JS_GetPropertyUint32(ctx, v, p);
            JSValue hops_len_val = JS_GetPropertyStr(ctx, path_arr, "length");
            uint32_t num_hops = 0;
            JS_ToUint32(ctx, &num_hops, hops_len_val);
            JS_FreeValue(ctx, hops_len_val);

            for (uint32_t h = 0; h < num_hops && status; ++h) {
                JSValue hop = JS_GetPropertyUint32(ctx, path_arr, h);
                auto hop_path =
                    path + "[" + std::to_string(p) + "][" + std::to_string(h) + "]";

                uint8_t type_byte = 0;
                bool has_account  = js_has_prop(ctx, hop, "account");
                bool has_currency = js_has_prop(ctx, hop, "currency");
                bool has_issuer   = js_has_prop(ctx, hop, "issuer");
                if (has_account)  type_byte |= PathSet::TYPE_ACCOUNT;
                if (has_currency) type_byte |= PathSet::TYPE_CURRENCY;
                if (has_issuer)   type_byte |= PathSet::TYPE_ISSUER;

                s.add_u8(type_byte);

                if (status && has_account) {
                    auto acct = js_get_string_prop(ctx, hop, "account");
                    if (!acct)
                        status = encode_error(
                            CodecErrorCode::invalid_value,
                            "PathSet",
                            "account must be a string",
                            hop_path);
                    else if (auto r = AccountIDCodec::encode_expected(
                            s, acct.sv(), hop_path); !r)
                        status = std::unexpected(std::move(r.error()));
                }
                if (status && has_currency) {
                    auto cur = js_get_string_prop(ctx, hop, "currency");
                    if (!cur)
                        status = encode_error(
                            CodecErrorCode::invalid_value,
                            "PathSet",
                            "currency must be a string",
                            hop_path);
                    else if (auto r = CurrencyCodec::encode_expected(
                            s, cur.sv(), hop_path); !r)
                        status = std::unexpected(std::move(r.error()));
                }
                if (status && has_issuer) {
                    auto iss = js_get_string_prop(ctx, hop, "issuer");
                    if (!iss)
                        status = encode_error(
                            CodecErrorCode::invalid_value,
                            "PathSet",
                            "issuer must be a string",
                            hop_path);
                    else if (auto r = AccountIDCodec::encode_expected(
                            s, iss.sv(), hop_path); !r)
                        status = std::unexpected(std::move(r.error()));
                }
                JS_FreeValue(ctx, hop);
            }
            JS_FreeValue(ctx, path_arr);
        }
        if (!status)
            return status;
        s.add_pathset_end();
        return {};
    }

    // STObject: recurse
    if (t == FieldTypes::STObject)
    {
        if (!JS_IsObject(v))
            return encode_error(
                CodecErrorCode::invalid_value,
                "STObject",
                "must be an object",
                path);
        XDATA_TRY(encode_stobject_js(s, ctx, v, protocol, false, path));
        return {};
    }

    // STArray: recurse
    if (t == FieldTypes::STArray)
    {
        if (!JS_IsObject(v))
            return encode_error(
                CodecErrorCode::invalid_value,
                "STArray",
                "must be an object",
                path);
        XDATA_TRY(encode_starray_js(s, ctx, v, protocol, path));
        return {};
    }

    // Fallback: fixed-size or VL-encoded unknown types — treat as hex blob
    if (field.meta.type.fixed_size > 0 || field.meta.is_vl_encoded)
    {
        JsCString str(ctx, v);
        if (str) {
            XDATA_TRY(add_hex_validated_js(
                s, str.sv(), std::string(field.meta.type.name), path,
                field.meta.type.fixed_size > 0
                    ? field.meta.type.fixed_size * 2
                    : std::string_view::npos));
            return {};
        }
    }

    return encode_error(
        CodecErrorCode::invalid_value,
        std::string(field.meta.type.name),
        "unknown type code " + std::to_string(field.meta.type.code),
        path);
}

// ---------------------------------------------------------------------------
// encode_stobject_js: walk JS object properties, sort by field code, encode
// ---------------------------------------------------------------------------

template <ByteSink Sink>
std::expected<void, CodecErrorValue>
encode_stobject_js(
    Serializer<Sink>& s,
    JSContext* ctx,
    JSValue obj,
    Protocol const& protocol,
    bool only_signing,
    std::string const& path)
{
    struct FieldEntry
    {
        FieldDef def;
        JSValue val;  // borrowed — must not free until after encoding
    };

    // Get all property names
    JSPropertyEnum* tab = nullptr;
    uint32_t tab_len = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &tab_len, obj,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) != 0) {
        JSValue exc = JS_GetException(ctx);
        JS_FreeValue(ctx, exc);
        return encode_error(
            CodecErrorCode::invalid_value,
            "STObject",
            "must be an object",
            path);
    }

    // Collect field entries with their JSValues
    std::vector<FieldEntry> fields;
    fields.reserve(tab_len);

    // We need to hold references to JSValues we get
    std::vector<JSValue> held_values;
    held_values.reserve(tab_len);

    for (uint32_t i = 0; i < tab_len; ++i) {
        const char* key = JS_AtomToCString(ctx, tab[i].atom);
        if (!key) continue;

        auto field_opt = protocol.find_field(std::string(key));
        JS_FreeCString(ctx, key);

        if (!field_opt)
            continue;
        if (!field_opt->meta.is_serialized)
            continue;
        if (only_signing && !field_opt->meta.is_signing_field)
            continue;

        JSValue val = JS_GetProperty(ctx, obj, tab[i].atom);
        if (JS_IsUndefined(val)) {
            JS_FreeValue(ctx, val);
            continue;
        }
        held_values.push_back(val);
        fields.push_back({*field_opt, val});
    }

    // Free atom table
    for (uint32_t i = 0; i < tab_len; ++i)
        JS_FreeAtom(ctx, tab[i].atom);
    js_free(ctx, tab);

    // Sort by field code (same as STObjectCodec)
    std::sort(fields.begin(), fields.end(), [](auto const& a, auto const& b) {
        return a.def.code < b.def.code;
    });

    // Encode each field. Track failure as a value so held JSValues are always
    // freed (no early return past the cleanup below).
    std::expected<void, CodecErrorValue> status;
    for (auto const& entry : fields) {
        if (!status)
            break;

        auto field_path =
            path.empty() ? entry.def.name : path + "." + entry.def.name;

        s.add_field_header(entry.def);

        if (entry.def.meta.is_vl_encoded) {
            auto val_size = field_value_encoded_size_js(
                ctx, entry.def, entry.val, protocol, field_path);
            if (!val_size) {
                status = std::unexpected(std::move(val_size.error()));
                break;
            }
            // VL length ceiling (matches add_vl_prefix's throw threshold);
            // surfaces as a value instead of a C++ throw on the wasm build.
            if (*val_size > 918744) {
                status = encode_error(
                    CodecErrorCode::out_of_range, entry.def.name,
                    "VL field is too large: " + std::to_string(*val_size),
                    field_path);
                break;
            }
            s.add_vl_prefix(*val_size);
        }

        if (auto r = encode_field_value_js(
                s, ctx, entry.def, entry.val, protocol, field_path); !r) {
            status = std::unexpected(std::move(r.error()));
            break;
        }

        if (entry.def.meta.type == FieldTypes::STObject)
            s.add_object_end_marker();
        else if (entry.def.meta.type == FieldTypes::STArray)
            s.add_array_end_marker();
    }

    // Free held JSValues
    for (auto& val : held_values)
        JS_FreeValue(ctx, val);

    return status;
}

// ---------------------------------------------------------------------------
// encode_starray_js: walk JS array, each element is {FieldName: inner_object}
// ---------------------------------------------------------------------------

template <ByteSink Sink>
std::expected<void, CodecErrorValue>
encode_starray_js(
    Serializer<Sink>& s,
    JSContext* ctx,
    JSValue arr,
    Protocol const& protocol,
    std::string const& path)
{
    if (!JS_IsObject(arr))
        return encode_error(
            CodecErrorCode::invalid_value,
            "STArray",
            "must be an object",
            path);

    JSValue len_val = JS_GetPropertyStr(ctx, arr, "length");
    uint32_t arr_len = 0;
    JS_ToUint32(ctx, &arr_len, len_val);
    JS_FreeValue(ctx, len_val);

    // Track failure as a value so per-element JS handles are always freed.
    std::expected<void, CodecErrorValue> status;
    for (uint32_t idx = 0; idx < arr_len && status; ++idx) {
        JSValue elem = JS_GetPropertyUint32(ctx, arr, idx);
        auto elem_path = path + "[" + std::to_string(idx) + "]";
        if (JS_IsUndefined(elem)) {
            status = encode_error(
                CodecErrorCode::invalid_value,
                "STArray",
                "element must contain exactly one protocol field",
                elem_path);
            JS_FreeValue(ctx, elem);
            break;
        }
        if (!JS_IsObject(elem)) {
            status = encode_error(
                CodecErrorCode::invalid_value,
                "STArray",
                "element must be an object",
                elem_path);
            JS_FreeValue(ctx, elem);
            break;
        }

        // Each element is a wrapper object with one key
        JSPropertyEnum* etab = nullptr;
        uint32_t etab_len = 0;
        if (JS_GetOwnPropertyNames(ctx, &etab, &etab_len, elem,
                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) != 0) {
            JSValue exc = JS_GetException(ctx);
            JS_FreeValue(ctx, exc);
            status = encode_error(
                CodecErrorCode::invalid_value,
                "STArray",
                "element must be an object",
                elem_path);
            JS_FreeValue(ctx, elem);
            break;
        }

        uint32_t protocol_fields = 0;
        for (uint32_t j = 0; j < etab_len && status; ++j) {
            const char* key = JS_AtomToCString(ctx, etab[j].atom);
            if (!key) continue;

            auto field_opt = protocol.find_field(std::string(key));
            JS_FreeCString(ctx, key);
            if (!field_opt || !field_opt->meta.is_serialized)
                continue;

            ++protocol_fields;
            if (protocol_fields > 1) {
                status = encode_error(
                    CodecErrorCode::invalid_value,
                    "STArray",
                    "element must contain exactly one protocol field",
                    elem_path);
                break;
            }

            auto field_path = elem_path + "." + field_opt->name;
            JSValue inner = JS_GetProperty(ctx, elem, etab[j].atom);
            if (JS_IsUndefined(inner) || !JS_IsObject(inner)) {
                status = encode_error(
                    CodecErrorCode::invalid_value,
                    "STArray",
                    "element field must be object-valued",
                    field_path);
                JS_FreeValue(ctx, inner);
                break;
            }

            s.add_field_header(*field_opt);
            auto r = encode_stobject_js(
                s, ctx, inner, protocol, false, field_path);
            if (r)
                s.add_object_end_marker();
            else
                status = std::unexpected(std::move(r.error()));

            JS_FreeValue(ctx, inner);
        }

        if (status && protocol_fields != 1) {
            status = encode_error(
                CodecErrorCode::invalid_value,
                "STArray",
                "element must contain exactly one protocol field",
                elem_path);
        }

        for (uint32_t j = 0; j < etab_len; ++j)
            JS_FreeAtom(ctx, etab[j].atom);
        js_free(ctx, etab);
        JS_FreeValue(ctx, elem);
    }

    return status;
}

#undef XDATA_TRY

// ---------------------------------------------------------------------------
// Top-level convenience: serialize a JS object to bytes
// ---------------------------------------------------------------------------

// Errors-as-values entry point consumed by the bridge.
inline std::expected<std::vector<uint8_t>, CodecErrorValue>
serialize_object_js(
    JSContext* ctx,
    JSValue obj,
    Protocol const& protocol,
    bool only_signing = false)
{
    std::vector<uint8_t> buf;
    // Pre-size estimate: walk once for size, then encode
    // For simplicity, just let the vector grow dynamically.
    // A real production path could call field_value_encoded_size_js for pre-alloc.
    buf.reserve(256);
    VectorSink vs(buf);
    Serializer<VectorSink> ser(vs);
    if (auto r = encode_stobject_js(ser, ctx, obj, protocol, only_signing); !r)
        return std::unexpected(std::move(r.error()));
    return buf;
}

// Throwing facade for native / existing callers.
inline std::vector<uint8_t>
serialize_object_js_or_throw(
    JSContext* ctx,
    JSValue obj,
    Protocol const& protocol,
    bool only_signing = false)
{
    return encode_or_throw(
        serialize_object_js(ctx, obj, protocol, only_signing));
}

}  // namespace catl::xdata
