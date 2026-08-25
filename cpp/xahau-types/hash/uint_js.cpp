#include "result.hpp"
#include "runtime_type.hpp"

#include "object/nominal_payload.hpp"
#include "quickjs.hpp"
#include "xfl/xfl.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

namespace {

using jshookz::provider::bindings::result_success;
using jshookz::provider::bindings::uint_failure;
using jshookz::provider::qjs::OwnedValue;

struct UIntValue
{
    std::uint64_t value;
    std::uint8_t bits;
};

JSClassID uintClassId;

void
uintFinalizer(JSRuntime* runtime, JSValue value)
{
    auto* integer = static_cast<UIntValue*>(
        JS_GetOpaque(value, uintClassId));
    if (integer != nullptr)
        js_free_rt(runtime, integer);
}

JSClassDef const uintClass = {
    .class_name = "UInt",
    .finalizer = uintFinalizer,
};

std::uint64_t
maximum(std::uint8_t bits) noexcept
{
    return bits == 64
        ? std::numeric_limits<std::uint64_t>::max()
        : (std::uint64_t{1} << bits) - 1;
}

constexpr std::uint64_t xflMinMantissa = 1000000000000000ULL;
constexpr std::uint64_t xflMaxMantissa = 9999999999999999ULL;
constexpr std::uint64_t xflResultCeiling = 10000000000000000ULL;
constexpr std::int32_t xflMinExponent = -96;
constexpr std::int32_t xflMaxExponent = 80;

std::uint64_t
decimalPower(std::uint32_t exponent) noexcept
{
    std::uint64_t value = 1;
    while (exponent-- > 0)
        value *= 10;
    return value;
}

bool
normalizeUnsignedXfl(
    std::uint64_t& mantissa,
    std::int32_t& exponent) noexcept
{
    if (mantissa == 0) {
        exponent = 0;
        return true;
    }
    // @source xahaud-vectors:src/xrpld/app/hook/HookAPI.h:237
    // Exact min/max off-by-one values correct inward without rescaling.
    while (mantissa < xflMinMantissa) {
        // normalize_xfl corrects this exact boundary without rescaling.
        if (mantissa == xflMinMantissa - 1) {
            ++mantissa;
            break;
        }
        mantissa *= 10;
        --exponent;
    }
    while (mantissa > xflMaxMantissa) {
        // normalize_xfl corrects this exact boundary without rescaling.
        if (mantissa == xflMaxMantissa + 1) {
            --mantissa;
            break;
        }
        mantissa /= 10;
        ++exponent;
    }
    if (exponent < xflMinExponent) {
        mantissa = 0;
        exponent = 0;
        return true;
    }
    return exponent <= xflMaxExponent;
}

bool
makeUnsignedXfl(std::uint64_t value, hook::XFL& output) noexcept
{
    std::int32_t exponent = 0;
    if (!normalizeUnsignedXfl(value, exponent))
        return false;
    output = value == 0
        ? hook::XFL{}
        : hook::XFL::from_components(
              false, exponent, static_cast<std::int64_t>(value));
    return true;
}

bool
multiplyUnsignedXfl(
    hook::XFL const& left,
    hook::XFL const& right,
    hook::XFL& output) noexcept
{
    if (left.is_zero() || right.is_zero()) {
        output = hook::XFL{};
        return true;
    }

    using Wide = unsigned __int128;
    Wide const scaled =
        (static_cast<Wide>(left.mantissa()) * right.mantissa()) /
        xflMinMantissa;
    if (scaled > std::numeric_limits<std::uint64_t>::max())
        return false;
    std::uint64_t mantissa = static_cast<std::uint64_t>(scaled);
    std::int32_t exponent = left.exponent() + right.exponent() + 15;
    if (!normalizeUnsignedXfl(mantissa, exponent))
        return false;
    output = mantissa == 0
        ? hook::XFL{}
        : hook::XFL::from_components(
              false, exponent, static_cast<std::int64_t>(mantissa));
    return true;
}

bool
divideUnsignedXfl(
    hook::XFL const& numerator,
    hook::XFL const& denominator,
    hook::XFL& output) noexcept
{
    if (denominator.is_zero())
        return false;
    if (numerator.is_zero()) {
        output = hook::XFL{};
        return true;
    }
    if (denominator.mantissa() == xflMinMantissa &&
        denominator.exponent() == -15) {
        output = numerator;
        return true;
    }

    std::uint64_t mantissa1 = numerator.mantissa();
    std::int32_t exponent1 = numerator.exponent();
    std::uint64_t mantissa2 = denominator.mantissa();
    std::int32_t exponent2 = denominator.exponent();
    if (!normalizeUnsignedXfl(mantissa1, exponent1) ||
        !normalizeUnsignedXfl(mantissa2, exponent2))
        return false;

    while (mantissa2 > mantissa1) {
        mantissa2 /= 10;
        ++exponent2;
    }
    if (mantissa2 == 0)
        return false;
    while (mantissa2 < mantissa1) {
        if (mantissa2 * 10 > mantissa1)
            break;
        mantissa2 *= 10;
        --exponent2;
    }

    std::uint64_t quotientMantissa = 0;
    std::int32_t quotientExponent = exponent1 - exponent2;
    while (mantissa2 > 0) {
        std::uint32_t digit = 0;
        while (mantissa1 >= mantissa2) {
            mantissa1 -= mantissa2;
            ++digit;
        }
        quotientMantissa = quotientMantissa * 10 + digit;
        mantissa2 /= 10;
        if (mantissa2 == 0)
            break;
        --quotientExponent;
    }

    if (!normalizeUnsignedXfl(quotientMantissa, quotientExponent))
        return false;
    output = quotientMantissa == 0
        ? hook::XFL{}
        : hook::XFL::from_components(
              false,
              quotientExponent,
              static_cast<std::int64_t>(quotientMantissa));
    return true;
}

bool
floorUnsignedXfl(hook::XFL const& value, std::uint64_t& output) noexcept
{
    output = 0;
    if (value.is_zero())
        return true;
    std::int32_t const shift = -value.exponent();
    if (shift > 15)
        return true;
    if (shift < 0)
        return false;
    output = value.mantissa() /
        decimalPower(static_cast<std::uint32_t>(shift));
    return output <= xflMaxMantissa;
}

JSValue
newUInt(JSContext* ctx, std::uint8_t bits, std::uint64_t value)
{
    auto* integer = static_cast<UIntValue*>(
        js_malloc(ctx, sizeof(UIntValue)));
    if (integer == nullptr)
        return JS_EXCEPTION;
    *integer = UIntValue{value, bits};

    JSValue object = JS_NewObjectClass(ctx, uintClassId);
    if (JS_IsException(object)) {
        js_free(ctx, integer);
        return object;
    }
    JS_SetOpaque(object, integer);
    if (JS_PreventExtensions(ctx, object) < 0) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    return object;
}

enum class InputStatus
{
    valid,
    outOfRange,
    invalidType,
    exception,
};

InputStatus
parseDecimalBigInt(
    JSContext* ctx,
    JSValueConst input,
    std::uint64_t limit,
    std::uint64_t& output)
{
    OwnedValue rendered(ctx, JS_ToString(ctx, input));
    if (rendered.isException())
        return InputStatus::exception;
    std::size_t length = 0;
    char const* text = JS_ToCStringLen(ctx, &length, rendered.get());
    if (text == nullptr)
        return InputStatus::exception;

    bool valid = length != 0;
    std::uint64_t value = 0;
    for (std::size_t index = 0; valid && index < length; ++index) {
        char const digit = text[index];
        valid = digit >= '0' && digit <= '9';
        if (!valid)
            break;
        std::uint64_t const part = static_cast<std::uint64_t>(digit - '0');
        if (value > (limit - part) / 10) {
            valid = false;
            break;
        }
        value = value * 10 + part;
    }
    JS_FreeCString(ctx, text);
    if (!valid)
        return InputStatus::outOfRange;
    output = value;
    return InputStatus::valid;
}

InputStatus
readUInt(
    JSContext* ctx,
    JSValueConst input,
    std::uint8_t bits,
    std::uint64_t& output)
{
    if (JS_IsObject(input) && JS_GetClassID(input) == uintClassId) {
        auto const* integer = static_cast<UIntValue const*>(
            JS_GetOpaque(input, uintClassId));
        if (integer == nullptr)
            return InputStatus::invalidType;
        if (integer->bits != bits)
            return InputStatus::outOfRange;
        output = integer->value;
        return InputStatus::valid;
    }

    if (JS_IsBigInt(ctx, input))
        return parseDecimalBigInt(ctx, input, maximum(bits), output);

    if (JS_IsNumber(input)) {
        double value = 0;
        if (JS_ToFloat64(ctx, &value, input) < 0)
            return InputStatus::exception;
        constexpr double maxSafeInteger = 9007199254740991.0;
        if (!std::isfinite(value) || std::trunc(value) != value || value < 0 ||
            value > maxSafeInteger || value > static_cast<double>(maximum(bits)))
            return InputStatus::outOfRange;
        output = static_cast<std::uint64_t>(value);
        return InputStatus::valid;
    }

    return InputStatus::invalidType;
}

InputStatus
readSameWidthUInt(
    JSValueConst input,
    std::uint8_t bits,
    std::uint64_t& output)
{
    if (!JS_IsObject(input) || JS_GetClassID(input) != uintClassId)
        return InputStatus::invalidType;
    auto const* integer = static_cast<UIntValue const*>(
        JS_GetOpaque(input, uintClassId));
    if (integer == nullptr)
        return InputStatus::invalidType;
    if (integer->bits != bits)
        return InputStatus::outOfRange;
    output = integer->value;
    return InputStatus::valid;
}

JSValue
inputFailure(JSContext* ctx, InputStatus status, std::uint8_t bits)
{
    if (status == InputStatus::exception)
        return JS_EXCEPTION;
    if (status == InputStatus::invalidType)
        return JS_ThrowTypeError(
            ctx, "UInt%u expects a same-width UInt, bigint, or safe integer",
            bits);
    return uint_failure(ctx, "out-of-range", bits);
}

UIntValue*
thisUInt(JSContext* ctx, JSValueConst value)
{
    return static_cast<UIntValue*>(
        JS_GetOpaque2(ctx, value, uintClassId));
}

JSValue
// @binding provider:UInt.bits
uintBits(JSContext* ctx, JSValueConst thisValue)
{
    auto const* integer = thisUInt(ctx, thisValue);
    return integer == nullptr
        ? JS_EXCEPTION
        : JS_NewUint32(ctx, integer->bits);
}

JSValue
// @binding provider:UInt.byteLength
uintByteLength(JSContext* ctx, JSValueConst thisValue)
{
    auto const* integer = thisUInt(ctx, thisValue);
    return integer == nullptr
        ? JS_EXCEPTION
        : JS_NewUint32(ctx, integer->bits / 8);
}

JSValue
// @binding provider:UInt.toBigInt
uintToBigInt(
    JSContext* ctx, JSValueConst thisValue, int, JSValueConst*)
{
    auto const* integer = thisUInt(ctx, thisValue);
    return integer == nullptr
        ? JS_EXCEPTION
        : JS_NewBigUint64(ctx, integer->value);
}

JSValue
// @binding provider:UInt.toNumber
uintToNumber(
    JSContext* ctx, JSValueConst thisValue, int, JSValueConst*)
{
    auto const* integer = thisUInt(ctx, thisValue);
    if (integer == nullptr)
        return JS_EXCEPTION;
    if (integer->bits != 64)
        return JS_NewUint32(ctx, static_cast<std::uint32_t>(integer->value));
    if (integer->value > 9007199254740991ULL)
        return uint_failure(ctx, "out-of-range", 64);
    return result_success(
        ctx, JS_NewInt64(ctx, static_cast<std::int64_t>(integer->value)));
}

JSValue
// @binding provider:UInt.toString
uintToString(
    JSContext* ctx, JSValueConst thisValue, int, JSValueConst*)
{
    auto const* integer = thisUInt(ctx, thisValue);
    if (integer == nullptr)
        return JS_EXCEPTION;
    OwnedValue bigint(ctx, JS_NewBigUint64(ctx, integer->value));
    return bigint.isException()
        ? bigint.release()
        : JS_ToString(ctx, bigint.get());
}

JSValue
// @binding provider:UInt.isZero
uintIsZero(
    JSContext* ctx, JSValueConst thisValue, int, JSValueConst*)
{
    auto const* integer = thisUInt(ctx, thisValue);
    return integer == nullptr
        ? JS_EXCEPTION
        : JS_NewBool(ctx, integer->value == 0);
}

JSValue
// @binding provider:UInt.equals
uintEquals(
    JSContext* ctx,
    JSValueConst thisValue,
    int argc,
    JSValueConst* argv)
{
    auto const* integer = thisUInt(ctx, thisValue);
    if (integer == nullptr)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_FALSE;
    std::uint64_t other = 0;
    InputStatus const status =
        readUInt(ctx, argv[0], integer->bits, other);
    if (status == InputStatus::exception)
        return JS_EXCEPTION;
    if (status != InputStatus::valid)
        return JS_FALSE;
    return JS_NewBool(ctx, integer->value == other);
}

JSValue
// @binding provider:UInt.compare
uintCompare(
    JSContext* ctx,
    JSValueConst thisValue,
    int argc,
    JSValueConst* argv)
{
    auto const* integer = thisUInt(ctx, thisValue);
    if (integer == nullptr)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "UInt.compare expects a value");
    std::uint64_t other = 0;
    InputStatus const status =
        readSameWidthUInt(argv[0], integer->bits, other);
    if (status != InputStatus::valid) {
        if (status == InputStatus::outOfRange)
            return JS_ThrowRangeError(
                ctx, "UInt.compare expects a same-width UInt");
        return JS_ThrowTypeError(
            ctx, "UInt.compare expects a same-width UInt");
    }
    return JS_NewInt32(
        ctx, integer->value < other ? -1 : integer->value > other ? 1 : 0);
}

