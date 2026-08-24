#include "js.hpp"
#include "xfl/xfl.hpp"

namespace jshookz::provider::types {
namespace qjs = jshookz::provider::qjs;
using hook::XFL;

namespace {

JSClassID js_xfl_class_id;

constexpr std::uint64_t xflMinMagnitude = 1000000000000000ull;
constexpr std::uint64_t xflMaxMagnitude = 9999999999999999ull;
constexpr std::int32_t xflMinExponent = -96;
constexpr std::int32_t xflMaxExponent = 80;

bool
validDecimalParts(std::uint64_t magnitude, std::int32_t exponent) noexcept
{
    return magnitude >= xflMinMagnitude && magnitude <= xflMaxMagnitude &&
        exponent >= xflMinExponent && exponent <= xflMaxExponent;
}

void
js_xfl_finalizer(JSRuntime* rt, JSValue val)
{
    qjs::destroyOpaque<XFL>(rt, val, js_xfl_class_id);
}

JSClassDef js_xfl_class = {
    .class_name = "XFLDecimal",
    .finalizer = js_xfl_finalizer,
};

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
    JS_CFUNC_DEF("isNegative", 0, js_xfl_is_negative),
    JS_CFUNC_DEF("isZero", 0, js_xfl_is_zero),
};

}  // namespace

bool
registerXFL(JSContext* ctx)
{
    return registerHiddenClass(
        ctx, &js_xfl_class_id, &js_xfl_class, proto);
}

bool
isXFLDecimal(JSValueConst value) noexcept
{
    return JS_IsObject(value) && JS_GetClassID(value) == js_xfl_class_id &&
        JS_GetOpaque(value, js_xfl_class_id) != nullptr;
}

JSValue
makeXFLDecimalParts(
    JSContext* ctx,
    bool negative,
    std::uint64_t magnitude,
    std::int32_t exponent)
{
    // Amount and XFLDecimal share this closed decimal domain. Zero is the one
    // exception to component encoding: all zero-parts spellings mint raw 0.
    if (magnitude == 0)
        return nativeNew<XFL>(ctx, js_xfl_class_id, XFL{});
    if (!validDecimalParts(magnitude, exponent))
        return JS_ThrowInternalError(
            ctx, "XFLDecimal construction received invalid decimal parts");
    return nativeNew<XFL>(
        ctx,
        js_xfl_class_id,
        XFL::from_components(
            negative, exponent, static_cast<std::int64_t>(magnitude)));
}

bool
readXFLDecimalParts(
    JSContext*,
    JSValueConst input,
    bool* negative,
    std::uint64_t* magnitude,
    std::int32_t* exponent) noexcept
{
    if (negative != nullptr)
        *negative = false;
    if (magnitude != nullptr)
        *magnitude = 0;
    if (exponent != nullptr)
        *exponent = 0;
    if (negative == nullptr || magnitude == nullptr || exponent == nullptr ||
        !JS_IsObject(input) || JS_GetClassID(input) != js_xfl_class_id)
        return false;
    auto const* decimal =
        static_cast<XFL const*>(JS_GetOpaque(input, js_xfl_class_id));
    if (decimal == nullptr)
        return false;

    // Every instance is provider-minted from canonical decimal parts. Keep the
    // read seam defensive so a corrupted opaque cannot cross into Amount.
    if (decimal->raw() == 0)
        return true;
    std::uint64_t const decodedMagnitude = decimal->mantissa();
    std::int32_t const decodedExponent = decimal->exponent();
    if (decimal->raw() < 0 ||
        !validDecimalParts(decodedMagnitude, decodedExponent))
        return false;
    bool const decodedNegative = decimal->is_negative();
    if (XFL::from_components(
            decodedNegative,
            decodedExponent,
            static_cast<std::int64_t>(decodedMagnitude))
            .raw() != decimal->raw())
        return false;

    *negative = decodedNegative;
    *magnitude = decodedMagnitude;
    *exponent = decodedExponent;
    return true;
}

}  // namespace jshookz::provider::types
