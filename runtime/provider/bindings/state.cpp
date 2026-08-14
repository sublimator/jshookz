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
    auto key = qjs::ByteView::get(
        ctx,
        argv[0],
        qjs::StringBytes::utf8,
        qjs::RichBytes::callToBytes);
    if (!key)
        return qjs::pendingOrTypeError(ctx, "state.get: invalid key");

    /* Extended Hook state tops out at 16 * 256 bytes. A maximum-sized buffer
       preserves the fixed-buffer host contract: TOO_SMALL never means a
       truncated success. */
    uint8_t value[4096];
    int64_t result = hook_state(
        (uint32_t)(uintptr_t)value, sizeof(value),
        (uint32_t)(uintptr_t)key.data(), key.size());

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
    auto key = qjs::ByteView::get(
        ctx,
        argv[0],
        qjs::StringBytes::utf8,
        qjs::RichBytes::callToBytes);
    if (!key)
        return qjs::pendingOrTypeError(ctx, "state.set: invalid key");
    /* Parsing a rich value executes its toBytes method. Snapshot the key so
       that method cannot detach or resize the key's ArrayBuffer underneath
       the subsequent host call. */
    if (!key.snapshot())
        return JS_EXCEPTION;
    auto value = qjs::ByteView::get(
        ctx,
        argv[1],
        qjs::StringBytes::utf8,
        qjs::RichBytes::callToBytes);
    if (!value)
        return qjs::pendingOrTypeError(ctx, "state.set: invalid value");

    int64_t result = hook_state_set(
        (uint32_t)(uintptr_t)value.data(), value.size(),
        (uint32_t)(uintptr_t)key.data(), key.size());
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