enum class Arithmetic : int
{
    add,
    subtract,
    saturatingAdd,
    saturatingSubtract,
    wrappingAdd,
    wrappingSubtract,
    wrappingMultiply,
};

JSValue
// @binding provider:UInt.add
// @binding provider:UInt.subtract
// @binding provider:UInt.saturatingAdd
// @binding provider:UInt.saturatingSubtract
// @binding provider:UInt.wrappingAdd
// @binding provider:UInt.wrappingSubtract
// @binding provider:UInt.wrappingMultiply
uintArithmetic(
    JSContext* ctx,
    JSValueConst thisValue,
    int argc,
    JSValueConst* argv,
    int magic)
{
    auto const* integer = thisUInt(ctx, thisValue);
    if (integer == nullptr)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "UInt arithmetic expects a value");

    auto const operation = static_cast<Arithmetic>(magic);
    bool const saturating = operation == Arithmetic::saturatingAdd ||
        operation == Arithmetic::saturatingSubtract;
    bool const wrapping = operation == Arithmetic::wrappingAdd ||
        operation == Arithmetic::wrappingSubtract ||
        operation == Arithmetic::wrappingMultiply;
    std::uint64_t other = 0;
    InputStatus const status = saturating || wrapping
        ? readSameWidthUInt(argv[0], integer->bits, other)
        : readUInt(ctx, argv[0], integer->bits, other);
    if (status != InputStatus::valid) {
        if (!saturating && !wrapping)
            return inputFailure(ctx, status, integer->bits);
        if (status == InputStatus::invalidType)
            return JS_ThrowTypeError(
                ctx,
                "UInt%u %s arithmetic expects a same-width UInt",
                integer->bits,
                saturating ? "saturating" : "wrapping");
        return JS_ThrowRangeError(
            ctx,
            "UInt%u %s arithmetic expects a same-width UInt",
            integer->bits,
            saturating ? "saturating" : "wrapping");
    }

    std::uint64_t const limit = maximum(integer->bits);
    if (operation == Arithmetic::add) {
        if (integer->value > limit - other)
            return uint_failure(ctx, "overflow", integer->bits);
        return result_success(
            ctx, newUInt(ctx, integer->bits, integer->value + other));
    }
    if (operation == Arithmetic::subtract) {
        if (integer->value < other)
            return uint_failure(ctx, "underflow", integer->bits);
        return result_success(
            ctx, newUInt(ctx, integer->bits, integer->value - other));
    }
    if (operation == Arithmetic::saturatingAdd) {
        std::uint64_t const value = integer->value > limit - other
            ? limit
            : integer->value + other;
        return newUInt(ctx, integer->bits, value);
    }
    if (operation == Arithmetic::saturatingSubtract)
        return newUInt(
            ctx,
            integer->bits,
            integer->value < other ? 0 : integer->value - other);

    std::uint64_t value = 0;
    if (operation == Arithmetic::wrappingAdd)
        value = integer->value + other;
    else if (operation == Arithmetic::wrappingSubtract)
        value = integer->value - other;
    else
        value = integer->value * other;
    return newUInt(ctx, integer->bits, value & limit);
}

