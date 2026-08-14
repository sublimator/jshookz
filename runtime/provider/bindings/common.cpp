#include "common.hpp"

namespace jshookz::provider::bindings {

JSValue
host_success(JSContext *ctx, JSValue value)
{
    if (JS_IsException(value))
        return value;
    qjs::OwnedValue ownedValue(ctx, value);
    qjs::OwnedValue result(ctx, JS_NewObject(ctx));
    if (result.isException())
        return result.release();
    if (JS_SetPropertyStr(ctx, result.get(), "ok", JS_TRUE) < 0)
        return JS_EXCEPTION;
    if (JS_SetPropertyStr(
            ctx, result.get(), "value", ownedValue.release()) < 0)
        return JS_EXCEPTION;
    return result.release();
}

JSValue
host_failure(JSContext *ctx, int64_t code)
{
    qjs::OwnedValue result(ctx, JS_NewObject(ctx));
    if (result.isException())
        return result.release();
    if (JS_SetPropertyStr(ctx, result.get(), "ok", JS_FALSE) < 0)
        return JS_EXCEPTION;
    if (JS_SetPropertyStr(
            ctx, result.get(), "code", JS_NewInt64(ctx, code)) < 0)
        return JS_EXCEPTION;
    return result.release();
}

JSValue
rich_from_bytes(JSContext *ctx, const char *type_name,
                const uint8_t *bytes, uint32_t length)
{
    qjs::OwnedValue global(ctx, JS_GetGlobalObject(ctx));
    if (global.isException())
        return global.release();
    qjs::OwnedValue type = qjs::property(ctx, global.get(), type_name);
    if (type.isException())
        return type.release();
    qjs::OwnedValue from = qjs::property(ctx, type.get(), "from");
    if (from.isException())
        return from.release();
    qjs::OwnedValue input(
        ctx,
        qjs::uint8Array(
            ctx, std::span<std::uint8_t const>{bytes, length}));
    if (input.isException())
        return input.release();
    JSValueConst args[1] = {input.get()};
    return JS_Call(ctx, from.get(), type.get(), 1, args);
}

}  // namespace jshookz::provider::bindings
