#include "amount/amount_js.hpp"
#include "js.hpp"
#include "leaf/leaf.hpp"
#include "object/field_js.hpp"
#include "object/nominal_payload.hpp"
#include "object/object.hpp"
#include "pathset/pathset_js.hpp"
#include "result.hpp"
#include "xfl/xfl_profile_context.hpp"

#include "catl/xdata/static_protocol.h"
#include "runtime_profile_limits.h"

#include <jshookz/qjs.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

extern "C" bool register_cpp_types(JSContext* ctx);
extern "C" bool register_uint_types(JSContext* ctx);

namespace {

std::vector<std::uint8_t>
hexBytes(char const* text)
{
    auto nibble = [](char value) -> std::uint8_t {
        if (value >= '0' && value <= '9')
            return static_cast<std::uint8_t>(value - '0');
        if (value >= 'A' && value <= 'F')
            return static_cast<std::uint8_t>(value - 'A' + 10);
        return static_cast<std::uint8_t>(value - 'a' + 10);
    };
    std::size_t const length = std::strlen(text);
    std::vector<std::uint8_t> bytes(length / 2);
    for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<std::uint8_t>(
            (nibble(text[i * 2]) << 4) | nibble(text[i * 2 + 1]));
    return bytes;
}

JSValue
makeIssueForAmount(JSContext* ctx, JSValueConst owner,
    jshookz::provider::types::AmountIssueKind kind,
    std::uint8_t const* identity, std::uint32_t length)
{
    namespace types = jshookz::provider::types;
    if (kind == types::AmountIssueKind::native) {
        std::uint8_t native[20] = {};
        return JS_IsUndefined(owner)
            ? types::makeIssueBytes(ctx, native, sizeof(native))
            : types::makeIssueDerivedBytes(
                  ctx, owner, native, sizeof(native));
    }
    if (kind == types::AmountIssueKind::iou)
        return JS_IsUndefined(owner)
            ? types::makeIssueBytes(ctx, identity, length)
            : types::makeIssueView(ctx, owner, identity, length);
    if (identity == nullptr || length != 24)
        return JS_ThrowInternalError(
            ctx, "invalid certified MPT issue identity");
    std::uint8_t issue[44] = {};
    std::memcpy(issue, identity + 4, 20);
    issue[39] = 1;
    issue[40] = identity[3];
    issue[41] = identity[2];
    issue[42] = identity[1];
    issue[43] = identity[0];
    return JS_IsUndefined(owner)
        ? types::makeIssueBytes(ctx, issue, sizeof(issue))
        : types::makeIssueDerivedBytes(ctx, owner, issue, sizeof(issue));
}

}  // namespace

class XahauTypes : public ::testing::Test {
protected:
    JSRuntime* rt = nullptr;
    JSContext* ctx = nullptr;

    void
    SetUp() override
    {
        rt = JS_NewRuntime();
        ASSERT_NE(rt, nullptr);
        ctx = JS_NewContext(rt);
        ASSERT_NE(ctx, nullptr);
        ASSERT_TRUE(jshookz::provider::bindings::registerResult(ctx));
        ASSERT_TRUE(register_cpp_types(ctx));
        ASSERT_TRUE(register_uint_types(ctx));
        jshookz::qjs::OwnedValue global(ctx, JS_GetGlobalObject(ctx));
        ASSERT_FALSE(global.isException());
        jshookz::qjs::OwnedValue util(
            ctx, JS_GetPropertyStr(ctx, global.get(), "util"));
        ASSERT_FALSE(util.isException());
        if (JS_IsUndefined(util.get())) {
            ASSERT_TRUE(jshookz::provider::types::registerRichLeafTypes(ctx));
            namespace types = jshookz::provider::types;
            types::AmountLeafMaterializers const amountLeaves{
                types::makeAccountIDView,
                types::makeCurrencyView,
                types::makeHash192View,
                types::makeXFLDecimalParts,
                makeIssueForAmount,
            };
            ASSERT_TRUE(types::registerAmount(ctx, amountLeaves));
            ASSERT_TRUE(types::registerObjectTypes(ctx));
            types::PathSetLeafMaterializers const pathLeaves{
                types::makeAccountIDView,
                types::makeCurrencyView,
                types::isCertifiedObjectRange,
            };
            ASSERT_TRUE(types::registerPathSet(ctx, pathLeaves));
            ASSERT_TRUE(types::registerFieldDescriptors(ctx, global.get()));
        } else
            ASSERT_TRUE(JS_IsObject(util.get()));
        ASSERT_FALSE(JS_HasException(ctx));
    }

    void
    TearDown() override
    {
        if (ctx)
            JS_FreeContext(ctx);
        if (rt) {
            jshookz::provider::types::unregisterObjectTypes(rt);
            jshookz::provider::types::unregisterRichLeafTypes(rt);
            JS_FreeRuntime(rt);
        }
    }

    jshookz::qjs::OwnedValue
    eval(char const* src)
    {
        return jshookz::qjs::OwnedValue(ctx,
            JS_Eval(ctx, src, std::strlen(src), "<test>", JS_EVAL_TYPE_GLOBAL));
    }

    std::string
    to_string(JSValueConst value)
    {
        char const* text = JS_ToCString(ctx, value);
        std::string out = text ? text : "";
        if (text)
            JS_FreeCString(ctx, text);
        return out;
    }

    void
    installRoot(std::vector<std::uint8_t> const& bytes)
    {
        jshookz::qjs::OwnedValue value(ctx,
            jshookz::provider::types::makeCertifiedObjectCopy(
                ctx, bytes.data(), static_cast<std::uint32_t>(bytes.size())));
        ASSERT_FALSE(value.isException());
        jshookz::qjs::OwnedValue global(ctx, JS_GetGlobalObject(ctx));
        ASSERT_FALSE(global.isException());
        ASSERT_GE(
            JS_SetPropertyStr(ctx, global.get(), "root", value.release()), 0);
    }

    void
    installValue(char const* name, JSValue value)
    {
        ASSERT_FALSE(JS_IsException(value));
        jshookz::qjs::OwnedValue owned(ctx, value);
        jshookz::qjs::OwnedValue global(ctx, JS_GetGlobalObject(ctx));
        ASSERT_FALSE(global.isException());
        ASSERT_GE(
            JS_SetPropertyStr(ctx, global.get(), name, owned.release()), 0);
    }
};

TEST_F(XahauTypes, Hash256Roundtrip)
{
    auto v = eval(
        "Hash256.fromHex("
        "'00000000000000000000000000000000000000000000000000000000000000ff'"
        ").toHex()");
    ASSERT_FALSE(v.isException());
    EXPECT_EQ(to_string(v.get()),
        "00000000000000000000000000000000000000000000000000000000000000FF");
}

TEST_F(XahauTypes, Hash256Heritage)
{
    namespace types = jshookz::provider::types;
    std::array<std::uint8_t, 16> hash128Bytes{};
    installValue("hash128Value",
        types::makeHash128Bytes(
            ctx, hash128Bytes.data(), hash128Bytes.size()));

    auto value = eval(R"JS(
        (() => {
          const lowBytes = new Uint8Array(32);
          lowBytes[0] = 1;
          lowBytes[31] = 255;
          const highBytes = new Uint8Array(32);
          highBytes[0] = 2;
          const low = Hash256.from(lowBytes);
          const equal = Hash256.from(lowBytes);
          const high = Hash256.from(highBytes);
          let wrongBrandIsTypeError = false;
          try { low.compare(hash128Value); }
          catch (error) { wrongBrandIsTypeError = error instanceof TypeError; }
          const lookalike = {byteLength: 32, compare() { return 0; }};
          return JSON.stringify({
            byteLength: low.byteLength,
            equal: low.compare(equal),
            less: low.compare(high),
            greater: high.compare(low),
            wrongBrandIsTypeError,
            hashNominal: low instanceof Hash,
            hash256Nominal: low instanceof Hash256,
            hash128IsHash: hash128Value instanceof Hash,
            hash128IsHash256: hash128Value instanceof Hash256,
            lookalikeIsHash: lookalike instanceof Hash,
            lookalikeIsHash256: lookalike instanceof Hash256,
          });
        })()
    )JS");
    ASSERT_FALSE(value.isException());
    EXPECT_EQ(to_string(value.get()),
        R"({"byteLength":32,"equal":0,"less":-1,"greater":1,"wrongBrandIsTypeError":true,"hashNominal":true,"hash256Nominal":true,"hash128IsHash":true,"hash128IsHash256":false,"lookalikeIsHash":false,"lookalikeIsHash256":false})");
}

TEST_F(XahauTypes, AccountIDZero)
{
    auto v = eval("AccountID.zero.isZero()");
    ASSERT_FALSE(v.isException());
    EXPECT_TRUE(JS_ToBool(ctx, v.get()));
}

TEST_F(XahauTypes, XFLDecimalTotalValueKernel)
{
    namespace types = jshookz::provider::types;
    auto installDecimal = [this](char const* name, bool negative,
                              std::uint64_t magnitude,
                              std::int32_t exponent) {
        installValue(
            name,
            types::makeXFLDecimalParts(
                ctx, negative, magnitude, exponent));
    };

    installDecimal("zero", false, 0, 0);
    installDecimal("positiveOne", false, 1000000000000000ULL, -15);
    installDecimal("negativeOne", true, 1000000000000000ULL, -15);
    installDecimal("positiveTwo", false, 2000000000000000ULL, -15);
    installDecimal("negativeTwo", true, 2000000000000000ULL, -15);
    installDecimal("positiveTen", false, 1000000000000000ULL, -14);
    installDecimal("negativeTen", true, 1000000000000000ULL, -14);

    auto value = eval(R"JS(
        (() => {
          let callbacks = 0;
          const proxy = new Proxy(positiveOne, {
            get() { ++callbacks; throw new Error("get trap"); },
            getPrototypeOf() {
              ++callbacks;
              throw new Error("getPrototypeOf trap");
            },
          });
          let compareError = "";
          try { positiveOne.compare({}); }
          catch (error) {
            compareError = `${error instanceof TypeError}:${error.message}`;
          }
          let proxyCompareError = "";
          try { positiveOne.compare(proxy); }
          catch (error) {
            proxyCompareError = `${error instanceof TypeError}:${error.message}`;
          }
          const doubleNegated = positiveTwo.negate().negate();
          return JSON.stringify({
            signs: [negativeOne.sign(), zero.sign(), positiveOne.sign()],
            zeroNegatesToZero: zero.negate().isZero(),
            negatedPositive: positiveOne.negate().compare(negativeOne),
            negatedNegative: negativeOne.negate().compare(positiveOne),
            doubleNegation: doubleNegated.equals(positiveTwo),
            sameExponent: [
              positiveOne.compare(positiveTwo),
              positiveTwo.compare(positiveOne),
            ],
            exponent: [
              positiveOne.compare(positiveTen),
              positiveTen.compare(positiveOne),
            ],
            negativeReversal: [
              negativeTen.compare(negativeTwo),
              negativeTwo.compare(negativeOne),
            ],
            equality: [
              positiveOne.equals(positiveOne),
              positiveOne.equals(positiveTwo),
              positiveOne.equals({}),
              positiveOne.equals(proxy),
              positiveOne.equals(),
            ],
            callbacks,
            compareError,
            proxyCompareError,
          });
        })()
    )JS");
    ASSERT_FALSE(value.isException());
    EXPECT_EQ(to_string(value.get()),
        R"({"signs":[-1,0,1],"zeroNegatesToZero":true,"negatedPositive":0,"negatedNegative":0,"doubleNegation":true,"sameExponent":[-1,1],"exponent":[-1,1],"negativeReversal":[-1,-1],"equality":[true,false,false,false,false],"callbacks":0,"compareError":"true:XFLDecimal.compare() expects XFLDecimal","proxyCompareError":"true:XFLDecimal.compare() expects XFLDecimal"})");
}