JSValue
// @binding provider:UInt.mulDiv
uintMulDiv(
    JSContext* ctx,
    JSValueConst,
    int argc,
    JSValueConst* argv,
    int magic)
{
    auto const bits = static_cast<std::uint8_t>(magic);
    if (argc < 3)
        return JS_ThrowTypeError(
            ctx,
            "UInt%u.mulDiv expects multiplicand, multiplier, and divisor",
            bits);

    std::uint64_t multiplicand = 0;
    InputStatus status = readUInt(ctx, argv[0], bits, multiplicand);
    if (status != InputStatus::valid)
        return inputFailure(ctx, status, bits);

    std::uint64_t multiplier = 0;
    status = readUInt(ctx, argv[1], bits, multiplier);
    if (status != InputStatus::valid)
        return inputFailure(ctx, status, bits);

    std::uint64_t divisor = 0;
    status = readUInt(ctx, argv[2], bits, divisor);
    if (status != InputStatus::valid)
        return inputFailure(ctx, status, bits);
    if (divisor == 0)
        return uint_failure(ctx, "division-by-zero", bits);

    using Wide = unsigned __int128;
    Wide const quotient =
        (static_cast<Wide>(multiplicand) * multiplier) / divisor;
    if (quotient > maximum(bits))
        return uint_failure(ctx, "overflow", bits);
    return result_success(
        ctx,
        newUInt(ctx, bits, static_cast<std::uint64_t>(quotient)));
}

