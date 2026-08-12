// bridge_xdata.cpp — x-data codec backend for decode_object/encode_object
//
// Drop-in replacement for bridge.cpp. Uses catl::xdata (lightweight,
// definitions-driven codec) instead of xahaud's STObject.
//
// Same JS API: decode_object(hex) → JS object, encode_object(json) → hex

extern "C" {
#include "../../engine/quickjs/quickjs.h"
}

#include "qjs_visitor.h"
#include "qjs_encode.h"
#include <catl/xdata/protocol.h>
#include <catl/xdata/parser.h>
#include <catl/xdata/parser-context.h>
#include <catl/xdata/hex.h>

#include <cstdint>
#include <string>
#include <vector>

// Protocol singleton — loaded once, reused for all decode/encode calls.
static catl::xdata::Protocol* g_protocol = nullptr;

static catl::xdata::Protocol&
get_protocol()
{
    if (!g_protocol)
    {
        // Heap-allocate so it persists (Wizer will snapshot this)
        g_protocol = new catl::xdata::Protocol(
            catl::xdata::Protocol::load_embedded_xahau_protocol());
    }
    return *g_protocol;
}

// --- Hex utilities ---

static bool
hex_decode(const char* hex, size_t len, std::vector<uint8_t>& out)
{
    if (len % 2 != 0)
        return false;
    out.resize(len / 2);
    for (size_t i = 0; i < len; i += 2)
    {
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int hi = nibble(hex[i]);
        int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out[i / 2] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static std::string
hex_encode(const uint8_t* data, size_t len)
{
    static const char hex_chars[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(len * 2);
    for (size_t i = 0; i < len; i++)
    {
        result += hex_chars[(data[i] >> 4) & 0x0F];
        result += hex_chars[data[i] & 0x0F];
    }
    return result;
}

// --- decode_object(hex) → JS object ---

static JSValue
js_decode_object(JSContext* ctx, JSValueConst this_val,
                 int argc, JSValueConst* argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "decode_object requires hex string, Uint8Array, or ArrayBuffer");

    const uint8_t* input_ptr = nullptr;
    size_t input_len = 0;
    std::vector<uint8_t> owned_bytes;  // used by decoded hex strings and Array<number>

    // Try TypedArray first
    {
        size_t off, len, bpe;
        JSValue buf = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &len, &bpe);
        if (!JS_IsException(buf)) {
            size_t buf_size;
            uint8_t* data = JS_GetArrayBuffer(ctx, &buf_size, buf);
            if (data && bpe == 1) {
                input_ptr = data + off;
                input_len = len;
            }
            JS_FreeValue(ctx, buf);
        }
    }

    // Try ArrayBuffer
    if (!input_ptr) {
        size_t buf_size;
        uint8_t* data = JS_GetArrayBuffer(ctx, &buf_size, argv[0]);
        if (data) {
            input_ptr = data;
            input_len = buf_size;
        }
    }

    // Try hex string
    if (!input_ptr && JS_IsString(argv[0])) {
        const char* hex = JS_ToCString(ctx, argv[0]);
        if (!hex)
            return JS_EXCEPTION;
        size_t hex_len = strlen(hex);
        if (!hex_decode(hex, hex_len, owned_bytes)) {
            JS_FreeCString(ctx, hex);
            return JS_ThrowTypeError(ctx, "invalid hex string");
        }
        JS_FreeCString(ctx, hex);
        input_ptr = owned_bytes.data();
        input_len = owned_bytes.size();
    }

    // Try Array<number>
    if (!input_ptr && JS_IsArray(ctx, argv[0])) {
        JSValue len_val = JS_GetPropertyStr(ctx, argv[0], "length");
        int64_t n = 0;
        JS_ToInt64(ctx, &n, len_val);
        JS_FreeValue(ctx, len_val);
        if (n > 0 && n <= 1024 * 64) {
            owned_bytes.resize(n);
            for (int64_t i = 0; i < n; i++) {
                JSValue elem = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
                int64_t byte;
                JS_ToInt64(ctx, &byte, elem);
                JS_FreeValue(ctx, elem);
                if (byte < 0 || byte > 255)
                    return JS_ThrowTypeError(ctx, "decode_object: array elements must be 0-255");
                owned_bytes[i] = (uint8_t)byte;
            }
            input_ptr = owned_bytes.data();
            input_len = n;
        }
    }

    if (!input_ptr) {
        return JS_ThrowTypeError(ctx, "decode_object: expected Uint8Array, ArrayBuffer, hex string, or Array<number>");
    }

    auto& proto = get_protocol();
    //@@start live-decode
    auto offsets_result =
        catl::xdata::build_field_offsets_expected(input_ptr, input_len, proto);
    if (!offsets_result) {
        return JS_ThrowTypeError(
            ctx,
            "decode_object failed: %s",
            offsets_result.error().message.c_str());
    }

    // Create a lazy STObject: build offset map, store bytes, decode on demand.
    JSValue result = catl::xdata::STObjectClass::new_instance(ctx);
    if (JS_IsException(result)) return result;

    auto* obj_data = static_cast<catl::xdata::STObjectData*>(
        JS_GetOpaque(result, catl::xdata::STObjectClass::class_id));

    if (obj_data) {
        JSValue ab = JS_NewArrayBufferCopy(ctx, input_ptr, input_len);
        if (JS_IsException(ab)) {
            JS_FreeValue(ctx, result);
            return ab;
        }

        size_t buf_size;
        uint8_t* copied = JS_GetArrayBuffer(ctx, &buf_size, ab);
        if (!copied) {
            JS_FreeValue(ctx, ab);
            JS_FreeValue(ctx, result);
            return JS_ThrowTypeError(ctx, "decode_object failed: could not store bytes");
        }

        obj_data->bytes = ab;  // takes ownership (prevents GC)
        obj_data->protocol = &proto;
        obj_data->raw_ptr = copied;
        obj_data->raw_len = buf_size;
        obj_data->offsets = std::move(*offsets_result);
    }
    //@@end live-decode

    return result;
}

// --- encode_object(json_string) → hex string ---

static JSValue
js_encode_object(JSContext* ctx, JSValueConst this_val,
                 int argc, JSValueConst* argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "encode_object requires a JSON string");

    // Accept either a JS object or a JSON string
    JSValue obj;
    bool free_obj = false;

    if (JS_IsString(argv[0])) {
        // JSON string → parse to JS object first
        size_t len;
        const char* str = JS_ToCStringLen(ctx, &len, argv[0]);
        if (!str) return JS_EXCEPTION;
        obj = JS_ParseJSON(ctx, str, len, "<encode_object>");
        JS_FreeCString(ctx, str);
        if (JS_IsException(obj)) return JS_EXCEPTION;
        free_obj = true;
    } else if (JS_IsObject(argv[0])) {
        obj = argv[0];
    } else {
        return JS_ThrowTypeError(ctx, "encode_object: expected object or JSON string");
    }

    //@@start live-fast-path
    // Fast path: only an unmodified root STObject may return its original
    // bytes. Nested views also hold the root ArrayBuffer, so they must never
    // use this path.
    {
        auto* data = static_cast<catl::xdata::STObjectData*>(
            JS_GetOpaque(obj, catl::xdata::STObjectClass::class_id));
        if (data && JS_IsUndefined(data->root_obj) &&
            !JS_IsUndefined(data->bytes) && !data->subtree_dirty) {
            size_t len = 0;
            uint8_t* bytes = JS_GetArrayBuffer(ctx, &len, data->bytes);
            if (bytes || len == 0) {
                JSValue ab = JS_NewArrayBufferCopy(ctx, bytes, len);
                if (free_obj) JS_FreeValue(ctx, obj);
                if (JS_IsException(ab)) return ab;
                JSValue global = JS_GetGlobalObject(ctx);
                JSValue u8ctor = JS_GetPropertyStr(ctx, global, "Uint8Array");
                JSValue u8args[] = { ab };
                JSValue result = JS_CallConstructor(ctx, u8ctor, 1, u8args);
                JS_FreeValue(ctx, u8ctor);
                JS_FreeValue(ctx, global);
                JS_FreeValue(ctx, ab);
                return result;
            }
            JSValue probe_error = JS_GetException(ctx);
            JS_FreeValue(ctx, probe_error);
        }
    }
    //@@end live-fast-path

    auto& proto = get_protocol();
    // Slow path: full encode from JS properties. Errors-as-values — the
    // encoder never throws on the wasm build; invalid input becomes a clean
    // JS TypeError rather than a trap.
    auto encoded_result = catl::xdata::serialize_object_js(ctx, obj, proto);

    if (free_obj) JS_FreeValue(ctx, obj);

    if (!encoded_result) {
        return JS_ThrowTypeError(
            ctx, "encode_object failed: %s",
            encoded_result.error().message.c_str());
    }
    auto& encoded = *encoded_result;

    // Return as Uint8Array
    JSValue ab = JS_NewArrayBufferCopy(ctx, encoded.data(), encoded.size());
    if (JS_IsException(ab)) return ab;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue u8ctor = JS_GetPropertyStr(ctx, global, "Uint8Array");
    JSValue u8args[] = { ab };
    JSValue result = JS_CallConstructor(ctx, u8ctor, 1, u8args);
    JS_FreeValue(ctx, u8ctor);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, ab);
    return result;
}