TEST_F(XahauTypes, XFLDecimalArithmeticNominalityAndProfileBackstop)
{
    namespace profile = catl::xdata::xahau_profile;
    namespace types = jshookz::provider::types;
    auto installDecimal = [this](char const* name, bool negative,
                              std::uint64_t magnitude,
                              std::int32_t exponent) {
        installValue(
            name,
            types::makeXFLDecimalParts(
                ctx, negative, magnitude, exponent));
    };
    installDecimal("one", false, 1000000000000000ULL, -15);
    installDecimal("zero", false, 0, 0);
    installDecimal("two", false, 2000000000000000ULL, -15);
    installDecimal("four", false, 4000000000000000ULL, -15);
    installDecimal("maximum", false, 9999999999999999ULL, 80);

    JSContext* foreignContext = JS_NewContext(rt);
    ASSERT_NE(foreignContext, nullptr);
    JSValue foreign = JS_Eval(
        foreignContext,
        "Object.create(null)",
        std::strlen("Object.create(null)"),
        "<foreign-realm>",
        JS_EVAL_TYPE_GLOBAL);
    ASSERT_FALSE(JS_IsException(foreign));
    installValue("foreign", foreign);

    types::XFLProfileContext state{
        true,
        profile::xfl_arithmetic_profile_xahau_float_v1,
    };
    JS_SetContextOpaque(ctx, &state);
    auto value = eval(R"JS(
        (() => {
          let traps = 0;
          const proxy = new Proxy({}, {
            get() { ++traps; throw new Error("get trap"); },
            getPrototypeOf() { ++traps; throw new Error("prototype trap"); },
          });
          const add = one.add(one);
          const subtract = two.subtract(one);
          const multiply = two.multiply(two);
          const divide = two.divide(one);
          const overflow = maximum.add(maximum);
          const divisionByZero = one.divide(zero);
          const invalid = maximum.divide(two);
          let operandError = "";
          try { one.add(proxy); }
          catch (error) {
            operandError = `${error instanceof TypeError}:${error.message}`;
          }
          let receiverError = "";
          try { Object.getPrototypeOf(one).add.call({}, one); }
          catch (error) {
            receiverError = `${error instanceof TypeError}:${error.message}`;
          }
          let foreignOperandError = "";
          try { one.multiply(foreign); }
          catch (error) {
            foreignOperandError = `${error instanceof TypeError}:${error.message}`;
          }
          let foreignReceiverError = "";
          try { Object.getPrototypeOf(one).divide.call(foreign, one); }
          catch (error) {
            foreignReceiverError = `${error instanceof TypeError}:${error.message}`;
          }
          return JSON.stringify({
            add: add.ok && add.value.equals(two),
            subtract: subtract.ok && subtract.value.equals(one),
            multiply: multiply.ok && multiply.value.equals(four),
            divide: divide.ok && divide.value.equals(two),
            successNominal: add instanceof Result && add.value instanceof XFLDecimal,
            successClosed: !Object.isExtensible(add),
            valueFrozen: Object.isFrozen(add.value),
            overflow: !overflow.ok && overflow.error.domain === "xfl" &&
              overflow.error.issue === "overflow",
            failureNominal: overflow instanceof Result,
            divisionByZero: !divisionByZero.ok &&
              divisionByZero.error.issue === "division-by-zero",
            invalid: !invalid.ok && invalid.error.issue === "invalid",
            errorNullPrototype: Object.getPrototypeOf(overflow.error) === null,
            errorClosed: !Object.isExtensible(overflow.error),
            operandError,
            receiverError,
            foreignOperandError,
            foreignReceiverError,
            traps,
          });
        })()
    )JS");
    ASSERT_FALSE(value.isException());
    EXPECT_EQ(to_string(value.get()),
        R"({"add":true,"subtract":true,"multiply":true,"divide":true,"successNominal":true,"successClosed":true,"valueFrozen":true,"overflow":true,"failureNominal":true,"divisionByZero":true,"invalid":true,"errorNullPrototype":true,"errorClosed":true,"operandError":"true:XFLDecimal.add: expected XFLDecimal operand","receiverError":"true:XFLDecimal.add: invalid receiver","foreignOperandError":"true:XFLDecimal.multiply: expected XFLDecimal operand","foreignReceiverError":"true:XFLDecimal.divide: invalid receiver","traps":0})");

    state.active = false;
    auto inactive = eval(R"JS(
        (() => { try { one.add(one); } catch (error) {
          return `${error instanceof TypeError}:${error.message}`; } })()
    )JS");
    ASSERT_FALSE(inactive.isException());
    EXPECT_EQ(to_string(inactive.get()),
        "true:XFLDecimal.add: arithmetic profile is inactive");

    state.active = true;
    state.code = profile::xfl_arithmetic_profile_nearest_even_v1;
    auto unimplemented = eval(R"JS(
        (() => { try { one.multiply(one); } catch (error) {
          return `${error instanceof TypeError}:${error.message}`; } })()
    )JS");
    ASSERT_FALSE(unimplemented.isException());
    EXPECT_EQ(to_string(unimplemented.get()),
        "true:XFLDecimal.multiply: arithmetic profile does not implement operation");

    state.code = profile::xfl_arithmetic_profile_none;
    auto absent = eval(R"JS(
        (() => { try { one.divide(one); } catch (error) {
          return `${error instanceof TypeError}:${error.message}`; } })()
    )JS");
    ASSERT_FALSE(absent.isException());
    EXPECT_EQ(to_string(absent.get()),
        "true:XFLDecimal.divide: arithmetic profile does not implement operation");
    JS_SetContextOpaque(ctx, nullptr);
    auto released = eval("delete globalThis.foreign");
    ASSERT_FALSE(released.isException());
    ASSERT_TRUE(JS_ToBool(ctx, released.get()));
    JS_FreeContext(foreignContext);
}

TEST_F(XahauTypes, STBlobFromBytes)
{
    auto v = eval("STBlob.from(new Uint8Array([1, 2, 3])).byteLength");
    ASSERT_FALSE(v.isException());
    int32_t n = 0;
    ASSERT_EQ(JS_ToInt32(ctx, &n, v.get()), 0);
    EXPECT_EQ(n, 3);
}

TEST_F(XahauTypes, UIntAddOverflowIsResult)
{
    auto v = eval("UInt8.from(200).okOr(null).add(200)");
    ASSERT_FALSE(v.isException());
    auto ok = jshookz::qjs::property(ctx, v.get(), "ok");
    EXPECT_FALSE(JS_ToBool(ctx, ok.get()));
    auto domain = jshookz::qjs::property(
        ctx, jshookz::qjs::property(ctx, v.get(), "error").get(), "domain");
    EXPECT_EQ(to_string(domain.get()), "uint");
}

TEST_F(XahauTypes, UIntWrappingArithmetic)
{
    auto value = eval(R"JS(
        (() => {
          const failures = [];
          const operations = [
            ["wrappingAdd", (Type, zero, max, one) => [
              [max, one, 0n],
              [zero, max, max.toBigInt()],
            ]],
            ["wrappingSubtract", (Type, zero, max, one) => [
              [zero, one, max.toBigInt()],
              [max, zero, max.toBigInt()],
            ]],
            ["wrappingMultiply", (Type, zero, max, one) => [
              [max, Type.from(2).okOr(null), max.toBigInt() - 1n],
              [max, zero, 0n],
              [max, one, max.toBigInt()],
            ]],
          ];

          for (const bits of [8, 16, 32, 64]) {
            const Type = globalThis[`UInt${bits}`];
            const zero = Type.zero;
            const max = Type.max;
            const one = Type.from(1).okOr(null);
            const sibling = bits === 8 ? UInt16.zero : UInt8.zero;

            for (const [name, vectors] of operations) {
              if (typeof max[name] !== "function") {
                failures.push(`UInt${bits}.${name}: missing`);
                continue;
              }
              for (const [left, right, expected] of vectors(Type, zero, max, one)) {
                const result = left[name](right);
                if (result.toBigInt() !== expected || result.bits !== bits ||
                    !(result instanceof UInt) || !(result instanceof Type) ||
                    result instanceof (bits === 8 ? UInt16 : UInt8))
                  failures.push(`UInt${bits}.${name}: value or brand`);
              }
              try {
                max[name](1n);
                failures.push(`UInt${bits}.${name}: primitive accepted`);
              } catch (error) {
                if (!(error instanceof TypeError))
                  failures.push(`UInt${bits}.${name}: primitive error`);
              }
              try {
                max[name](sibling);
                failures.push(`UInt${bits}.${name}: wrong width accepted`);
              } catch (error) {
                if (!(error instanceof RangeError))
                  failures.push(`UInt${bits}.${name}: wrong-width error`);
              }
            }
          }
          return failures.join("\n");
        })()
    )JS");
    ASSERT_FALSE(value.isException());
    EXPECT_EQ(to_string(value.get()), "");
}

TEST_F(XahauTypes, UInt64MulDivXfl)
{
    auto value = eval(R"JS(
        (() => {
          if (typeof UInt64.mulDivXfl !== "function")
            return "UInt64.mulDivXfl: missing";

          const maximum = UInt64.mulDivXfl(
            9_999_999_999_999_999n, 1n, 1n);
          const ceiling = UInt64.mulDivXfl(
            10_000_000_000_000_000n, 1n, 1n);
          const floor = UInt64.mulDivXfl(10n, 1n, 3n);
          // @source xahaud-vectors:src/xrpld/app/hook/HookAPI.h:237
          const maxCorrection = UInt64.mulDivXfl(5n, 2n, 1n);
          const divideEquality = UInt64.mulDivXfl(2n, 1n, 2n);
          const minCorrection = UInt64.mulDivXfl(
            9_999_999_999_999_999n, 2n, 2n);
          const nominal = UInt64.mulDivXfl(
            UInt64.from(21n).okOr(null), 2, 4n);
          const zeroDividend = UInt64.mulDivXfl(0n, UInt64.max, 7n);
          const tinyFloor = UInt64.mulDivXfl(1n, 1n, UInt64.max);
          const zeroDivisor = UInt64.mulDivXfl(0n, UInt64.max, 0n);
          const negative = UInt64.mulDivXfl(-1n, 1n, 1n);
          const wrongWidth = UInt64.mulDivXfl(UInt32.max, 1n, 1n);
          let stringRejected = false;
          try { UInt64.mulDivXfl("10", 1n, 1n); }
          catch (error) { stringRejected = error instanceof TypeError; }

          const successShape = result => result instanceof Result &&
            result.ok && result.value instanceof UInt &&
            result.value instanceof UInt64 && !(result.value instanceof UInt32);
          const failureShape = (result, issue) => result instanceof Result &&
            !result.ok && result.error.domain === "uint" &&
            result.error.issue === issue && result.error.bits === 64 &&
            Object.getPrototypeOf(result.error) === null;

          const failures = [];
          if (!successShape(maximum) ||
              maximum.value.toBigInt() !== 9_999_999_999_999_999n)
            failures.push("maximum");
          if (!failureShape(ceiling, "overflow"))
            failures.push("ceiling");
          if (!successShape(floor) || floor.value.toBigInt() !== 3n)
            failures.push("floor");
          if (!successShape(maxCorrection) ||
              maxCorrection.value.toBigInt() !== 9n)
            failures.push("max mantissa correction");
          if (!successShape(divideEquality) ||
              divideEquality.value.toBigInt() !== 1n)
            failures.push("divide equality");
          if (!failureShape(minCorrection, "overflow"))
            failures.push("min mantissa correction");
          if (!successShape(nominal) || nominal.value.toBigInt() !== 10n)
            failures.push("operand admission");
          if (!successShape(zeroDividend) || zeroDividend.value.toBigInt() !== 0n)
            failures.push("zero dividend");
          if (!successShape(tinyFloor) || tinyFloor.value.toBigInt() !== 0n)
            failures.push("tiny floor");
          if (!failureShape(zeroDivisor, "division-by-zero"))
            failures.push("zero divisor");
          if (!failureShape(negative, "out-of-range"))
            failures.push("negative operand");
          if (!failureShape(wrongWidth, "out-of-range"))
            failures.push("wrong width");
          if (!stringRejected)
            failures.push("invalid type");
          return failures.join("\n");
        })()
    )JS");
    ASSERT_FALSE(value.isException());
    EXPECT_EQ(to_string(value.get()), "");
}