JSValue
// @binding provider:UInt64.mulDivXfl
uint64MulDivXfl(
    JSContext* ctx,
    JSValueConst,
    int argc,
    JSValueConst* argv)
{
    constexpr std::uint8_t bits = 64;
    if (argc < 3)
        return JS_ThrowTypeError(
            ctx,
            "UInt64.mulDivXfl expects multiplicand, multiplier, and divisor");

    std::uint64_t multiplicand = 0;
    InputStatus status = readUInt(ctx, argv[0], bits, multiplicand);
    if (status != InputStatus::valid)
        return inputFailure(ctx, status, bits);

    std::uint64_t multiplier = 0;
    status = readUInt(ctx, argv[1], bits, multiplier);
    if (status != InputStatus::valid)
        return inputFailure(ctx, status, bits);

    std::uint64_t divisor = 0;
    status = readUInt(ctx, argv[2], bits, divisor);
    if (status != InputStatus::valid)
        return inputFailure(ctx, status, bits);
    if (divisor == 0)
        return uint_failure(ctx, "division-by-zero", bits);

    using Wide = unsigned __int128;
    // float_int(..., 0, 0) cannot represent this result domain even when
    // an earlier XFL normalization rounds the boundary inward.
    if (static_cast<Wide>(multiplicand) * multiplier >=
        static_cast<Wide>(xflResultCeiling) * divisor)
        return uint_failure(ctx, "overflow", bits);

    hook::XFL xflMultiplicand;
    hook::XFL xflMultiplier;
    hook::XFL xflDivisor;
    hook::XFL product;
    hook::XFL quotient;
    std::uint64_t result = 0;
    if (!makeUnsignedXfl(multiplicand, xflMultiplicand) ||
        !makeUnsignedXfl(multiplier, xflMultiplier) ||
        !makeUnsignedXfl(divisor, xflDivisor) ||
        !multiplyUnsignedXfl(xflMultiplicand, xflMultiplier, product) ||
        !divideUnsignedXfl(product, xflDivisor, quotient) ||
        !floorUnsignedXfl(quotient, result))
        return uint_failure(ctx, "overflow", bits);
    return result_success(ctx, newUInt(ctx, bits, result));
}

