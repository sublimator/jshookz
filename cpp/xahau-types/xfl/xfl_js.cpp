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

XFL const*
xflValueNoThrow(JSValueConst value) noexcept
{
    if (!JS_IsObject(value) || JS_GetClassID(value) != js_xfl_class_id)
        return nullptr;
    return static_cast<XFL const*>(JS_GetOpaque(value, js_xfl_class_id));
}

int
xflSign(XFL const& value) noexcept
{
    if (value.is_zero())
        return 0;
    return value.is_negative() ? -1 : 1;
}

int
compareXFL(XFL const& left, XFL const& right) noexcept
{
    int const leftSign = xflSign(left);
    int const rightSign = xflSign(right);
    if (leftSign != rightSign)
        return leftSign < rightSign ? -1 : 1;
    if (leftSign == 0)
        return 0;

    int order = 0;
    if (left.exponent() != right.exponent())
        order = left.exponent() < right.exponent() ? -1 : 1;
    else if (left.mantissa() != right.mantissa())
        order = left.mantissa() < right.mantissa() ? -1 : 1;
    return leftSign < 0 ? -order : order;
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

// @binding provider:XFLDecimal.sign
JSValue
js_xfl_sign(JSContext* ctx, JSValueConst this_val, int, JSValueConst*)
{
    auto const* x = qjs::opaque<XFL>(ctx, this_val, js_xfl_class_id);
    if (!x)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, xflSign(*x));
}

// @binding provider:XFLDecimal.negate
JSValue
js_xfl_negate(JSContext* ctx, JSValueConst this_val, int, JSValueConst*)
{
    auto const* x = qjs::opaque<XFL>(ctx, this_val, js_xfl_class_id);
    if (!x)
        return JS_EXCEPTION;
    if (x->is_zero())
        return nativeNew<XFL>(ctx, js_xfl_class_id, XFL{});
    return nativeNew<XFL>(
        ctx,
        js_xfl_class_id,
        XFL::from_components(
            !x->is_negative(),
            x->exponent(),
            static_cast<std::int64_t>(x->mantissa())));
}

// @binding provider:XFLDecimal.equals
JSValue
js_xfl_equals(
    JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    auto const* left = qjs::opaque<XFL>(ctx, this_val, js_xfl_class_id);
    if (!left)
        return JS_EXCEPTION;
    auto const* right = argc > 0 ? xflValueNoThrow(argv[0]) : nullptr;
    return JS_NewBool(ctx, right != nullptr && left->raw() == right->raw());
}

// @binding provider:XFLDecimal.compare
JSValue
js_xfl_compare(
    JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    auto const* left = qjs::opaque<XFL>(ctx, this_val, js_xfl_class_id);
    if (!left)
        return JS_EXCEPTION;
    auto const* right = argc > 0 ? xflValueNoThrow(argv[0]) : nullptr;
    if (!right)
        return JS_ThrowTypeError(
            ctx, "XFLDecimal.compare() expects XFLDecimal");
    return JS_NewInt32(ctx, compareXFL(*left, *right));
}

//@@impl XFLDecimal
JSCFunctionListEntry const proto[] = {
    JS_CFUNC_DEF("isNegative", 0, js_xfl_is_negative),
    JS_CFUNC_DEF("isZero", 0, js_xfl_is_zero),
    JS_CFUNC_DEF("sign", 0, js_xfl_sign),
    JS_CFUNC_DEF("negate", 0, js_xfl_negate),
    JS_CFUNC_DEF("equals", 1, js_xfl_equals),
    JS_CFUNC_DEF("compare", 1, js_xfl_compare),
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
    return xflValueNoThrow(value) != nullptr;
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
