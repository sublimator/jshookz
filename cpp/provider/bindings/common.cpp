#include "common.hpp"

namespace jshookz::provider::bindings {

JSValue
host_success(JSContext *ctx, JSValue value)
{
    return result_success(ctx, value);
}

JSValue
host_failure(JSContext *ctx, int64_t code)
{
    qjs::OwnedValue error(ctx, result_error(ctx, "host"));
    if (error.isException())
        return error.release();
    if (JS_DefinePropertyValueStr(
            ctx,
            error.get(),
            "code",
            JS_NewInt64(ctx, code),
            JS_PROP_ENUMERABLE) < 0 ||
        !result_finish(ctx, error.get()))
        return JS_EXCEPTION;
    return result_failure(ctx, error.release());
}

JSValue
host_effect_success(JSContext* ctx)
{
    return effect_success(ctx);
}

JSValue
host_effect_failure(JSContext* ctx, int64_t code)
{
    qjs::OwnedValue error(ctx, result_error(ctx, "host"));
    if (error.isException())
        return error.release();
    if (JS_DefinePropertyValueStr(
            ctx,
            error.get(),
            "code",
            JS_NewInt64(ctx, code),
            JS_PROP_ENUMERABLE) < 0 ||
        !result_finish(ctx, error.get()))
        return JS_EXCEPTION;
    return result_failure(ctx, error.release(), true);
}

}  // namespace jshookz::provider::bindings