JSValue
// @binding provider:UInt.from
uintFrom(
    JSContext* ctx,
    JSValueConst,
    int argc,
    JSValueConst* argv,
    int magic)
{
    auto const bits = static_cast<std::uint8_t>(magic);
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "UInt%u.from expects a value", bits);
    std::uint64_t value = 0;
    InputStatus const status = readUInt(ctx, argv[0], bits, value);
    if (status != InputStatus::valid)
        return inputFailure(ctx, status, bits);
    return result_success(ctx, newUInt(ctx, bits, value));
}

JSCFunctionListEntry const uintPrototypeFunctions[] = {
    JS_CGETSET_DEF("bits", uintBits, nullptr),
    JS_CGETSET_DEF("byteLength", uintByteLength, nullptr),
    JS_CFUNC_DEF("toBigInt", 0, uintToBigInt),
    JS_CFUNC_DEF("toNumber", 0, uintToNumber),
    JS_CFUNC_DEF("toString", 0, uintToString),
    JS_CFUNC_DEF("isZero", 0, uintIsZero),
    JS_CFUNC_DEF("equals", 1, uintEquals),
    JS_CFUNC_DEF("compare", 1, uintCompare),
    JS_CFUNC_MAGIC_DEF(
        "add", 1, uintArithmetic,
        static_cast<int>(Arithmetic::add)),
    JS_CFUNC_MAGIC_DEF(
        "subtract", 1, uintArithmetic,
        static_cast<int>(Arithmetic::subtract)),
    JS_CFUNC_MAGIC_DEF(
        "saturatingAdd", 1, uintArithmetic,
        static_cast<int>(Arithmetic::saturatingAdd)),
    JS_CFUNC_MAGIC_DEF(
        "saturatingSubtract", 1, uintArithmetic,
        static_cast<int>(Arithmetic::saturatingSubtract)),
    JS_CFUNC_MAGIC_DEF(
        "wrappingAdd", 1, uintArithmetic,
        static_cast<int>(Arithmetic::wrappingAdd)),
    JS_CFUNC_MAGIC_DEF(
        "wrappingSubtract", 1, uintArithmetic,
        static_cast<int>(Arithmetic::wrappingSubtract)),
    JS_CFUNC_MAGIC_DEF(
        "wrappingMultiply", 1, uintArithmetic,
        static_cast<int>(Arithmetic::wrappingMultiply)),
};

