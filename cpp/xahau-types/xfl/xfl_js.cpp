#include "js.hpp"
#include "xfl/xfl.hpp"

namespace jshookz::provider::types {
namespace qjs = jshookz::provider::qjs;
using hook::XFL;

namespace {

JSClassID js_xfl_class_id;

void
js_xfl_finalizer(JSRuntime* rt, JSValue val)
{
    qjs::destroyOpaque<XFL>(rt, val, js_xfl_class_id);
}

JSClassDef js_xfl_class = {
    .class_name = "XFLDecimal",
    .finalizer = js_xfl_finalizer,
};

// @binding provider:XFLDecimal.fromRaw
JSValue
js_xfl_from_raw(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "XFLDecimal.fromRaw() expects a value");
    if (!JS_IsBigInt(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "XFLDecimal.fromRaw() expects a bigint");
    int64_t raw;
    if (JS_ToBigInt64(ctx, &raw, argv[0]))
        return JS_EXCEPTION;
    qjs::OwnedValue roundTrip(ctx, JS_NewBigInt64(ctx, raw));
    int const exact = JS_StrictEq(ctx, argv[0], roundTrip.get());
    if (exact < 0)
        return JS_EXCEPTION;
    if (raw < 0 || !exact)
        return JS_ThrowRangeError(
            ctx, "XFLDecimal.fromRaw() expects a non-negative int64 encoding");
    return nativeNew<XFL>(ctx, js_xfl_class_id, XFL(raw));
}

// @binding provider:XFLDecimal.raw
JSValue
js_xfl_raw(JSContext* ctx, JSValueConst this_val)
{
    auto* x = qjs::opaque<XFL>(ctx, this_val, js_xfl_class_id);
    if (!x)
        return JS_EXCEPTION;
    return JS_NewBigInt64(ctx, x->raw());
}

// @binding provider:XFLDecimal.mantissa
JSValue
js_xfl_mantissa(JSContext* ctx, JSValueConst this_val, int, JSValueConst*)
{
    auto* x = qjs::opaque<XFL>(ctx, this_val, js_xfl_class_id);
    if (!x)
        return JS_EXCEPTION;
    return JS_NewBigInt64(ctx, static_cast<std::int64_t>(x->mantissa()));
}

// @binding provider:XFLDecimal.exponent
JSValue
js_xfl_exponent(JSContext* ctx, JSValueConst this_val, int, JSValueConst*)
{
    auto* x = qjs::opaque<XFL>(ctx, this_val, js_xfl_class_id);
    if (!x)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, x->exponent());
}

// @binding provider:XFLDecimal.isNegative
JSValue
js_xfl_is_negative(JSContext* ctx, JSValueConst this_val, int, JSValueConst*)
{
    auto* x = qjs::opaque<XFL>(ctx, this_val, js_xfl_class_id);
    if (!x)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, x->is_negative());
}

// @binding provider:XFLDecimal.isZero
JSValue
js_xfl_is_zero(JSContext* ctx, JSValueConst this_val, int, JSValueConst*)
{
    auto* x = qjs::opaque<XFL>(ctx, this_val, js_xfl_class_id);
    if (!x)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, x->is_zero());
}

//@@impl XFLDecimal
JSCFunctionListEntry const proto[] = {
    JS_CGETSET_DEF("raw", js_xfl_raw, NULL),
    JS_CFUNC_DEF("mantissa", 0, js_xfl_mantissa),
    JS_CFUNC_DEF("exponent", 0, js_xfl_exponent),
    JS_CFUNC_DEF("isNegative", 0, js_xfl_is_negative),
    JS_CFUNC_DEF("isZero", 0, js_xfl_is_zero),
};

//@@impl XFLDecimalDecimal static
JSCFunctionListEntry const statics[] = {
    JS_CFUNC_DEF("fromRaw", 1, js_xfl_from_raw),
};

}  // namespace

bool
registerXFL(JSContext* ctx, JSValueConst global)
{
    return registerClass(
        ctx, global, "XFLDecimal", &js_xfl_class_id, &js_xfl_class, proto, statics);
}

}  // namespace jshookz::provider::types
