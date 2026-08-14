#include "common.hpp"

#include <cstdlib>

namespace jshookz::provider::bindings {

int
get_bytes_input(JSContext *ctx, JSValueConst val, BytesInput *out)
{
    size_t offset, byte_len;
    out->ptr = NULL;
    out->len = 0;
    out->owned = NULL;
    out->cstring = NULL;
    out->ab_ref = JS_UNDEFINED;

    /* Try TypedArray */
    JSValue ab = JS_GetTypedArrayBuffer(ctx, val, &offset, &byte_len, NULL);
    if (!JS_IsException(ab)) {
        size_t buf_size;
        uint8_t *buf = JS_GetArrayBuffer(ctx, &buf_size, ab);
        if (buf) {
            out->ptr = buf + offset;
            out->len = (uint32_t)byte_len;
            out->ab_ref = ab;
            return 1;
        }
        JS_FreeValue(ctx, ab);
    } else {
        JSValue exc = JS_GetException(ctx);
        JS_FreeValue(ctx, exc);
    }

    /* Try ArrayBuffer */
    {
        size_t buf_size;
        uint8_t *buf = JS_GetArrayBuffer(ctx, &buf_size, val);
        if (buf) {
            out->ptr = buf;
            out->len = (uint32_t)buf_size;
            return 1;
        }
    }

    /* Try hex string */
    if (JS_IsString(val)) {
        size_t slen;
        const char *s = JS_ToCStringLen(ctx, &slen, val);
        if (!s || slen % 2 != 0) {
            if (s) JS_FreeCString(ctx, s);
            return 0;
        }
        uint32_t n = (uint32_t)(slen / 2);
        out->owned = (uint8_t *)js_malloc(ctx, n);
        if (!out->owned) { JS_FreeCString(ctx, s); return 0; }
        for (uint32_t i = 0; i < n; i++) {
            int hi, lo;
            char c;
            c = s[i * 2];
            hi = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            c = s[i * 2 + 1];
            lo = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (hi < 0 || lo < 0) {
                JS_FreeCString(ctx, s);
                js_free(ctx, out->owned);
                out->owned = NULL;
                return 0;
            }
            out->owned[i] = (uint8_t)((hi << 4) | lo);
        }
        JS_FreeCString(ctx, s);
        out->ptr = out->owned;
        out->len = n;
        return 1;
    }
    return 0;
}

void
free_bytes_input(JSContext *ctx, BytesInput *in)
{
    if (in->owned) { js_free(ctx, in->owned); in->owned = NULL; }
    if (in->cstring) { JS_FreeCString(ctx, in->cstring); in->cstring = NULL; }
    JS_FreeValue(ctx, in->ab_ref);
    in->ab_ref = JS_UNDEFINED;
}

int
get_hook_input(JSContext *ctx, JSValueConst val, BytesInput *out)
{
    if (JS_IsString(val)) {
        size_t len;
        const char *text = JS_ToCStringLen(ctx, &len, val);
        if (!text)
            return 0;
        out->ptr = (const uint8_t *)text;
        out->len = (uint32_t)len;
        out->owned = NULL;
        out->cstring = text;
        out->ab_ref = JS_UNDEFINED;
        return 1;
    }

    if (get_bytes_input(ctx, val, out))
        return 1;

    /* Rich binary values cross raw host calls through their canonical bytes. */
    JSValue to_bytes = JS_GetPropertyStr(ctx, val, "toBytes");
    if (!JS_IsFunction(ctx, to_bytes)) {
        JS_FreeValue(ctx, to_bytes);
        return 0;
    }
    JSValue bytes = JS_Call(ctx, to_bytes, val, 0, NULL);
    JS_FreeValue(ctx, to_bytes);
    if (JS_IsException(bytes))
        return 0;
    int ok = get_bytes_input(ctx, bytes, out);
    JS_FreeValue(ctx, bytes);
    return ok;
}

JSValue
host_success(JSContext *ctx, JSValue value)
{
    if (JS_IsException(value))
        return value;
    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "ok", JS_TRUE);
    JS_SetPropertyStr(ctx, result, "value", value);
    return result;
}

JSValue
host_failure(JSContext *ctx, int64_t code)
{
    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "ok", JS_FALSE);
    JS_SetPropertyStr(ctx, result, "code", JS_NewInt64(ctx, code));
    return result;
}

JSValue
rich_from_bytes(JSContext *ctx, const char *type_name,
                const uint8_t *bytes, uint32_t length)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue type = JS_GetPropertyStr(ctx, global, type_name);
    JSValue from = JS_GetPropertyStr(ctx, type, "from");
    JSValue input = make_uint8array(ctx, bytes, length);
    JSValue value = JS_Call(ctx, from, type, 1, &input);
    JS_FreeValue(ctx, input);
    JS_FreeValue(ctx, from);
    JS_FreeValue(ctx, type);
    JS_FreeValue(ctx, global);
    return value;
}

JSValue
make_uint8array(JSContext *ctx, const uint8_t *data, uint32_t len)
{
    JSValue ab = JS_NewArrayBufferCopy(ctx, data, len);
    if (JS_IsException(ab)) return ab;
    JSValue args[3] = { ab, JS_NewInt32(ctx, 0), JS_NewInt32(ctx, (int32_t)len) };
    JSValue ta = JS_NewTypedArray(ctx, 3, args, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, args[2]);
    return ta;
}

}  // namespace jshookz::provider::bindings