TEST_F(XahauTypes, CertifiedObjectIsLazyImmutableAndCanonical)
{
    // Sequence is first on wire; canonical field-code order puts Flags first.
    installRoot({
        0x24,
        0x00,
        0x00,
        0x00,
        0x07,
        0x22,
        0x00,
        0x00,
        0x00,
        0x09,
    });
    auto value = eval(R"JS(
        (() => {
          const flags = root.Flags;
          const fieldBytes = root.fieldBytes("Flags").toBytes();
          const canonical = root.toBytes();
          let assignmentFailed = false;
          try {
            (() => {
              "use strict";
              root.Flags = UInt32.from(1).okOr(null);
            })();
          }
          catch (_) { assignmentFailed = true; }
          return JSON.stringify({
            keys: Object.keys(root),
            same: flags === root.Flags && flags === root.get("Flags"),
            flags: flags.toNumber(),
            sequence: root.Sequence.toNumber(),
            fieldBytes: Array.from(fieldBytes),
            canonical: Array.from(canonical),
            json: root.toJSON(),
            jsonFresh: root.toJSON() !== root.toJSON(),
            extensible: Object.isExtensible(root),
            assignmentFailed,
            absent: root.get("NotAField") === undefined,
          });
        })()
    )JS");
    ASSERT_FALSE(value.isException());
    EXPECT_EQ(to_string(value.get()),
        R"({"keys":["Flags","Sequence"],"same":true,"flags":9,"sequence":7,"fieldBytes":[0,0,0,9],"canonical":[34,0,0,0,9,36,0,0,0,7],"json":{"Flags":9,"Sequence":7},"jsonFresh":true,"extensible":false,"assignmentFailed":true,"absent":true})");
}

TEST_F(XahauTypes, GeneratedObjectClassesUseRealPrototypeHierarchy)
{
    installRoot(hexBytes(
        "12000024000000016140000000000F4240"
        "68400000000000000A73008114"
        "B5F762798A53D543A014CAF8B297CFF8F2F937E8"
        "831439249EE0886DE835D4F4D47DA9D9B1D2AED83C11"));
    auto payment = eval(R"JS(
        (() => {
          const nouns = [STObject, Transaction, Payment, LedgerEntry, AccountRoot];
          const hierarchy =
            Object.getPrototypeOf(Transaction.prototype) === STObject.prototype &&
            Object.getPrototypeOf(Payment.prototype) === Transaction.prototype &&
            Object.getPrototypeOf(LedgerEntry.prototype) === STObject.prototype &&
            Object.getPrototypeOf(AccountRoot.prototype) === LedgerEntry.prototype &&
            Object.getPrototypeOf(Transaction) === STObject &&
            Object.getPrototypeOf(Payment) === Transaction &&
            Object.getPrototypeOf(LedgerEntry) === STObject &&
            Object.getPrototypeOf(AccountRoot) === LedgerEntry;
          const shapes = nouns.every(noun =>
            typeof noun === "function" && Object.isFrozen(noun) &&
            Object.isFrozen(noun.prototype));
          const unavailable = nouns.every(noun => {
            let called = false;
            let constructed = false;
            try { noun(); } catch (error) { called = error instanceof TypeError; }
            try { new noun(); }
            catch (error) { constructed = error instanceof TypeError; }
            return called && constructed;
          });
          class Forged extends STObject {}
          let forged = false;
          try { new Forged(); } catch (error) { forged = error instanceof TypeError; }
          const checks = {
            hierarchy, shapes, unavailable, forged,
            payment: root instanceof Payment,
            transaction: root instanceof Transaction,
            object: root instanceof STObject,
            notLedger: !(root instanceof LedgerEntry),
            notAccountRoot: !(root instanceof AccountRoot),
            prototype: Object.getPrototypeOf(root) === Payment.prototype,
            transactionType: root.TransactionType === 0,
            accountIdentity: root.Account === root.Account,
            destinationIdentity: root.Destination === root.Destination,
            accountsDifferent: !root.Account.equals(root.Destination),
            amountIdentity: root.Amount === root.Amount,
            amount: root.Amount.asNative().drops === 1000000n,
            flagsIdentity: root.Flags !== undefined && root.Flags === root.Flags,
            flags: root.Flags?.toNumber() === 0,
            bytes: root.toBytes().length === 72,
          };
          return Object.entries(checks).filter(([, ok]) => !ok)
            .map(([name]) => name).join(",");
        })()
    )JS");
    if (payment.isException()) {
        jshookz::qjs::OwnedValue error(ctx, JS_GetException(ctx));
        FAIL() << to_string(error.get());
    }
    EXPECT_EQ(to_string(payment.get()), "");

    installRoot(hexBytes(
        "11006122000000002400000007250000007B2D00000000"
        "550000000000000000000000000000000000000000000000000000000000000000"
        "62400000000EE6B2808114"
        "B5F762798A53D543A014CAF8B297CFF8F2F937E8"));
    auto accountRoot = eval(R"JS(
        root instanceof AccountRoot && root instanceof LedgerEntry &&
        root instanceof STObject && !(root instanceof Transaction) &&
        !(root instanceof Payment) &&
        Object.getPrototypeOf(root) === AccountRoot.prototype &&
        root.LedgerEntryType === 97 && root.Flags.toNumber() === 0 &&
        root.Sequence.toNumber() === 7 && root.OwnerCount.toNumber() === 0 &&
        root.PreviousTxnLgrSeq.toNumber() === 123 &&
        root.Balance.asNative().drops === 250000000n &&
        root.Balance === root.Balance && root.Account === root.Account
    )JS");
    if (accountRoot.isException()) {
        jshookz::qjs::OwnedValue error(ctx, JS_GetException(ctx));
        FAIL() << to_string(error.get());
    }
    EXPECT_TRUE(JS_ToBool(ctx, accountRoot.get()));

    installRoot(hexBytes(
        "120003240000000220210000000868400000000000000C73008114"
        "B5F762798A53D543A014CAF8B297CFF8F2F937E8"));
    auto transaction = eval(
        "root instanceof Transaction && root instanceof STObject && "
        "!(root instanceof Payment) && !(root instanceof LedgerEntry) && "
        "Object.getPrototypeOf(root) === Transaction.prototype");
    ASSERT_FALSE(transaction.isException());
    EXPECT_TRUE(JS_ToBool(ctx, transaction.get()));

    installRoot(hexBytes(
        "1100642200000000310000000000000002320000000000000001"
        "581111111111111111111111111111111111111111111111111111111111111111"
        "8214B5F762798A53D543A014CAF8B297CFF8F2F937E8"
        "0113402222222222222222222222222222222222222222222222222222222222222222"
        "3333333333333333333333333333333333333333333333333333333333333333"));
    auto ledgerEntry = eval(
        "root instanceof LedgerEntry && root instanceof STObject && "
        "!(root instanceof AccountRoot) && !(root instanceof Transaction) && "
        "Object.getPrototypeOf(root) === LedgerEntry.prototype");
    ASSERT_FALSE(ledgerEntry.isException());
    EXPECT_TRUE(JS_ToBool(ctx, ledgerEntry.get()));
}

TEST_F(XahauTypes, ObjectClassificationFailsClosedToConcreteSTObject)
{
    auto expectGeneric = [this](char const* hex) {
        installRoot(hexBytes(hex));
        auto result = eval(
            "root instanceof STObject && !(root instanceof Transaction) && "
            "!(root instanceof Payment) && !(root instanceof LedgerEntry) && "
            "!(root instanceof AccountRoot) && "
            "Object.getPrototypeOf(root) === STObject.prototype");
        if (result.isException()) {
            jshookz::qjs::OwnedValue error(ctx, JS_GetException(ctx));
            ADD_FAILURE() << to_string(error.get());
            return;
        }
        EXPECT_TRUE(JS_ToBool(ctx, result.get()));
    };

    expectGeneric(""); // no discriminator / non-ledger serialized object
    expectGeneric("127FFF"); // unknown TransactionType
    expectGeneric("1200008114B5F762798A53D543A014CAF8B297CFF8F2F937E8");
    // AccountRoot shape with an IOU Balance violates the native-balance rule.
    expectGeneric(
        "1100612200000000240000000125000000012D00000000"
        "550000000000000000000000000000000000000000000000000000000000000000"
        "62D4838D7EA4C680000000000000000000000000005553440000000000"
        "B5F762798A53D543A014CAF8B297CFF8F2F937E8"
        "8114B5F762798A53D543A014CAF8B297CFF8F2F937E8");
    // Both family discriminators are intrinsically conflicting.
    expectGeneric(
        "1100611200006140000000000F42408114"
        "B5F762798A53D543A014CAF8B297CFF8F2F937E8"
        "8314B5F762798A53D543A014CAF8B297CFF8F2F937E8");
}

