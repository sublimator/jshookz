#include "common.hpp"
#include "hook_imports.hpp"

namespace jshookz::provider::bindings {
namespace {

JSValue
js_state_get(JSContext *ctx, JSValueConst this_val,
             int argc, JSValueConst *argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "state.get: expected a key");
    BytesInput key;
    if (!get_hook_input(ctx, argv[0], &key))
        return JS_ThrowTypeError(ctx, "state.get: invalid key");

    /* Extended Hook state tops out at 16 * 256 bytes. A maximum-sized buffer
       preserves the fixed-buffer host contract: TOO_SMALL never means a
       truncated success. */
    uint8_t value[4096];
    int64_t result = hook_state(
        (uint32_t)(uintptr_t)value, sizeof(value),
        (uint32_t)(uintptr_t)key.ptr, key.len);
    free_bytes_input(ctx, &key);

    if (result == -5) /* DOESNT_EXIST is typed absence, not host failure. */
        return host_success(ctx, JS_UNDEFINED);
    if (result < 0)
        return host_failure(ctx, result);
    if ((uint64_t)result > sizeof(value))
        return JS_ThrowInternalError(
            ctx, "state.get: host returned oversized length %lld",
            (long long)result);
    return host_success(
        ctx, rich_from_bytes(ctx, "STBlob", value, (uint32_t)result));
}

JSValue
js_state_set(JSContext *ctx, JSValueConst this_val,
             int argc, JSValueConst *argv)
{
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "state.set: expected key and value");
    BytesInput key;
    if (!get_hook_input(ctx, argv[0], &key))
        return JS_ThrowTypeError(ctx, "state.set: invalid key");
    BytesInput value;
    if (!get_hook_input(ctx, argv[1], &value)) {
        free_bytes_input(ctx, &key);
        return JS_ThrowTypeError(ctx, "state.set: invalid value");
    }

    int64_t result = hook_state_set(
        (uint32_t)(uintptr_t)value.ptr, value.len,
        (uint32_t)(uintptr_t)key.ptr, key.len);
    free_bytes_input(ctx, &value);
    free_bytes_input(ctx, &key);
    return result < 0
        ? host_failure(ctx, result)
        : host_success(ctx, JS_UNDEFINED);
}

}  // namespace

void
registerState(JSContext *ctx, JSValue global)
{
    JSValue state = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, state, "get",
        JS_NewCFunction(ctx, js_state_get, "get", 1));
    JS_SetPropertyStr(ctx, state, "set",
        JS_NewCFunction(ctx, js_state_set, "set", 2));
    JS_SetPropertyStr(ctx, global, "state", state);
}

}  // namespace jshookz::provider::bindings
