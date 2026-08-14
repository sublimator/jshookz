#include "common.hpp"
#include "hook_imports.hpp"

#include <climits>

namespace jshookz::provider::bindings {
namespace {

JSValue
js_trace(JSContext *ctx, JSValueConst this_val,
         int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "trace: expected a string label");

    size_t label_len;
    const char *label = JS_ToCStringLen(ctx, &label_len, argv[0]);
    if (!label)
        return JS_EXCEPTION;

    JSValue rendered = JS_UNDEFINED;
    const char *data = "";
    size_t data_len = 0;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        rendered = JS_ToString(ctx, argv[1]);
        if (JS_IsException(rendered)) {
            JS_FreeCString(ctx, label);
            return rendered;
        }
        data = JS_ToCStringLen(ctx, &data_len, rendered);
        if (!data) {
            JS_FreeValue(ctx, rendered);
            JS_FreeCString(ctx, label);
            return JS_EXCEPTION;
        }
    }

    if (label_len > UINT32_MAX || data_len > UINT32_MAX) {
        if (data_len != 0) JS_FreeCString(ctx, data);
        JS_FreeValue(ctx, rendered);
        JS_FreeCString(ctx, label);
        return JS_ThrowRangeError(ctx, "trace: label or value is too large");
    }

    int64_t result = hook_trace(
        (uint32_t)(uintptr_t)label, (uint32_t)label_len,
        (uint32_t)(uintptr_t)data, (uint32_t)data_len, 0);
    if (!JS_IsUndefined(rendered)) JS_FreeCString(ctx, data);
    JS_FreeValue(ctx, rendered);
    JS_FreeCString(ctx, label);
    if (result < 0)
        return JS_ThrowInternalError(
            ctx, "trace: host returned %lld", (long long)result);
    return JS_UNDEFINED;
}

}  // namespace

void
registerTrace(JSContext *ctx, JSValue global)
{
    JS_SetPropertyStr(ctx, global, "trace",
        JS_NewCFunction(ctx, js_trace, "trace", 2));
}

}  // namespace jshookz::provider::bindings