TEST_F(XahauTypes, TrustedTransactionMintMatchesGenericDecodeAndFailsClosed)
{
    namespace types = jshookz::provider::types;
    auto mintOwned = [this](std::vector<std::uint8_t> const& source) {
        auto* owned = static_cast<std::uint8_t*>(js_malloc(ctx, source.size()));
        if (owned == nullptr)
            return JS_ThrowOutOfMemory(ctx);
        std::memcpy(owned, source.data(), source.size());
        return types::makeCertifiedTransactionOwned(
            ctx, owned, static_cast<std::uint32_t>(source.size()));
    };
    auto expectInternal = [this, &mintOwned](
                              std::vector<std::uint8_t> const& source,
                              std::string_view message) {
        jshookz::qjs::OwnedValue failed(ctx, mintOwned(source));
        EXPECT_TRUE(failed.isException());
        jshookz::qjs::OwnedValue error(ctx, JS_GetException(ctx));
        ASSERT_FALSE(error.isException());
        jshookz::qjs::OwnedValue text(ctx, JS_ToString(ctx, error.get()));
        ASSERT_FALSE(text.isException());
        EXPECT_EQ(to_string(text.get()), message);
    };

    struct AcceptedFixture {
        char const* name;
        char const* hex;
        bool payment;
    };
    // Every accepted blob is independently encoded at
    // xahaud-hookz-test-vectors:b865c6f. The nested Payment and
    // EnableAmendment are pinned by oracle notes d877442.
    std::array const accepted = {
        AcceptedFixture{
            "payment",
            "12000024000000016140000000000F4240"
            "68400000000000000A73008114"
            "B5F762798A53D543A014CAF8B297CFF8F2F937E8"
            "831439249EE0886DE835D4F4D47DA9D9B1D2AED83C11",
            true},
        AcceptedFixture{
            "account-set",
            "120003240000000220210000000868400000000000000C73008114"
            "B5F762798A53D543A014CAF8B297CFF8F2F937E8",
            false},
        AcceptedFixture{
            "payment-with-memo",
            "12000024000000036140000000000F4240"
            "68400000000000000A73008114"
            "B5F762798A53D543A014CAF8B297CFF8F2F937E8"
            "831439249EE0886DE835D4F4D47DA9D9B1D2AED83C11"
            "F9EA7C0A746578742F706C61696E"
            "7D0B68656C6C6F2D7861686175"
            "7E0A746578742F706C61696EE1F1",
            true},
        AcceptedFixture{
            "enable-amendment-pseudo-transaction",
            "1200642400000000260000007B5013"
            "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
            "68400000000000000073008114"
            "0000000000000000000000000000000000000000",
            false},
    };

    for (auto const& fixture : accepted) {
        SCOPED_TRACE(fixture.name);
        auto const source = hexBytes(fixture.hex);
        installValue("genericTransaction", types::makeCertifiedObjectCopy(
                                               ctx, source.data(), source.size()));
        installValue("trustedTransaction", mintOwned(source));
        auto equivalent = eval(R"JS(
            (() => {
              const kind = value => value instanceof Payment ? "Payment" :
                value instanceof Transaction ? "Transaction" : "other";
              const snapshot = value => JSON.stringify({
                kind: kind(value),
                keys: Reflect.ownKeys(value),
                fields: Reflect.ownKeys(value).map(name => {
                  const bytes = value.fieldBytes(name);
                  return [name, bytes === undefined ? null : bytes.toHex()];
                }),
                bytes: Array.from(value.toBytes()),
                json: value.toJSON(),
              });
              return Object.getPrototypeOf(genericTransaction) ===
                  Object.getPrototypeOf(trustedTransaction) &&
                snapshot(genericTransaction) === snapshot(trustedTransaction);
            })()
        )JS");
        if (equivalent.isException()) {
            jshookz::qjs::OwnedValue error(ctx, JS_GetException(ctx));
            FAIL() << to_string(error.get());
        }
        EXPECT_TRUE(JS_ToBool(ctx, equivalent.get()));

        auto expectedClass = eval(fixture.payment
                ? "genericTransaction instanceof Payment && "
                  "trustedTransaction instanceof Payment"
                : "genericTransaction instanceof Transaction && "
                  "trustedTransaction instanceof Transaction && "
                  "!(genericTransaction instanceof Payment) && "
                  "!(trustedTransaction instanceof Payment)");
        ASSERT_FALSE(expectedClass.isException());
        EXPECT_TRUE(JS_ToBool(ctx, expectedClass.get()));
    }

    auto expectBothReject = [this, &mintOwned](char const* name,
                                char const* hex) {
        SCOPED_TRACE(name);
        auto const source = hexBytes(hex);
        jshookz::qjs::OwnedValue generic(ctx,
            jshookz::provider::types::makeCertifiedObjectCopy(
                ctx, source.data(), static_cast<std::uint32_t>(source.size())));
        EXPECT_TRUE(generic.isException());
        if (generic.isException()) {
            jshookz::qjs::OwnedValue error(ctx, JS_GetException(ctx));
            EXPECT_TRUE(JS_IsError(ctx, error.get()));
        }

        jshookz::qjs::OwnedValue trusted(ctx, mintOwned(source));
        EXPECT_TRUE(trusted.isException());
        if (trusted.isException()) {
            jshookz::qjs::OwnedValue error(ctx, JS_GetException(ctx));
            EXPECT_TRUE(JS_IsError(ctx, error.get()));
        }
    };

    // Independent red controls mirror exact cases in
    // cpp/x-data/tests/oracle_corpus.json and recursive-index tests. They span
    // malformed/unknown headers, duplicate fields, wrong terminators,
    // recursive structure, and a semantically invalid leaf.
    expectBothReject("truncated-field", "2200");
    expectBothReject("unknown-long-form-header", "1000");
    expectBothReject("duplicate-field", "22000000012200000002");
    expectBothReject("wrong-root-terminator", "F1");
    expectBothReject("wrong-array-terminator", "F9E1");
    expectBothReject("invalid-account-length", "8101FF");
    expectBothReject(
        "invalid-iou-zero-issuer",
        "61D4838D7EA4C680000000000000000000000000005553440000000000"
        "0000000000000000000000000000000000000000000008114"
        "B5F762798A53D543A014CAF8B297CFF8F2F937E8");

    expectInternal(
        hexBytes(
            "11006122000000002400000007250000007B2D00000000"
            "550000000000000000000000000000000000000000000000000000000000000000"
            "62400000000EE6B2808114"
            "B5F762798A53D543A014CAF8B297CFF8F2F937E8"),
        "InternalError: trusted originating object is not a complete Transaction");
    expectInternal(
        hexBytes(
            "1200006140000000000000018314"
            "39249EE0886DE835D4F4D47DA9D9B1D2AED83C11"),
                   "InternalError: trusted originating object is not a complete Transaction");
    expectInternal(hexBytes("2200"),
                   "InternalError: trusted object certification failed: truncated field");
}

TEST_F(XahauTypes, ObjectFieldSelectorFormsMatchTheDeclaredSurface)
{
    installRoot({0x22, 0x00, 0x00, 0x00, 0x09});
    auto value = eval(R"JS(
        (() => {
          const code = Field.Flags.code;
          const changed = root.withField(
            code, new Uint8Array([0, 0, 0, 10]));
          const removed = root.withoutField(code);
          return root.has(code) === false &&
            root.get(code) === undefined &&
            root.fieldBytes(code).toHex() === "00000009" &&
            changed.Flags.toNumber() === 10 &&
            removed.Flags === undefined;
        })()
    )JS");
    ASSERT_FALSE(value.isException());
    EXPECT_TRUE(JS_ToBool(ctx, value.get()));
}

TEST_F(
    XahauTypes, ObjectReplacementIsIndependentAndAbsentRemovalPreservesIdentity)
{
    installRoot({
        0x24,
        0x00,
        0x00,
        0x00,
        0x07,
        0x22,
        0x00,
        0x00,
        0x00,
        0x09,
    });
    auto value = eval(R"JS(
        (() => {
          const changed = root.withField(
            Field.Flags, new Uint8Array([0, 0, 0, 10]));
          const identical = root.withField(
            "Flags", new Uint8Array([0, 0, 0, 9]));
          const removed = root.withoutField(Field.Flags);
          const absent = root.withoutField("Account");
          return changed !== root && identical !== root && removed !== root &&
            absent === root &&
            root.Flags.toNumber() === 9 && changed.Flags.toNumber() === 10 &&
            identical.Flags.toNumber() === 9 &&
            removed.Flags === undefined && removed.Sequence.toNumber() === 7 &&
            Array.from(changed.toBytes()).join(",") ===
              "34,0,0,0,10,36,0,0,0,7";
        })()
    )JS");
    ASSERT_FALSE(value.isException());
    EXPECT_TRUE(JS_ToBool(ctx, value.get()));
}

TEST_F(XahauTypes, ObjectReplacementAcceptsNumberEnumAndCanonicalRawContainer)
{
    installRoot({});
    auto value = eval(R"JS(
        (() => {
          const number = root.withField("Number", "1.25");
          const transaction = root.withField("TransactionType", 0);
          const nested = root.withField("Memo", new Uint8Array([0xE1]));
          return JSON.stringify({
            number: number.Number,
            bytes: Array.from(number.fieldBytes("Number").toBytes()),
            transaction: transaction.TransactionType,
            nestedSize: nested.Memo.toBytes().length,
          });
        })()
    )JS");
    ASSERT_FALSE(value.isException());
    EXPECT_EQ(to_string(value.get()),
        R"({"number":"1.25","bytes":[0,4,112,222,77,248,32,0,255,255,255,241],"transaction":0,"nestedSize":1})");

    auto rejects = eval(R"JS(
        (() => {
          const invalid = [
            () => root.withField("Flags", new Uint8Array([0, 0, 1])),
            () => root.withField("Memo", new Uint8Array([])),
            () => root.withField("Number", "0001.25"),
            () => root.withField("TransactionType", 1.5),
            () => root.withField("Flags", undefined),
            () => root.withField("NotAField", new Uint8Array()),
          ];
          return invalid.every(fn => {
            try { fn(); return false; }
            catch (error) { return error instanceof TypeError; }
          });
        })()
    )JS");
    ASSERT_FALSE(rejects.isException());
    EXPECT_TRUE(JS_ToBool(ctx, rejects.get()));
}

TEST_F(XahauTypes, ObjectReplacementNumberStringsMatchPinnedOracleFormatting)
{
    installRoot({});
    auto value = eval(R"JS(
        JSON.stringify([
          "0", "-0", "1", "1.25", "0.5", "-0.00125",
          "1e10", "1e11", "1e-10", "1e-11",
          "10000000000000005", "10000000000000015",
        ].map(input => root.withField("Number", input).Number))
    )JS");
    ASSERT_FALSE(value.isException());
    EXPECT_EQ(to_string(value.get()),
        R"(["0","0","1","1.25","0.5","-0.00125","10000000000","1000000000000000e-4","0.0000000001","1000000000000000e-26","1000000000000000e1","1000000000000002e1"])");
}

TEST_F(XahauTypes, ObjectReplacementStreamsCertifiedObjectAndArrayValues)
{
    installRoot({
        0xF9,
        0xEA,
        0x24,
        0,
        0,
        0,
        4,
        0x22,
        0,
        0,
        0,
        2,
        0xE1,
        0xF1,
    });
    auto value = eval(R"JS(
        (() => {
          const element = root.Memos[0];
          const objectCopy = root.withField("Memo", element);
          const arrayCopy = root.withField("Memos", root.Memos);
          let mismatch = false;
          try { root.withField("Memo", root.Memos); }
          catch (error) { mismatch = error instanceof TypeError; }
          return objectCopy !== root && arrayCopy !== root &&
            objectCopy.Memo.Flags.toNumber() === 2 &&
            objectCopy.Memo.Sequence.toNumber() === 4 &&
            arrayCopy.Memos !== root.Memos &&
            arrayCopy.Memos[0].Flags.toNumber() === 2 && mismatch;
        })()
    )JS");
    ASSERT_FALSE(value.isException());
    EXPECT_TRUE(JS_ToBool(ctx, value.get()));
}

