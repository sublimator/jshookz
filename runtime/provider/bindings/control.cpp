#include "common.hpp"
#include "hook_imports.hpp"

#include <cstdio>

namespace jshookz::provider::bindings {
namespace {

JSValue
js_hook_accept(JSContext *ctx, JSValueConst this_val,
               int argc, JSValueConst *argv)
{
    int64_t code = 0;
    if (argc > 1 && !JS_IsUndefined(argv[1]) &&
        JS_ToInt64(ctx, &code, argv[1]) < 0)
        return JS_EXCEPTION;

    if (argc == 0 || JS_IsUndefined(argv[0]))
        return JS_NewInt64(ctx, hook_accept(0, 0, code));

    /* A lifecycle message string is UTF-8 text, not the hex-string shorthand
       accepted by BytesLike APIs. */
    if (JS_IsString(argv[0])) {
        size_t len;
        const char *message = JS_ToCStringLen(ctx, &len, argv[0]);
        if (!message)
            return JS_EXCEPTION;
        int64_t result = hook_accept(
            (uint32_t)(uintptr_t)message, (uint32_t)len, code);
        JS_FreeCString(ctx, message);
        return JS_NewInt64(ctx, result);
    }

    BytesInput message;
    if (!get_bytes_input(ctx, argv[0], &message))
        return JS_ThrowTypeError(
            ctx, "accept: expected string, Uint8Array, or ArrayBuffer");
    int64_t result = hook_accept(
        (uint32_t)(uintptr_t)message.ptr, message.len, code);
    free_bytes_input(ctx, &message);
    return JS_NewInt64(ctx, result);
}

JSValue
js_hook_rollback(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv)
{
    int64_t code = 0;
    if (argc > 1 && !JS_IsUndefined(argv[1]) &&
        JS_ToInt64(ctx, &code, argv[1]) < 0)
        return JS_EXCEPTION;
    if (argc == 0 || JS_IsUndefined(argv[0]))
        return JS_NewInt64(ctx, hook_rollback(0, 0, code));

    BytesInput message;
    if (!get_hook_input(ctx, argv[0], &message))
        return JS_ThrowTypeError(
            ctx, "rollback: expected string, Uint8Array, or ArrayBuffer");
    int64_t result = hook_rollback(
        (uint32_t)(uintptr_t)message.ptr, message.len, code);
    free_bytes_input(ctx, &message);
    return JS_NewInt64(ctx, result);
}

int
get_result_success(JSContext *ctx, JSValueConst value,
                   const char *function_name)
{
    if (!JS_IsObject(value)) {
        JS_ThrowTypeError(ctx, "%s: expected Result", function_name);
        return -1;
    }

    JSValue ok = JS_GetPropertyStr(ctx, value, "ok");
    if (JS_IsException(ok))
        return -1;
    if (!JS_IsBool(ok)) {
        JS_FreeValue(ctx, ok);
        JS_ThrowTypeError(ctx, "%s: expected boolean ok", function_name);
        return -1;
    }
    int success = JS_ToBool(ctx, ok);
    JS_FreeValue(ctx, ok);
    return success;
}

JSValue
js_rollback_on_fail(JSContext *ctx, JSValueConst this_val,
                    int argc, JSValueConst *argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "rollback.onFail: expected Result");
    int const success =
        get_result_success(ctx, argv[0], "rollback.onFail");
    if (success < 0)
        return JS_EXCEPTION;
    if (success)
        return JS_GetPropertyStr(ctx, argv[0], "value");

    int64_t code;
    int const explicit_code = argc > 2 && !JS_IsUndefined(argv[2]);
    if (explicit_code) {
        if (argc < 2 || JS_IsUndefined(argv[1]))
            return JS_ThrowTypeError(
                ctx,
                "rollback.onFail: uncoded Result requires message and code");
        if (!JS_IsNumber(argv[2]) || JS_ToInt64(ctx, &code, argv[2]) < 0)
            return JS_ThrowTypeError(
                ctx, "rollback.onFail: expected numeric rollback code");
    } else {
        JSValue code_value = JS_GetPropertyStr(ctx, argv[0], "code");
        if (JS_IsException(code_value))
            return code_value;
        if (!JS_IsNumber(code_value) ||
            JS_ToInt64(ctx, &code, code_value) < 0) {
            JS_FreeValue(ctx, code_value);
            return JS_ThrowTypeError(
                ctx,
                "rollback.onFail: uncoded Result requires explicit code");
        }
        JS_FreeValue(ctx, code_value);
    }

    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        JSValue code_arg = JS_NewInt64(ctx, code);
        JSValueConst rollback_args[2] = { argv[1], code_arg };
        JSValue result = js_hook_rollback(ctx, this_val, 2, rollback_args);
        JS_FreeValue(ctx, code_arg);
        return result;
    }

    char message[64];
    int length = snprintf(
        message, sizeof(message), "host operation failed: %lld",
        (long long)code);
    if (length < 0 || (size_t)length >= sizeof(message))
        return JS_ThrowInternalError(
            ctx, "rollback.onFail: failed to format status");
    return JS_NewInt64(ctx, hook_rollback(
        (uint32_t)(uintptr_t)message, (uint32_t)length, code));
}

JSValue
js_rollback_require(JSContext *ctx, JSValueConst this_val,
                    int argc, JSValueConst *argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "rollback.require: expected Result");
    int const success =
        get_result_success(ctx, argv[0], "rollback.require");
    if (success < 0)
        return JS_EXCEPTION;
    if (success) {
        JSValue value = JS_GetPropertyStr(ctx, argv[0], "value");
        if (JS_IsException(value) || !JS_IsUndefined(value))
            return value;
        JS_FreeValue(ctx, value);
    }

    if (argc < 3 || JS_IsUndefined(argv[1]) ||
        !JS_IsNumber(argv[2]))
        return JS_ThrowTypeError(
            ctx, "rollback.require: expected message and numeric code");
    JSValueConst rollback_args[2] = { argv[1], argv[2] };
    return js_hook_rollback(ctx, this_val, 2, rollback_args);
}

}  // namespace

void
registerControl(JSContext *ctx, JSValue global)
{
    JS_SetPropertyStr(ctx, global, "accept",
        JS_NewCFunction(ctx, js_hook_accept, "accept", 2));
    JSValue rollback = JS_NewCFunction(ctx, js_hook_rollback, "rollback", 2);
    JS_SetPropertyStr(ctx, rollback, "onFail",
        JS_NewCFunction(ctx, js_rollback_on_fail, "onFail", 3));
    JS_SetPropertyStr(ctx, rollback, "require",
        JS_NewCFunction(ctx, js_rollback_require, "require", 3));
    JS_SetPropertyStr(ctx, global, "rollback", rollback);
}

}  // namespace jshookz::provider::bindings