JSValue
// @binding provider:UInt.zero
// @binding provider:UInt.max
newFactory(JSContext* ctx, std::uint8_t bits)
{
    OwnedValue factory(ctx, JS_NewObject(ctx));
    if (factory.isException())
        return factory.release();
    if (JS_DefinePropertyValueStr(
            ctx,
            factory.get(),
            "zero",
            newUInt(ctx, bits, 0),
            JS_PROP_ENUMERABLE) < 0 ||
        JS_DefinePropertyValueStr(
            ctx,
            factory.get(),
            "max",
            newUInt(ctx, bits, maximum(bits)),
            JS_PROP_ENUMERABLE) < 0 ||
        JS_DefinePropertyValueStr(
            ctx,
            factory.get(),
            "from",
            JS_NewCFunctionMagic(
                ctx,
                uintFrom,
                "from",
                1,
                JS_CFUNC_generic_magic,
                bits),
            JS_PROP_ENUMERABLE) < 0 ||
        JS_DefinePropertyValueStr(
            ctx,
            factory.get(),
            "mulDiv",
            JS_NewCFunctionMagic(
                ctx,
                uintMulDiv,
                "mulDiv",
                3,
                JS_CFUNC_generic_magic,
                bits),
            JS_PROP_ENUMERABLE) < 0 ||
        (bits == 64 &&
            JS_DefinePropertyValueStr(
                ctx,
                factory.get(),
                "mulDivXfl",
                JS_NewCFunction(
                    ctx, uint64MulDivXfl, "mulDivXfl", 3),
                JS_PROP_ENUMERABLE) < 0) ||
        !jshookz::provider::types::installRuntimeTypeClassifier(
            ctx,
            factory.get(),
            bits == 8        ? jshookz::provider::types::RuntimeTypeId::uInt8
                : bits == 16 ? jshookz::provider::types::RuntimeTypeId::uInt16
                : bits == 32
                ? jshookz::provider::types::RuntimeTypeId::uInt32
                : jshookz::provider::types::RuntimeTypeId::uInt64) ||
        !jshookz::provider::qjs::freezeObject(ctx, factory.get()))
        return JS_EXCEPTION;
    return factory.release();
}

}  // namespace

