#include "js.hpp"
#include "result.hpp"
#include "runtime_profile_limits.h"
#include "xfl/xfl_arithmetic.hpp"
#include "xfl/xfl.hpp"
#include "xfl/xfl_profile_context.hpp"

#include <array>
#include <cmath>
#include <limits>

namespace jshookz::provider::types {
namespace qjs = jshookz::provider::qjs;
using hook::XFL;
namespace profile = catl::xdata::xahau_profile;

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

bool
validRawDecimal(std::int64_t raw) noexcept
{
    if (raw == 0)
        return true;
    if (raw < 0)
        return false;
    XFL const decimal(raw);
    std::uint64_t const magnitude = decimal.mantissa();
    std::int32_t const exponent = decimal.exponent();
    return validDecimalParts(magnitude, exponent) &&
        XFL::from_components(
            decimal.is_negative(),
            exponent,
            static_cast<std::int64_t>(magnitude))
                .raw() == raw;
}

enum class SignedInputStatus : std::uint8_t
{
    valid,
    invalid,
    exception,
};

SignedInputStatus
readSignedMantissa(
    JSContext* ctx, JSValueConst input, std::int64_t& output)
{
    output = 0;
    if (JS_IsNumber(input)) {
        double number = 0;
        if (JS_ToFloat64(ctx, &number, input) < 0)
            return SignedInputStatus::exception;
        constexpr double maxSafeInteger = 9007199254740991.0;
        if (!std::isfinite(number) || std::trunc(number) != number ||
            number < -maxSafeInteger || number > maxSafeInteger)
            return SignedInputStatus::invalid;
        output = static_cast<std::int64_t>(number);
        return SignedInputStatus::valid;
    }
    if (!JS_IsBigInt(ctx, input))
        return SignedInputStatus::invalid;

    qjs::OwnedValue rendered(ctx, JS_ToString(ctx, input));
    if (rendered.isException())
        return SignedInputStatus::exception;
    std::size_t length = 0;
    char const* text = JS_ToCStringLen(ctx, &length, rendered.get());
    if (text == nullptr)
        return SignedInputStatus::exception;
    bool const negative = length > 0 && text[0] == '-';
    std::size_t const first = negative ? 1 : 0;
    bool valid = first < length;
    constexpr std::uint64_t positiveLimit =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    constexpr std::uint64_t negativeLimit = positiveLimit + 1;
    std::uint64_t const limit = negative ? negativeLimit : positiveLimit;
    std::uint64_t magnitude = 0;
    for (std::size_t index = first; valid && index < length; ++index) {
        char const digit = text[index];
        valid = digit >= '0' && digit <= '9';
        if (!valid)
            break;
        std::uint64_t const part = static_cast<std::uint64_t>(digit - '0');
        if (magnitude > (limit - part) / 10) {
            valid = false;
            break;
        }
        magnitude = magnitude * 10 + part;
    }
    JS_FreeCString(ctx, text);
    if (!valid)
        return SignedInputStatus::invalid;
    if (negative && magnitude == negativeLimit)
        output = std::numeric_limits<std::int64_t>::min();
    else
        output = negative ? -static_cast<std::int64_t>(magnitude)
                          : static_cast<std::int64_t>(magnitude);
    return SignedInputStatus::valid;
}

// @binding provider:XFLDecimal.from
JSValue
js_xfl_from(
    JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1 || (!JS_IsNumber(argv[0]) && !JS_IsBigInt(ctx, argv[0])))
        return JS_ThrowTypeError(
            ctx, "XFLDecimal.from() expects an integer number or bigint");
    std::int64_t mantissa = 0;
    SignedInputStatus const mantissaStatus =
        readSignedMantissa(ctx, argv[0], mantissa);
    if (mantissaStatus == SignedInputStatus::exception)
        return JS_EXCEPTION;
    if (mantissaStatus != SignedInputStatus::valid)
        return bindings::xfl_failure(ctx, "invalid");

    std::int32_t exponent = 0;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        if (!JS_IsNumber(argv[1]))
            return JS_ThrowTypeError(
                ctx, "XFLDecimal.from() exponent must be an integer number");
        double number = 0;
        if (JS_ToFloat64(ctx, &number, argv[1]) < 0)
            return JS_EXCEPTION;
        if (!std::isfinite(number) || std::trunc(number) != number ||
            number < std::numeric_limits<std::int32_t>::min() ||
            number > std::numeric_limits<std::int32_t>::max())
            return bindings::xfl_failure(ctx, "invalid");
        exponent = static_cast<std::int32_t>(number);
    }

    hook::XFLArithmeticResult const result =
        hook::setXahauFloatV1(mantissa, exponent);
    if (!result.ok())
        return bindings::xfl_failure(ctx, "invalid");
    return bindings::result_success(
        ctx, nativeNew<XFL>(ctx, js_xfl_class_id, result.value));
}

JSCFunctionListEntry const factoryFunctions[] = {
    JS_CFUNC_DEF("from", 2, js_xfl_from),
};

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