TEST_F(XahauTypes, ObjectReplacementAcceptsEveryExactNominalMaterializer)
{
    namespace types = jshookz::provider::types;
    auto expose = [&](char const* name, JSValue value) {
        EXPECT_FALSE(JS_IsException(value)) << name;
        if (JS_IsException(value))
            return;
        jshookz::qjs::OwnedValue global(ctx, JS_GetGlobalObject(ctx));
        ASSERT_FALSE(global.isException());
        ASSERT_EQ(JS_SetPropertyStr(ctx, global.get(), name, value), 1);
    };

    std::array<std::uint8_t, 32> bytes{};
    for (std::uint32_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<std::uint8_t>(i + 1);
    std::array<std::uint8_t, 20> account{};
    for (std::uint32_t i = 0; i < account.size(); ++i)
        account[i] = static_cast<std::uint8_t>(0x40 + i);
    std::array<std::uint8_t, 20> currency{};
    currency[12] = 'U';
    currency[13] = 'S';
    currency[14] = 'D';
    std::array<std::uint8_t, 8> amount{0x40, 0, 0, 0, 0, 0, 0, 42};
    std::array<std::uint8_t, 20> nativeIssue{};
    std::array<std::uint8_t, 82> bridge{};
    bridge[0] = 20;
    std::memcpy(bridge.data() + 1, account.data(), account.size());
    bridge[41] = 20;
    std::memcpy(bridge.data() + 42, account.data(), account.size());

    expose("vUInt8", types::makeUIntValue(ctx, 8, 0x7f));
    expose("vUInt16", types::makeUIntValue(ctx, 16, 0x1234));
    expose("vUInt32", types::makeUIntValue(ctx, 32, 0x12345678));
    expose("vUInt64", types::makeUIntValue(ctx, 64, 0x123456789abcdef0));
    expose("vHash128", types::makeHash128Bytes(ctx, bytes.data(), 16));
    expose("vHash160", types::makeHash160Bytes(ctx, bytes.data(), 20));
    expose("vHash192", types::makeHash192Bytes(ctx, bytes.data(), 24));
    expose("vHash256", types::makeHash256Bytes(ctx, bytes.data(), 32));
    expose("vBlob", types::makeSTBlobBytes(ctx, bytes.data(), 7));
    expose("vAccount",
        types::makeAccountIDBytes(ctx, account.data(), account.size()));
    expose(
        "vAmount", types::makeAmountBytes(ctx, amount.data(), amount.size()));
    expose("vCurrency",
        types::makeCurrencyBytes(ctx, currency.data(), currency.size()));
    expose("vIssue",
        types::makeIssueBytes(ctx, nativeIssue.data(), nativeIssue.size()));
    expose(
        "vVector", types::makeVector256Bytes(ctx, bytes.data(), bytes.size()));
    expose("vBridge",
        types::makeXChainBridgeBytes(ctx, bridge.data(), bridge.size()));

    installRoot(hexBytes("011201404142434445464748494A4B4C4D4E4F5051525300"));
    auto path = eval("root.Paths");
    ASSERT_FALSE(path.isException());
    expose("vPathSet", path.release());
    installRoot({});

    auto result = eval(R"JS(
        (() => {
          const inputs = [
            ["CloseResolution", vUInt8],
            ["LedgerEntryType", 97],
            ["NetworkID", vUInt32],
            ["IndexNext", vUInt64],
            ["EmailHash", vHash128],
            ["TakerPaysCurrency", vHash160],
            ["MPTokenIssuanceID", vHash192],
            ["LedgerHash", vHash256],
            ["PublicKey", vBlob],
            ["Account", vAccount],
            ["Amount", vAmount],
            ["BaseAsset", vCurrency],
            ["LockingChainIssue", vIssue],
            ["Paths", vPathSet],
            ["Indexes", vVector],
            ["XChainBridge", vBridge],
          ];
          let current = root;
          for (const [field, value] of inputs)
            current = current.withField(field, value);
          const exact = inputs.every(([field, input]) => {
            const observed = current.get(field);
            return typeof input === "number" ? observed === input :
              observed !== input && (typeof input.toBytes === "function"
                ? Array.from(current.fieldBytes(field).toBytes()).join(",") ===
                    Array.from(input.toBytes()).join(",")
                : observed.toString() === input.toString());
          });
          let wrongNominal = false;
          try { root.withField("BaseAsset", vAccount); }
          catch (error) { wrongNominal = error instanceof TypeError; }
          return exact && wrongNominal && Reflect.ownKeys(current).length === 16 &&
            Reflect.ownKeys(root).length === 0;
        })()
    )JS");
    if (result.isException()) {
        jshookz::qjs::OwnedValue error(ctx, JS_GetException(ctx));
        FAIL() << to_string(error.get());
    }
    EXPECT_TRUE(JS_ToBool(ctx, result.get()));
}

TEST_F(XahauTypes, NestedArraySharesElementIdentityAndRealIteratorSymbol)
{
    // Memos -> two Memo object elements, each with a Flags leaf.
    installRoot({
        0xF9,
        0xEA,
        0x22,
        0x00,
        0x00,
        0x00,
        0x01,
        0xE1,
        0xEA,
        0x22,
        0x00,
        0x00,
        0x00,
        0x02,
        0xE1,
        0xF1,
    });
    auto value = eval(R"JS(
        (() => {
          const values = root.Memos;
          const first = values[0];
          const iterated = Array.from(values);
          return JSON.stringify({
            rootKeys: Object.keys(root),
            arrayKeys: Reflect.ownKeys(values),
            lengthOwn: Object.hasOwn(values, "length"),
            length: values.length,
            firstSame: first === values.at(0) && first === iterated[0],
            numbers: iterated.map(v => v.Flags.toNumber()),
            json: root.toJSON(),
            arrayJson: values.toJSON(),
            extensible: Object.isExtensible(values),
            absent: values.at(99) === undefined,
          });
        })()
    )JS");
    ASSERT_FALSE(value.isException());
    EXPECT_EQ(to_string(value.get()),
        R"({"rootKeys":["Memos"],"arrayKeys":["0","1","length"],"lengthOwn":true,"length":2,"firstSame":true,"numbers":[1,2],"json":{"Memos":[{"Memo":{"Flags":1}},{"Memo":{"Flags":2}}]},"arrayJson":[{"Memo":{"Flags":1}},{"Memo":{"Flags":2}}],"extensible":false,"absent":true})");
}

TEST_F(XahauTypes, ExactMintRejectsMalformedInputAndRegistrarRetries)
{
    std::uint8_t const malformed[] = {0x22, 0x00};
    jshookz::qjs::OwnedValue value(ctx,
        jshookz::provider::types::makeCertifiedObjectCopy(
            ctx, malformed, sizeof(malformed)));
    EXPECT_TRUE(value.isException());
    jshookz::qjs::OwnedValue exception(ctx, JS_GetException(ctx));
    EXPECT_FALSE(exception.isException());
    EXPECT_TRUE(JS_IsError(ctx, exception.get()));

    EXPECT_TRUE(jshookz::provider::types::registerObjectTypes(ctx));
    EXPECT_FALSE(JS_HasException(ctx));
    installRoot({});
    auto empty =
        eval("Object.keys(root).length === 0 && root.toBytes().length === 0");
    ASSERT_FALSE(empty.isException());
    EXPECT_TRUE(JS_ToBool(ctx, empty.get()));
}

TEST_F(XahauTypes, ObjectByteUtilitiesUseExactInputAndNominalResultLaws)
{
    auto valid = eval("new Uint8Array([0x22, 0, 0, 0, 9])");
    ASSERT_FALSE(valid.isException());
    jshookz::qjs::OwnedValue predicate(
        ctx, jshookz::provider::types::validateObjectBytes(ctx, valid.get()));
    ASSERT_FALSE(predicate.isException());
    EXPECT_TRUE(JS_ToBool(ctx, predicate.get()));

    installValue("decodedResult",
        jshookz::provider::types::safeDecodeObjectBytes(ctx, valid.get()));
    auto success = eval(
        "decodedResult.ok && decodedResult.value.Flags.toNumber() === 9 && "
        "Object.isExtensible(decodedResult) === false");
    ASSERT_FALSE(success.isException());
    EXPECT_TRUE(JS_ToBool(ctx, success.get()));

    auto malformed = eval("new Uint8Array([0x22, 0])");
    ASSERT_FALSE(malformed.isException());
    jshookz::qjs::OwnedValue invalidPredicate(ctx,
        jshookz::provider::types::validateObjectBytes(ctx, malformed.get()));
    ASSERT_FALSE(invalidPredicate.isException());
    EXPECT_FALSE(JS_ToBool(ctx, invalidPredicate.get()));

    installValue("failedResult",
        jshookz::provider::types::safeDecodeObjectBytes(ctx, malformed.get()));
    auto failure =
        eval("JSON.stringify({ok: failedResult.ok, "
             "domain: failedResult.error.domain, "
             "issue: failedResult.error.issue, "
             "offset: failedResult.error.offset, "
             "fieldCode: failedResult.error.fieldCode, "
             "proto: Object.getPrototypeOf(failedResult.error) === null})");
    ASSERT_FALSE(failure.isException());
    EXPECT_EQ(to_string(failure.get()),
        R"({"ok":false,"domain":"parse","issue":"invalid-field","offset":1,"fieldCode":131074,"proto":true})");

    auto blob = eval("STBlob.from(new Uint8Array([0x22, 0, 0, 0, 7]))");
    ASSERT_FALSE(blob.isException());
    jshookz::qjs::OwnedValue fromBlob(
        ctx, jshookz::provider::types::decodeObjectBytes(ctx, blob.get()));
    ASSERT_FALSE(fromBlob.isException());
    EXPECT_TRUE(jshookz::provider::types::isSTObject(fromBlob.get()));
}

TEST_F(
    XahauTypes, ExplicitObjectUtilitiesPublishWithRuntimeTypeObjects)
{
    auto value = eval(R"JS(
        (() => {
          const bytes = new Uint8Array([0x22, 0, 0, 0, 9]);
          const decoded = util.safeDecodeObject(bytes);
          const asserted = util.decodeObject(bytes);
          const runtimeNouns = [
            "Hash128", "Hash160", "Hash192", "Currency", "Issue",
            "Amount", "NativeAmount", "IOUAmount", "MPTAmount",
            "XFLDecimal", "PathSet", "Path", "PathHop", "Vector256",
            "XChainBridge", "STObject", "STArray", "SerializedField",
            "Hash", "LedgerKeylet",
          ];
          const published = [
            "STBlob", "Hash256", "AccountID", "Field", "util",
            ...runtimeNouns,
          ];
          return Object.isFrozen(util) &&
            Reflect.ownKeys(util).join(",") ===
              "validateObject,safeDecodeObject,decodeObject,keylet" &&
            Object.isFrozen(util.keylet) &&
            Reflect.ownKeys(util.keylet).join(",") === "account" &&
            util.validateObject(bytes) && !util.validateObject(bytes.subarray(0, 2)) &&
            decoded.ok && decoded.value.Flags.toNumber() === 9 &&
            asserted.Flags.toNumber() === 9 &&
            runtimeNouns.every(name =>
              typeof globalThis[name]?.[Symbol.hasInstance] === "function") &&
            published.every(name => Object.hasOwn(globalThis, name));
        })()
    )JS");
    ASSERT_FALSE(value.isException());
    EXPECT_TRUE(JS_ToBool(ctx, value.get()));
}

TEST_F(XahauTypes, AmountDropsMintsOnlyBoundedNativeNominalAmounts)
{
    namespace types = jshookz::provider::types;
    auto behavior = eval(R"JS(
        (() => {
          const hex = value => Array.from(value.toBytes(), byte =>
            byte.toString(16).padStart(2, "0")).join("").toUpperCase();
          const catches = (name, action) => {
            try { action(); return false; }
            catch (error) { return error?.name === name; }
          };

          const zero = Amount.drops(0n);
          const one = Amount.drops(1n);
          const maximum = Amount.drops(100_000_000_000_000_000n);
          const detached = Amount.drops.call(Object.create(null), 42n);
          const forged = {
            kind: "native",
            drops: 1n,
            byteLength: 8,
            toBytes: () => one.toBytes(),
          };
          const dropsGetter = Object.getOwnPropertyDescriptor(
            Object.getPrototypeOf(one), "drops").get;
          const ownNames = Object.getOwnPropertyNames(Amount);
          const ownSymbols = Object.getOwnPropertySymbols(Amount);

          return typeof Amount === "object" && Amount !== null &&
            Object.isFrozen(Amount) && !Object.isExtensible(Amount) &&
            ownNames.length === 1 && ownNames[0] === "drops" &&
            ownSymbols.length === 1 && ownSymbols[0] === Symbol.hasInstance &&
            typeof Amount.drops === "function" && Amount.drops.length === 1 &&
            Amount.from === undefined && Amount.iou === undefined &&
            Amount.mpt === undefined &&
            zero.drops === 0n && hex(zero) === "4000000000000000" &&
            one.drops === 1n && hex(one) === "4000000000000001" &&
            maximum.drops === 100_000_000_000_000_000n &&
            hex(maximum) === "416345785D8A0000" &&
            detached.drops === 42n && detached instanceof Amount &&
            detached instanceof NativeAmount &&
            !(detached instanceof IOUAmount) && !(detached instanceof MPTAmount) &&
            !(forged instanceof Amount) && !(forged instanceof NativeAmount) &&
            catches("TypeError", () => dropsGetter.call(forged)) &&
            catches("TypeError", () => Amount.drops()) &&
            catches("TypeError", () => Amount.drops(1)) &&
            catches("TypeError", () => Amount.drops("1")) &&
            catches("TypeError", () => Amount.drops(Object(1n))) &&
            catches("RangeError", () => Amount.drops(-1n)) &&
            catches("RangeError", () =>
              Amount.drops(100_000_000_000_000_001n)) &&
            catches("RangeError", () => Amount.drops(1n << 1024n)) &&
            catches("TypeError", () => Reflect.construct(Amount, [])) &&
            catches("TypeError", () => Reflect.construct(Amount.drops, [1n])) &&
            !Reflect.set(Amount, "drops", () => forged) &&
            Amount.drops(2n).drops === 2n;
        })()
    )JS");
    ASSERT_FALSE(behavior.isException());
    EXPECT_TRUE(JS_ToBool(ctx, behavior.get()));

    auto nominal = eval("Amount.drops(1n)");
    ASSERT_FALSE(nominal.isException());
    std::uint8_t scratch[8]{};
    types::NominalPayloadView payload;
    ASSERT_TRUE(types::readNominalPayload(
        ctx, nominal.get(), catl::xdata::MaterializerKind::amount, scratch,
        payload));
    std::array<std::uint8_t, 8> const expected{
        0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    ASSERT_EQ(payload.size, expected.size());
    EXPECT_EQ(std::memcmp(payload.data, expected.data(), expected.size()), 0);
}

TEST_F(XahauTypes, STObjectJoinsSerializedByteFamily)
{
    auto const expected = hexBytes("2200000009");
    jshookz::qjs::OwnedValue object(
        ctx,
        jshookz::provider::types::makeCertifiedObjectCopy(
            ctx,
            expected.data(),
            static_cast<std::uint32_t>(expected.size())));
    ASSERT_FALSE(object.isException());

    auto bytes = jshookz::provider::qjs::ByteView::get(
        ctx,
        object.get(),
        jshookz::provider::qjs::BytePolicy::stateValueLike);
    ASSERT_TRUE(static_cast<bool>(bytes));
    ASSERT_EQ(bytes.size(), expected.size());
    EXPECT_EQ(std::memcmp(bytes.data(), expected.data(), expected.size()), 0);
}

TEST_F(XahauTypes, RuntimeTypeObjectsUseOneSealedNominalClassifier)
{
    namespace types = jshookz::provider::types;

    std::array<std::uint8_t, 16> hash128{};
    std::array<std::uint8_t, 20> hash160{};
    std::array<std::uint8_t, 24> hash192{};
    std::array<std::uint8_t, 32> hash256{};
    std::array<std::uint8_t, 20> currency{};
    std::array<std::uint8_t, 20> issue{};
    std::array<std::uint8_t, 32> vector{};
    std::array<std::uint8_t, 82> bridge{};
    bridge[0] = 20;
    bridge[41] = 20;

    installValue(
        "accountIDValue", types::makeAccountIDBytes(ctx, hash160.data(), 20));
    installValue(
        "hash128Value", types::makeHash128Bytes(ctx, hash128.data(), 16));
    installValue(
        "hash160Value", types::makeHash160Bytes(ctx, hash160.data(), 20));
    installValue(
        "hash192Value", types::makeHash192Bytes(ctx, hash192.data(), 24));
    installValue(
        "hash256Value", types::makeHash256Bytes(ctx, hash256.data(), 32));
    installValue(
        "currencyValue", types::makeCurrencyBytes(ctx, currency.data(), 20));
    installValue("issueValue", types::makeIssueBytes(ctx, issue.data(), 20));
    installValue(
        "vectorValue", types::makeVector256Bytes(ctx, vector.data(), 32));
    installValue(
        "bridgeValue", types::makeXChainBridgeBytes(ctx, bridge.data(), 82));
    installValue(
        "xflValue",
        types::makeXFLDecimalParts(ctx, false, 1000000000000000ULL, -96));

    auto const native = hexBytes("400000000000002A");
    auto const iou = hexBytes(
        "D4838D7EA4C680000000000000000000000000005553440000000000"
        "B5F762798A53D543A014CAF8B297CFF8F2F937E8");
    auto const mpt = hexBytes(
        "600000000000000001"
        "000102030405060708090A0B0C0D0E0F1011121314151617");
    installValue(
        "nativeAmountValue",
        types::makeAmountBytes(ctx, native.data(), native.size()));
    installValue(
        "iouAmountValue", types::makeAmountBytes(ctx, iou.data(), iou.size()));
    installValue(
        "mptAmountValue", types::makeAmountBytes(ctx, mpt.data(), mpt.size()));

    installValue(
        "resultValue",
        jshookz::provider::bindings::result_success(ctx, JS_NewInt32(ctx, 7)));
    installValue(
        "voidResultValue", jshookz::provider::bindings::effect_success(ctx));
    installValue(
        "stObjectValue", types::makeCertifiedObjectCopy(ctx, nullptr, 0));

    installRoot(hexBytes("011201B5F762798A53D543A014CAF8B297CFF8F2F937E800"));
    auto paths = eval(
        "globalThis.pathSetValue = root.Paths;"
        "globalThis.pathValue = pathSetValue.at(0);"
        "globalThis.pathHopValue = pathValue.at(0);");
    ASSERT_FALSE(paths.isException());

    installRoot(hexBytes("F9EA2200000001E1F1"));
    auto array = eval("globalThis.stArrayValue = root.Memos");
    ASSERT_FALSE(array.isException());

    auto value = eval(R"JS(
        (() => {
          const nouns = [
            "AccountID", "Amount", "Currency", "Hash", "Hash128",
            "Hash160", "Hash192", "Hash256", "IOUAmount", "Issue",
            "MPTAmount", "NativeAmount", "Path", "PathHop", "PathSet",
            "Result", "STArray", "STBlob", "SerializedField",
            "UInt", "UInt8", "UInt16", "UInt32", "UInt64", "Vector256",
            "VoidResult", "XChainBridge", "XFLDecimal",
          ];
          const cases = [
            ["AccountID", accountIDValue, ["AccountID"]],
            ["native amount", nativeAmountValue, ["Amount", "NativeAmount"]],
            ["IOU amount", iouAmountValue, ["Amount", "IOUAmount"]],
            ["MPT amount", mptAmountValue, ["Amount", "MPTAmount"]],
            ["Currency", currencyValue, ["Currency"]],
            ["Hash128", hash128Value, ["Hash", "Hash128"]],
            ["Hash160", hash160Value, ["Hash", "Hash160"]],
            ["Hash192", hash192Value, ["Hash", "Hash192"]],
            ["Hash256", hash256Value, ["Hash", "Hash256"]],
            ["Issue", issueValue, ["Issue"]],
            ["Path", pathValue, ["Path"]],
            ["PathHop", pathHopValue, ["PathHop"]],
            ["PathSet", pathSetValue, ["PathSet"]],
            ["Result", resultValue, ["Result"]],
            ["STArray", stArrayValue, ["STArray"]],
            ["STBlob", STBlob.from(new Uint8Array()), ["STBlob"]],
            ["SerializedField", Field.Flags, ["SerializedField"]],
            ["UInt8", UInt8.zero, ["UInt", "UInt8"]],
            ["UInt16", UInt16.zero, ["UInt", "UInt16"]],
            ["UInt32", UInt32.zero, ["UInt", "UInt32"]],
            ["UInt64", UInt64.zero, ["UInt", "UInt64"]],
            ["Vector256", vectorValue, ["Vector256"]],
            ["VoidResult", voidResultValue, ["VoidResult"]],
            ["XChainBridge", bridgeValue, ["XChainBridge"]],
            ["XFLDecimal", xflValue, ["XFLDecimal"]],
          ];
          const failures = [];
          const fail = (where) => failures.push(where);

          for (const name of nouns) {
            const noun = globalThis[name];
            const descriptor = Object.getOwnPropertyDescriptor(
              noun, Symbol.hasInstance);
            if (typeof noun !== "object" || noun === null ||
                Object.getPrototypeOf(noun) !== Object.prototype ||
                !Object.isFrozen(noun) || Object.isExtensible(noun) ||
                Object.hasOwn(noun, "prototype") || !descriptor ||
                typeof descriptor.value !== "function" ||
                descriptor.writable || descriptor.enumerable ||
                descriptor.configurable)
              fail(`shape:${name}`);
            try { Reflect.construct(noun, []); fail(`construct:${name}`); }
            catch (error) {
              if (!(error instanceof TypeError)) fail(`construct-error:${name}`);
            }
            if (Reflect.set(noun, Symbol.hasInstance, () => true) ||
                Reflect.deleteProperty(noun, Symbol.hasInstance) ||
                Reflect.defineProperty(noun, Symbol.hasInstance,
                  {value: () => true}))
              fail(`mutable:${name}`);
          }

          for (const [label, instance, positives] of cases) {
            const expected = new Set(positives);
            for (const name of nouns) {
              if ((instance instanceof globalThis[name]) !== expected.has(name))
                fail(`matrix:${label}:${name}`);
            }

            let callbacks = 0;
            const proxy = new Proxy(instance, {
              get() { ++callbacks; throw new Error("get trap"); },
              getPrototypeOf() {
                ++callbacks;
                throw new Error("getPrototypeOf trap");
              },
            });
            for (const name of nouns) {
              if (proxy instanceof globalThis[name])
                fail(`proxy:${label}:${name}`);
            }
            if (callbacks !== 0) fail(`proxy-callback:${label}`);

            let clone = {};
            try { clone = JSON.parse(JSON.stringify(instance)); }
            catch (_) {}
            for (const name of nouns) {
              if (clone instanceof globalThis[name])
                fail(`clone:${label}:${name}`);
            }
          }

          const forged = {
            ok: true,
            value: 7,
            bits: 64,
            byteLength: 32,
            kind: "native",
            code: Field.Flags.code,
            typeCode: Field.Flags.typeCode,
            fieldCode: Field.Flags.fieldCode,
            [Symbol.toStringTag]: "Amount",
          };
          const plainValues = [
            null, undefined, false, 0, 0n, "", "value", {}, forged,
            Object.assign({}, forged), Object.create(forged),
          ];
          for (const candidate of plainValues) {
            for (const name of nouns) {
              if (candidate instanceof globalThis[name])
                fail(`plain:${String(candidate)}:${name}`);
            }
          }

          return failures.join("\n");
        })()
    )JS");
    ASSERT_FALSE(value.isException());
    EXPECT_EQ(to_string(value.get()), "");
}

TEST_F(XahauTypes, RuntimeTypeRegistrationIsRepeatableAndKeepsOldValuesNominal)
{
    namespace types = jshookz::provider::types;
    std::array<std::uint8_t, 20> currencyBytes{};
    currencyBytes[12] = 'U';
    currencyBytes[13] = 'S';
    currencyBytes[14] = 'D';
    installValue(
        "oldCurrency",
        types::makeCurrencyBytes(
            ctx,
            currencyBytes.data(),
            static_cast<std::uint32_t>(currencyBytes.size())));
    installValue("oldUInt8", types::makeUIntValue(ctx, 8, 7));
    installValue("oldBlob", types::makeSTBlobBytes(ctx, nullptr, 0));
    installValue(
        "oldResult",
        jshookz::provider::bindings::result_success(ctx, JS_NewInt32(ctx, 9)));
    installValue(
        "oldVoidResult", jshookz::provider::bindings::effect_success(ctx));

    installRoot(hexBytes(
        "12000024000000016140000000000F4240"
        "68400000000000000A73008114"
        "B5F762798A53D543A014CAF8B297CFF8F2F937E8"
        "8314B5F762798A53D543A014CAF8B297CFF8F2F937E8"));

    auto before = eval(
        "globalThis.oldPayment = root;"
        "globalThis.__priorNouns = [Currency, UInt8, Payment]");
    ASSERT_FALSE(before.isException());
    ASSERT_TRUE(jshookz::provider::bindings::registerResult(ctx));
    ASSERT_TRUE(register_cpp_types(ctx));
    ASSERT_TRUE(register_uint_types(ctx));
    ASSERT_FALSE(JS_HasException(ctx));

    auto after = eval(R"JS(
        (() => {
          const prior = globalThis.__priorNouns;
          return oldCurrency instanceof Currency &&
            oldUInt8 instanceof UInt && oldUInt8 instanceof UInt8 &&
            !(oldUInt8 instanceof UInt16) &&
            oldBlob instanceof STBlob &&
            util.decodeObject(oldBlob) instanceof STObject &&
            oldResult instanceof Result && oldVoidResult instanceof VoidResult &&
            oldPayment instanceof Payment && oldPayment instanceof Transaction &&
            Payment === prior[2] && Currency !== prior[0] && UInt8 !== prior[1];
        })()
    )JS");
    ASSERT_FALSE(after.isException());
    EXPECT_TRUE(JS_ToBool(ctx, after.get()));
}

TEST_F(XahauTypes, QuickJSStructuredCloneCannotForgeProviderNominality)
{
    namespace types = jshookz::provider::types;
    auto source = eval(R"JS(({
      bits: UInt8.zero.bits,
      byteLength: Hash256.zero.byteLength,
      [Symbol.toStringTag]: "UInt8",
    }))JS");
    ASSERT_FALSE(source.isException());

    std::size_t encodedSize = 0;
    std::uint8_t* encoded = JS_WriteObject(ctx, &encodedSize, source.get(), 0);
    ASSERT_NE(encoded, nullptr);
    jshookz::qjs::OwnedValue clone(
        ctx, JS_ReadObject(ctx, encoded, encodedSize, 0));
    js_free(ctx, encoded);
    ASSERT_FALSE(clone.isException());

    jshookz::qjs::OwnedValue global(ctx, JS_GetGlobalObject(ctx));
    ASSERT_FALSE(global.isException());
    jshookz::qjs::OwnedValue uint8Type(
        ctx, JS_GetPropertyStr(ctx, global.get(), "UInt8"));
    ASSERT_FALSE(uint8Type.isException());
    EXPECT_EQ(JS_IsInstanceOf(ctx, clone.get(), uint8Type.get()), 0);

    jshookz::qjs::OwnedValue genuine(ctx, types::makeUIntValue(ctx, 8, 7));
    ASSERT_FALSE(genuine.isException());
    encodedSize = 0;
    encoded = JS_WriteObject(ctx, &encodedSize, genuine.get(), 0);
    EXPECT_EQ(encoded, nullptr);
    ASSERT_TRUE(JS_HasException(ctx));
    jshookz::qjs::OwnedValue exception(ctx, JS_GetException(ctx));
    EXPECT_TRUE(JS_IsError(ctx, exception.get()));
    EXPECT_FALSE(JS_HasException(ctx));
}