namespace jshookz::provider::types {

bool
isUInt(JSValueConst value, std::uint8_t expectedBits) noexcept
{
    if (!JS_IsObject(value) || JS_GetClassID(value) != uintClassId)
        return false;
    auto const* integer =
        static_cast<UIntValue const*>(JS_GetOpaque(value, uintClassId));
    return integer != nullptr &&
        (expectedBits == 0 || integer->bits == expectedBits);
}

bool
detail::readUIntNominalPayload(
    JSValueConst input,
    std::uint8_t expectedBits,
    std::uint64_t& value) noexcept
{
    value = 0;
    if (!JS_IsObject(input) || JS_GetClassID(input) != uintClassId)
        return false;
    auto const* integer = static_cast<UIntValue const*>(
        JS_GetOpaque(input, uintClassId));
    if (integer == nullptr || integer->bits != expectedBits ||
        integer->value > maximum(expectedBits))
        return false;
    value = integer->value;
    return true;
}

JSValue
makeUIntValue(JSContext* ctx, std::uint8_t bits, std::uint64_t value)
{
    if (bits != 8 && bits != 16 && bits != 32 && bits != 64)
        return JS_ThrowInternalError(ctx, "unsupported UInt width");
    return newUInt(ctx, bits, value);
}

}  // namespace jshookz::provider::types

extern "C" bool
register_uint_types(JSContext* ctx)
{
    if (!jshookz::qjs::defineClass(
            JS_GetRuntime(ctx), &uintClassId, &uintClass))
        return false;

    OwnedValue prototype(ctx, JS_NewObject(ctx));
    if (prototype.isException())
        return false;
    if (!jshookz::provider::qjs::installFunctions(
            ctx, prototype.get(), uintPrototypeFunctions))
        return false;
    if (!jshookz::provider::qjs::freezeObject(ctx, prototype.get()))
        return false;
    JS_SetClassProto(ctx, uintClassId, prototype.release());

    OwnedValue global(ctx, JS_GetGlobalObject(ctx));
    if (global.isException())
        return false;
    for (std::uint8_t bits : {8, 16, 32, 64}) {
        char name[7] = {};
        std::snprintf(name, sizeof(name), "UInt%u", bits);
        if (JS_SetPropertyStr(ctx, global.get(), name, newFactory(ctx, bits)) < 0)
            return false;
    }
    if (!jshookz::provider::types::publishRuntimeType(
            ctx,
            global.get(),
            "UInt",
            jshookz::provider::types::RuntimeTypeId::uInt))
        return false;
    return !JS_HasException(ctx);
}