JSValue
xflArithmetic(
    JSContext* ctx,
    JSValueConst thisValue,
    int argc,
    JSValueConst* argv,
    char const* method,
    profile::xfl_arithmetic_operation operation,
    hook::XFLArithmeticResult (*kernel)(XFL const&, XFL const&) noexcept)
{
    auto const* left = xflValueNoThrow(thisValue);
    if (left == nullptr)
        return JS_ThrowTypeError(ctx, "XFLDecimal.%s: invalid receiver", method);
    auto const* right = argc > 0 ? xflValueNoThrow(argv[0]) : nullptr;
    if (right == nullptr)
        return JS_ThrowTypeError(
            ctx, "XFLDecimal.%s: expected XFLDecimal operand", method);

    ActiveXFLArithmeticProfile const active = activeXFLArithmeticProfile(ctx);
    if (!active.active)
        return JS_ThrowTypeError(
            ctx, "XFLDecimal.%s: arithmetic profile is inactive", method);
    if (!profile::xfl_arithmetic_profile_implements(active.code, operation))
        return JS_ThrowTypeError(
            ctx,
            "XFLDecimal.%s: arithmetic profile does not implement operation",
            method);

    hook::XFLArithmeticResult const result = kernel(*left, *right);
    constexpr std::array<char const*, 4> issueNames{
        nullptr,
        "overflow",
        "division-by-zero",
        "invalid",
    };
    static_assert(
        issueNames.size() ==
        static_cast<std::size_t>(hook::XFLArithmeticIssue::count));
    if (!result.ok()) {
        std::size_t const issue = static_cast<std::size_t>(result.issue);
        if (issue == 0 || issue >= issueNames.size())
            return JS_ThrowInternalError(
                ctx, "XFLDecimal.%s: unmapped arithmetic issue", method);
        return bindings::xfl_failure(ctx, issueNames[issue]);
    }
    return bindings::result_success(
        ctx, nativeNew<XFL>(ctx, js_xfl_class_id, result.value));
}

// @binding provider:XFLDecimal.add
JSValue
js_xfl_add(
    JSContext* ctx, JSValueConst thisValue, int argc, JSValueConst* argv)
{
    return xflArithmetic(
        ctx,
        thisValue,
        argc,
        argv,
        "add",
        profile::xfl_arithmetic_operation::xfl_decimal_add,
        hook::addXahauFloatV1);
}

// @binding provider:XFLDecimal.subtract
JSValue
js_xfl_subtract(
    JSContext* ctx, JSValueConst thisValue, int argc, JSValueConst* argv)
{
    return xflArithmetic(
        ctx,
        thisValue,
        argc,
        argv,
        "subtract",
        profile::xfl_arithmetic_operation::xfl_decimal_subtract,
        hook::subtractXahauFloatV1);
}

// @binding provider:XFLDecimal.multiply
JSValue
js_xfl_multiply(
    JSContext* ctx, JSValueConst thisValue, int argc, JSValueConst* argv)
{
    return xflArithmetic(
        ctx,
        thisValue,
        argc,
        argv,
        "multiply",
        profile::xfl_arithmetic_operation::xfl_decimal_multiply,
        hook::multiplyXahauFloatV1);
}

// @binding provider:XFLDecimal.divide
JSValue
js_xfl_divide(
    JSContext* ctx, JSValueConst thisValue, int argc, JSValueConst* argv)
{
    return xflArithmetic(
        ctx,
        thisValue,
        argc,
        argv,
        "divide",
        profile::xfl_arithmetic_operation::xfl_decimal_divide,
        hook::divideXahauFloatV1);
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
    JS_CFUNC_DEF("add", 1, js_xfl_add),
    JS_CFUNC_DEF("subtract", 1, js_xfl_subtract),
    JS_CFUNC_DEF("multiply", 1, js_xfl_multiply),
    JS_CFUNC_DEF("divide", 1, js_xfl_divide),
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
// @binding provider:XFLDecimal.zero
publishXFLFactory(JSContext* ctx, JSValueConst global)
{
    qjs::OwnedValue factory(ctx, JS_NewObject(ctx));
    qjs::OwnedValue zero(ctx, nativeNew<XFL>(ctx, js_xfl_class_id, XFL{}));
    if (factory.isException() || zero.isException() ||
        !qjs::freezeObject(ctx, zero.get()) ||
        !qjs::installFunctions(ctx, factory.get(), factoryFunctions) ||
        JS_DefinePropertyValueStr(
            ctx,
            factory.get(),
            "zero",
            zero.release(),
            JS_PROP_ENUMERABLE) < 0 ||
        !installRuntimeTypeClassifier(
            ctx, factory.get(), RuntimeTypeId::xflDecimal) ||
        !qjs::freezeObject(ctx, factory.get()))
        return false;
    return JS_SetPropertyStr(ctx, global, "XFLDecimal", factory.release()) >= 0;
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

JSValue
makeXFLDecimalRaw(JSContext* ctx, std::int64_t raw)
{
    if (!validRawDecimal(raw))
        return JS_ThrowInternalError(
            ctx, "XFLDecimal construction received invalid raw word");
    return nativeNew<XFL>(ctx, js_xfl_class_id, XFL(raw));
}

bool
isCanonicalXFLRaw(std::int64_t raw) noexcept
{
    return validRawDecimal(raw);
}

bool
readXFLDecimalRaw(JSValueConst input, std::int64_t* raw) noexcept
{
    if (raw != nullptr)
        *raw = 0;
    XFL const* decimal = xflValueNoThrow(input);
    if (raw == nullptr || decimal == nullptr ||
        !validRawDecimal(decimal->raw()))
        return false;
    *raw = decimal->raw();
    return true;
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
    if (negative == nullptr || magnitude == nullptr || exponent == nullptr)
        return false;
    std::int64_t raw = 0;
    if (!readXFLDecimalRaw(input, &raw))
        return false;
    XFL const decimal(raw);

    // Every instance is provider-minted from canonical decimal parts. Keep the
    // read seam defensive so a corrupted opaque cannot cross into Amount.
    if (decimal.raw() == 0)
        return true;
    std::uint64_t const decodedMagnitude = decimal.mantissa();
    std::int32_t const decodedExponent = decimal.exponent();
    bool const decodedNegative = decimal.is_negative();

    *negative = decodedNegative;
    *magnitude = decodedMagnitude;
    *exponent = decodedExponent;
    return true;
}

}  // namespace jshookz::provider::types