TEST_F(XahauTypes, ObjectByteUtilitiesPublishExactWrongTerminatorAndTypeErrors)
{
    auto wrongClose = eval("new Uint8Array([0xF1])");
    ASSERT_FALSE(wrongClose.isException());
    installValue("closeResult",
        jshookz::provider::types::safeDecodeObjectBytes(ctx, wrongClose.get()));
    auto close = eval("JSON.stringify(closeResult.error)");
    ASSERT_FALSE(close.isException());
    EXPECT_EQ(to_string(close.get()),
        R"({"domain":"parse","issue":"wrong-terminator","offset":0,"expected":"root-eof","actual":983041})");

    auto malformed = eval("new Uint8Array([0x22, 0])");
    ASSERT_FALSE(malformed.isException());
    jshookz::qjs::OwnedValue decoded(
        ctx, jshookz::provider::types::decodeObjectBytes(ctx, malformed.get()));
    ASSERT_TRUE(decoded.isException());
    jshookz::qjs::OwnedValue assertionError(ctx, JS_GetException(ctx));
    ASSERT_TRUE(JS_IsError(ctx, assertionError.get()));
    installValue("assertionError", assertionError.release());
    auto assertion =
        eval("JSON.stringify({type: assertionError instanceof TypeError, "
             "message: assertionError.message, offset: assertionError.offset, "
             "fieldCode: assertionError.fieldCode, "
             "stack: Object.hasOwn(assertionError, 'stack')})");
    ASSERT_FALSE(assertion.isException());
    EXPECT_EQ(to_string(assertion.get()),
        R"({"type":true,"message":"truncated field","offset":1,"fieldCode":131074,"stack":false})");

    auto wrongKind = eval("[0x22, 0, 0, 0, 1]");
    ASSERT_FALSE(wrongKind.isException());
    jshookz::qjs::OwnedValue wrong(ctx,
        jshookz::provider::types::validateObjectBytes(ctx, wrongKind.get()));
    ASSERT_TRUE(wrong.isException());
    jshookz::qjs::OwnedValue contractError(ctx, JS_GetException(ctx));
    installValue("contractError", contractError.release());
    auto contract = eval("contractError instanceof TypeError && "
                         "contractError.message === "
                         "'expected Uint8Array, ArrayBuffer, or STBlob' && "
                         "Reflect.ownKeys(contractError).length === 1");
    ASSERT_FALSE(contract.isException());
    EXPECT_TRUE(JS_ToBool(ctx, contract.get()));
}