// --- util_hex(BytesLike) → hex string ---

static JSValue
js_util_hex(JSContext* ctx, JSValueConst this_val,
            int argc, JSValueConst* argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "util_hex requires a BytesLike argument");

    // Try TypedArray
    {
        size_t off, len, bpe;
        JSValue buf = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &len, &bpe);
        if (!JS_IsException(buf)) {
            size_t buf_size;
            uint8_t* data = JS_GetArrayBuffer(ctx, &buf_size, buf);
            JS_FreeValue(ctx, buf);
            if (data) {
                std::string hex = hex_encode(data + off, len);
                return JS_NewStringLen(ctx, hex.c_str(), hex.size());
            }
        }
    }

    // Try ArrayBuffer
    {
        size_t buf_size;
        uint8_t* data = JS_GetArrayBuffer(ctx, &buf_size, argv[0]);
        if (data) {
            std::string hex = hex_encode(data, buf_size);
            return JS_NewStringLen(ctx, hex.c_str(), hex.size());
        }
    }

    return JS_ThrowTypeError(ctx, "util_hex: expected Uint8Array or ArrayBuffer");
}

// --- Registration ---

extern "C" void
register_protocol_functions(JSContext* ctx)
{
    // Init exotic classes
    catl::xdata::STObjectClass::init(ctx);
    catl::xdata::STArrayClass::init(ctx);

    // Pre-load protocol definitions (snapshotted by Wizer)
    get_protocol();

    JSValue global = JS_GetGlobalObject(ctx);

    // Expose STObject as a global so JS can do `tx instanceof STObject`
    JSValue ctor = JS_NewCFunction2(ctx, [](JSContext* c, JSValueConst, int, JSValueConst*) -> JSValue {
        return catl::xdata::STObjectClass::new_instance(c);
    }, "STObject", 0, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, ctor, catl::xdata::STObjectClass::prototype);
    JS_SetPropertyStr(ctx, global, "STObject", ctor);

    JS_SetPropertyStr(ctx, global, "decode_object",
        JS_NewCFunction(ctx, js_decode_object, "decode_object", 1));
    JS_SetPropertyStr(ctx, global, "encode_object",
        JS_NewCFunction(ctx, js_encode_object, "encode_object", 1));
    JS_SetPropertyStr(ctx, global, "util_hex",
        JS_NewCFunction(ctx, js_util_hex, "util_hex", 1));
    JS_FreeValue(ctx, global);
}