TEST_F(XahauTypes, GeneratedFieldDescriptorsAreExactNominalAndComplete)
{
    installRoot({0x22, 0x00, 0x00, 0x00, 0x09});
    auto value = eval(R"JS(
        (() => {
          const descriptor = Field.Flags;
          const own = Object.getOwnPropertyDescriptor(Field, "Flags");
          const child = Object.create(Field);
          const keys = Reflect.ownKeys(Field);
          const descriptors = Object.getOwnPropertyDescriptors(Field);
          const spread = {...Field};
          const assigned = Object.assign({}, Field);
          const descriptorPrototype = Object.getPrototypeOf(descriptor);
          const codeGetter = Object.getOwnPropertyDescriptor(
            descriptorPrototype, "code").get;
          let strictSetRejected = false;
          let strictDeleteRejected = false;
          let getterReceiverRejected = false;
          try {
            Function("Field", "value", '"use strict"; Field.Flags = value')(
              Field, {});
          } catch (error) { strictSetRejected = error instanceof TypeError; }
          try {
            Function("Field", '"use strict"; delete Field.Flags')(Field);
          } catch (error) { strictDeleteRejected = error instanceof TypeError; }
          try { codeGetter.call({}); }
          catch (error) { getterReceiverRejected = error instanceof TypeError; }
          return JSON.stringify({
            count: Object.keys(Field).length,
            ownKeyCount: keys.length,
            code: descriptor.code,
            typeCode: descriptor.typeCode,
            fieldCode: descriptor.fieldCode,
            frozen: Object.isFrozen(Field),
            descriptorFrozen: Object.isFrozen(descriptor),
            identity: descriptor === Field.Flags,
            reflectionIdentity: keys.every(name =>
              descriptors[name].value === Field[name] &&
              spread[name] === Field[name] && assigned[name] === Field[name]),
            reflectionFlags: keys.every(name => {
              const reflected = descriptors[name];
              return reflected.enumerable && !reflected.configurable &&
                !reflected.writable && !Object.hasOwn(reflected, "get") &&
                !Object.hasOwn(reflected, "set");
            }),
            plainPrototype: Object.getPrototypeOf(Field) === Object.prototype,
            samePrototype: Reflect.setPrototypeOf(Field, Object.prototype),
            freezeIdentity: Object.freeze(Field) === Field,
            sealIdentity: Object.seal(Field) === Field,
            deleteRejected: Reflect.deleteProperty(Field, "Flags") === false,
            setRejected: Reflect.set(Field, "Flags", {}) === false,
            sloppySetRejected: Function(
              "Field", "value", "Field.Flags = value; return Field.Flags !== value"
            )(Field, {}),
            strictSetRejected,
            sloppyDeleteRejected:
              Function("Field", "return delete Field.Flags")(Field) === false,
            strictDeleteRejected,
            sameDefinition: Reflect.defineProperty(Field, "Flags", own),
            changedDefinition: Reflect.defineProperty(
              Field, "Flags", {...own, enumerable: false}) === false,
            unknownDefinition: Reflect.defineProperty(
              Field, "Unknown", {value: descriptor}) === false,
            accessorDefinition: Reflect.defineProperty(
              Field, "Flags", {get() { return descriptor; }}) === false,
            symbolDefinition: Reflect.defineProperty(
              Field, Symbol("unknown"), {value: descriptor}) === false,
            unknownDelete: Reflect.deleteProperty(Field, "Unknown"),
            symbolDelete: Reflect.deleteProperty(Field, Symbol("unknown")),
            unknownRead: Field.Unknown === undefined,
            prototypeRejected: Reflect.setPrototypeOf(Field, {}) === false,
            inheritedShadowRejected:
              Reflect.set(child, "Flags", descriptor) === false &&
              !Object.hasOwn(child, "Flags"),
            lookup: root.get(descriptor).toNumber(),
            structuralMiss: root.get({code: descriptor.code}) === undefined,
            copiedMiss: root.get(Object.assign({}, descriptor)) === undefined,
            proxyMiss: root.get(new Proxy(descriptor, {})) === undefined,
            prototypeMiss: root.get(Object.create(descriptorPrototype)) === undefined,
            getterReceiverRejected,
          });
        })()
    )JS");
    ASSERT_FALSE(value.isException());
    EXPECT_EQ(to_string(value.get()),
        R"({"count":325,"ownKeyCount":325,"code":131074,"typeCode":2,"fieldCode":2,"frozen":true,"descriptorFrozen":true,"identity":true,"reflectionIdentity":true,"reflectionFlags":true,"plainPrototype":true,"samePrototype":true,"freezeIdentity":true,"sealIdentity":true,"deleteRejected":true,"setRejected":true,"sloppySetRejected":true,"strictSetRejected":true,"sloppyDeleteRejected":true,"strictDeleteRejected":true,"sameDefinition":true,"changedDefinition":true,"unknownDefinition":true,"accessorDefinition":true,"symbolDefinition":true,"unknownDelete":true,"symbolDelete":true,"unknownRead":true,"prototypeRejected":true,"inheritedShadowRejected":true,"lookup":9,"structuralMiss":true,"copiedMiss":true,"proxyMiss":true,"prototypeMiss":true,"getterReceiverRejected":true})");
}

TEST_F(XahauTypes, GeneratedFieldRowsJoinStaticProtocolExactly)
{
    namespace xdata = catl::xdata;
    auto const& protocol = xdata::xahau_static_protocol();
    ASSERT_EQ(protocol.material_field_count, 325);

    auto keys = eval("Object.keys(Field)");
    ASSERT_FALSE(keys.isException());
    jshookz::qjs::OwnedValue global(ctx, JS_GetGlobalObject(ctx));
    ASSERT_FALSE(global.isException());
    jshookz::qjs::OwnedValue fields(
        ctx, JS_GetPropertyStr(ctx, global.get(), "Field"));
    ASSERT_FALSE(fields.isException());
    std::unordered_set<void const*> identities;

    for (std::uint32_t ordinal = 0; ordinal < protocol.material_field_count;
         ++ordinal) {
        SCOPED_TRACE(ordinal);
        auto const* material = protocol.material_field(ordinal);
        ASSERT_NE(material, nullptr);
        auto const* row = protocol.field_by_code(material->field_code);
        ASSERT_NE(row, nullptr);
        ASSERT_EQ(row->material_ordinal, ordinal);
        auto const name = protocol.field_name(row->name_ordinal);
        ASSERT_NE(name.data, nullptr);
        std::string expected{name.data, name.size};

        jshookz::qjs::OwnedValue key(
            ctx, JS_GetPropertyUint32(ctx, keys.get(), ordinal));
        ASSERT_FALSE(key.isException());
        EXPECT_EQ(to_string(key.get()), expected);

        JSAtom atom = JS_NewAtomLen(ctx, name.data, name.size);
        ASSERT_NE(atom, JS_ATOM_NULL);
        jshookz::qjs::OwnedValue observed(
            ctx, JS_GetProperty(ctx, fields.get(), atom));
        ASSERT_FALSE(observed.isException());
        ASSERT_TRUE(JS_IsObject(observed.get()));
        EXPECT_TRUE(identities.insert(JS_VALUE_GET_PTR(observed.get())).second);

        jshookz::qjs::OwnedValue repeated(
            ctx, JS_GetProperty(ctx, fields.get(), atom));
        ASSERT_FALSE(repeated.isException());
        EXPECT_EQ(
            JS_VALUE_GET_PTR(repeated.get()), JS_VALUE_GET_PTR(observed.get()));

        for (auto const* property : {"code", "typeCode", "fieldCode"}) {
            jshookz::qjs::OwnedValue number(
                ctx, JS_GetPropertyStr(ctx, observed.get(), property));
            ASSERT_FALSE(number.isException());
            std::uint32_t actual = 0;
            ASSERT_EQ(JS_ToUint32(ctx, &actual, number.get()), 0);
            std::uint32_t const expectedNumber =
                std::string_view(property) == "code" ? material->field_code
                : std::string_view(property) == "typeCode"
                ? material->field_code >> 16
                : material->field_code & 0xffffu;
            EXPECT_EQ(actual, expectedNumber);
        }

        JSPropertyDescriptor descriptor{
            .flags = 0,
            .value = JS_UNDEFINED,
            .getter = JS_UNDEFINED,
            .setter = JS_UNDEFINED,
        };
        ASSERT_EQ(JS_GetOwnProperty(ctx, &descriptor, fields.get(), atom), 1);
        EXPECT_EQ(descriptor.flags, JS_PROP_ENUMERABLE);
        EXPECT_EQ(JS_VALUE_GET_PTR(descriptor.value),
            JS_VALUE_GET_PTR(observed.get()));
        EXPECT_TRUE(JS_IsUndefined(descriptor.getter));
        EXPECT_TRUE(JS_IsUndefined(descriptor.setter));
        JS_FreeValue(ctx, descriptor.value);
        JS_FreeValue(ctx, descriptor.getter);
        JS_FreeValue(ctx, descriptor.setter);
        JS_FreeAtom(ctx, atom);
    }
    EXPECT_EQ(identities.size(), protocol.material_field_count);
}

TEST_F(XahauTypes, RichFixedIssueVectorAndBridgeLeavesAreNominalAndImmutable)
{
    std::uint8_t hash192[24] = {};
    hash192[0] = 0x12;
    hash192[23] = 0xEF;
    std::uint8_t currency[20] = {};
    currency[12] = 'U';
    currency[13] = 'S';
    currency[14] = 'D';
    std::uint8_t issue[40] = {};
    std::memcpy(issue, currency, sizeof(currency));
    issue[20] = 0xB5;
    issue[39] = 0xE8;
    std::uint8_t vector[64] = {};
    vector[0] = 1;
    vector[32] = 2;
    std::uint8_t bridge[82] = {};
    bridge[0] = 20;
    bridge[1] = 3;
    bridge[41] = 20;
    bridge[42] = 4;

    installValue("hash192",
        jshookz::provider::types::makeHash192Bytes(
            ctx, hash192, sizeof(hash192)));
    installValue("currency",
        jshookz::provider::types::makeCurrencyBytes(
            ctx, currency, sizeof(currency)));
    installValue("issue",
        jshookz::provider::types::makeIssueBytes(ctx, issue, sizeof(issue)));
    installValue("vector",
        jshookz::provider::types::makeVector256Bytes(
            ctx, vector, sizeof(vector)));
    installValue("bridge",
        jshookz::provider::types::makeXChainBridgeBytes(
            ctx, bridge, sizeof(bridge)));

    auto value = eval(R"JS(
        (() => {
          const first = vector.at(0);
          const iterated = [...vector];
          const bytes = vector.toBytes();
          bytes[0] = 9;
          return JSON.stringify({
            hash: hash192.toHex(),
            currency: currency.toString(),
            issueKind: issue.kind,
            issueCurrencySame: issue.currency === issue.currency,
            issueIssuerSame: issue.issuer === issue.issuer,
            vectorLength: vector.length,
            vectorKeys: Reflect.ownKeys(vector),
            vectorIdentity: first === vector[0] && first === iterated[0],
            vectorFresh: vector.toBytes()[0] === 1,
            bridgeDoorSame:
              bridge.LockingChainDoor === bridge.LockingChainDoor,
            bridgeIssueSame:
              bridge.LockingChainIssue === bridge.LockingChainIssue,
            immutable: !Object.isExtensible(hash192) &&
              !Object.isExtensible(currency) && !Object.isExtensible(issue) &&
              !Object.isExtensible(vector) && !Object.isExtensible(bridge),
          });
        })()
    )JS");
    ASSERT_FALSE(value.isException());
    EXPECT_EQ(to_string(value.get()),
        R"({"hash":"1200000000000000000000000000000000000000000000EF","currency":"USD","issueKind":"iou","issueCurrencySame":true,"issueIssuerSame":true,"vectorLength":2,"vectorKeys":["0","1","length"],"vectorIdentity":true,"vectorFresh":true,"bridgeDoorSame":true,"bridgeIssueSame":true,"immutable":true})");

    std::uint8_t copiedCurrency[20] = {};
    std::uint8_t copiedHash192[24] = {};
    auto currencyValue = eval("currency");
    auto hashValue = eval("hash192");
    ASSERT_FALSE(currencyValue.isException());
    ASSERT_FALSE(hashValue.isException());
    EXPECT_TRUE(jshookz::provider::types::readCurrencyBytes(
        ctx, currencyValue.get(), copiedCurrency));
    EXPECT_TRUE(jshookz::provider::types::readHash192Bytes(
        ctx, hashValue.get(), copiedHash192));
    EXPECT_EQ(std::memcmp(copiedCurrency, currency, sizeof(currency)), 0);
    EXPECT_EQ(std::memcmp(copiedHash192, hash192, sizeof(hash192)), 0);
}

TEST_F(XahauTypes, ObjectMaterializesAmountPathSetAndBridgeWithExactIdentity)
{
    installRoot(hexBytes("61400000000000002A"));
    auto nativeAmount = eval(R"JS(
        (() => {
          const value = root.Amount;
          const issue = value.issue;
          const currency = issue.currency;
          return value.kind === "native" && value.drops === 42n &&
            issue.kind === "native" && issue.toBytes().byteLength === 20 &&
            currency === issue.currency && currency.isNative &&
            currency.toString() === "XAH" &&
            currency.toBytes().every(byte => byte === 0);
        })()
    )JS");
    ASSERT_FALSE(nativeAmount.isException());
    EXPECT_TRUE(JS_ToBool(ctx, nativeAmount.get()));

    installRoot(
        hexBytes("61D4838D7EA4C680000000000000000000000000005553440000000000"
                 "B5F762798A53D543A014CAF8B297CFF8F2F937E8"
                 "8114B5F762798A53D543A014CAF8B297CFF8F2F937E8"));
    auto amount = eval(R"JS(
        (() => {
          const value = root.Amount;
          const decimal = value.value;
          const converted = value.toXFL();
          const decimalPrototype = Object.getPrototypeOf(decimal);
          return value === root.get(Field.Amount) && value === root.Amount &&
            value.kind === "iou" && !decimal.isNegative() && !decimal.isZero() &&
            converted !== decimal && !converted.isNegative() &&
            !converted.isZero() &&
            Object.getPrototypeOf(converted) === decimalPrototype &&
            Reflect.ownKeys(decimalPrototype).join(",") ===
              "isNegative,isZero,sign,add,subtract,multiply,divide,negate,equals,compare" &&
            Object.isFrozen(decimalPrototype) &&
            !("raw" in decimal) && !("mantissa" in decimal) &&
            !("exponent" in decimal) &&
            value.currency.toString() === "USD" &&
            value.issuer.toHex() === "B5F762798A53D543A014CAF8B297CFF8F2F937E8" &&
            value.issue.kind === "iou" && !Object.isExtensible(value);
        })()
    )JS");
    ASSERT_FALSE(amount.isException());
    EXPECT_TRUE(JS_ToBool(ctx, amount.get()));

    installRoot(hexBytes("8114B5F762798A53D543A014CAF8B297CFF8F2F937E8"
                         "011201B5F762798A53D543A014CAF8B297CFF8F2F937E800"));
    auto paths = eval(R"JS(
        (() => {
          const value = root.Paths;
          const path = value.at(0);
          const hop = path.at(0);
          return value === root.Paths && value.length === 1 &&
            path !== value.at(0) && hop !== path.at(0) &&
            hop.account.toHex() ===
              "B5F762798A53D543A014CAF8B297CFF8F2F937E8" &&
            [...value].length === 1 && [...path].length === 1;
        })()
    )JS");
    ASSERT_FALSE(paths.isException());
    EXPECT_TRUE(JS_ToBool(ctx, paths.get()));

    installRoot(hexBytes("011914B5F762798A53D543A014CAF8B297CFF8F2F937E8"
                         "0000000000000000000000000000000000000000"
                         "14B5F762798A53D543A014CAF8B297CFF8F2F937E8"
                         "0000000000000000000000000000000000000000"));
    auto bridge = eval(R"JS(
        (() => {
          const value = root.XChainBridge;
          return value === root.XChainBridge &&
            value.LockingChainDoor.toHex() ===
              "B5F762798A53D543A014CAF8B297CFF8F2F937E8" &&
            value.LockingChainIssue.kind === "native" &&
            value.IssuingChainIssue.kind === "native";
        })()
    )JS");
    ASSERT_FALSE(bridge.isException());
    EXPECT_TRUE(JS_ToBool(ctx, bridge.get()));
}

TEST_F(XahauTypes, ObjectJSONDispatchesEveryStructuredLeafCanonically)
{
    struct Vector {
        char const* wire;
        char const* json;
    };
    Vector const vectors[] = {
        {
            "8114B5F762798A53D543A014CAF8B297CFF8F2F937E8",
            R"({"Account":"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh"})",
        },
        {
            "61D4838D7EA4C680000000000000000000000000005553440000000000"
            "B5F762798A53D543A014CAF8B297CFF8F2F937E8",
            R"({"Amount":{"currency":"USD","issuer":"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh","value":"1"}})",
        },
        {
            "011A0000000000000000000000000000000000000000",
            R"({"BaseAsset":"XAH"})",
        },
        {
            "01180000000000000000000000000000000000000000",
            R"({"LockingChainIssue":{"currency":"XAH"}})",
        },
        {
            "011201B5F762798A53D543A014CAF8B297CFF8F2F937E800",
            R"({"Paths":[[{"account":"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh","type":1}]]})",
        },
        {
            "011320000102030405060708090A0B0C0D0E0F"
            "101112131415161718191A1B1C1D1E1F",
            R"({"Indexes":["000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F"]})",
        },
        {
            "011914B5F762798A53D543A014CAF8B297CFF8F2F937E8"
            "0000000000000000000000000000000000000000"
            "14B5F762798A53D543A014CAF8B297CFF8F2F937E8"
            "0000000000000000000000000000000000000000",
            R"({"XChainBridge":{"IssuingChainDoor":"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh","IssuingChainIssue":{"currency":"XAH"},"LockingChainDoor":"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh","LockingChainIssue":{"currency":"XAH"}}})",
        },
    };
    for (auto const& vector : vectors) {
        SCOPED_TRACE(vector.wire);
        installRoot(hexBytes(vector.wire));
        auto value = eval("JSON.stringify(root.toJSON())");
        ASSERT_FALSE(value.isException());
        EXPECT_EQ(to_string(value.get()), vector.json);
    }
}
