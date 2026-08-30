#include "common.hpp"

#include "../../xahau-types/js.hpp"
#include "hook_imports.hpp"

namespace jshookz::provider::bindings {
namespace {

using types::UIntInputStatus;

constexpr std::uint64_t entropyTierShift = 32;
constexpr std::uint64_t entropyCountShift = 16;
constexpr std::uint64_t entropyTierMask = 0xFF;
constexpr std::uint64_t entropyCountMask = 0xFFFF;
constexpr std::uint64_t entropyDenominatorMask = 0xFFFF;
constexpr std::uint64_t entropyStatusMask =
    (entropyTierMask << entropyTierShift) |
    (entropyCountMask << entropyCountShift) | entropyDenominatorMask;

JSValue
// @binding provider:entropy.cr.dice
js_entropy_cr_dice(
    JSContext* ctx,
    JSValueConst,
    int argc,
    JSValueConst* argv)
{
    if (argc < 2)
        return JS_ThrowTypeError(
            ctx, "entropy.cr.dice expects sides and minimumTier");

    std::uint64_t sides = 0;
    std::uint64_t minimumTier = 0;
    UIntInputStatus status = types::readUIntInput(ctx, argv[0], 32, sides);
    if (status == UIntInputStatus::exception)
        return JS_EXCEPTION;
    if (status != UIntInputStatus::valid)
        return JS_ThrowTypeError(
            ctx, "entropy.cr.dice sides must be a lossless UInt32");

    status = types::readUIntInput(ctx, argv[1], 32, minimumTier);
    if (status == UIntInputStatus::exception)
        return JS_EXCEPTION;
    if (status != UIntInputStatus::valid)
        return JS_ThrowTypeError(
            ctx, "entropy.cr.dice minimumTier must be a lossless UInt32");

    std::int64_t const result = hook_entropy_cr_dice(
        static_cast<std::uint32_t>(sides),
        static_cast<std::uint32_t>(minimumTier));
    if (result < 0)
        return host_failure(ctx, result);
    if (sides == 0 || static_cast<std::uint64_t>(result) >= sides)
        return JS_ThrowInternalError(
            ctx, "entropy_cr_dice returned an out-of-range face");
    return host_success(ctx, JS_NewInt64(ctx, result));
}

JSValue
// @binding provider:entropy.cr.status
js_entropy_cr_status(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
    std::int64_t const result = hook_entropy_cr_status();
    if (result < 0)
        return host_failure(ctx, result);

    std::uint64_t const packed = static_cast<std::uint64_t>(result);
    std::uint32_t const tier =
        static_cast<std::uint32_t>((packed >> entropyTierShift) & entropyTierMask);
    std::uint32_t const count =
        static_cast<std::uint32_t>((packed >> entropyCountShift) & entropyCountMask);
    std::uint32_t const denominator =
        static_cast<std::uint32_t>(packed & entropyDenominatorMask);
    if ((packed & ~entropyStatusMask) != 0 || tier < 1 || tier > 4 ||
        count > denominator ||
        (tier == 1 && (count != 0 || denominator != 0)))
        return JS_ThrowInternalError(
            ctx, "entropy_cr_status returned malformed metadata");

    qjs::OwnedValue status(ctx, JS_NewObject(ctx));
    if (status.isException())
        return status.release();
    if (JS_DefinePropertyValueStr(
            ctx, status.get(), "tier", JS_NewUint32(ctx, tier),
            JS_PROP_ENUMERABLE) < 0 ||
        JS_DefinePropertyValueStr(
            ctx, status.get(), "count", JS_NewUint32(ctx, count),
            JS_PROP_ENUMERABLE) < 0 ||
        JS_DefinePropertyValueStr(
            ctx,
            status.get(),
            "denominator",
            JS_NewUint32(ctx, denominator),
            JS_PROP_ENUMERABLE) < 0 ||
        JS_PreventExtensions(ctx, status.get()) < 0)
        return JS_EXCEPTION;
    return host_success(ctx, status.release());
}

}  // namespace

bool
// @binding provider:entropy.cr
registerEntropy(JSContext* ctx, JSValue global)
{
    qjs::OwnedValue entropy(ctx, JS_NewObject(ctx));
    qjs::OwnedValue cr(ctx, JS_NewObject(ctx));
    if (entropy.isException() || cr.isException())
        return false;
    if (JS_SetPropertyStr(
            ctx,
            cr.get(),
            "dice",
            JS_NewCFunction(ctx, js_entropy_cr_dice, "dice", 2)) < 0 ||
        JS_SetPropertyStr(
            ctx,
            cr.get(),
            "status",
            JS_NewCFunction(ctx, js_entropy_cr_status, "status", 0)) < 0 ||
        JS_SetPropertyStr(ctx, entropy.get(), "cr", cr.release()) < 0)
        return false;
    return JS_SetPropertyStr(ctx, global, "entropy", entropy.release()) >= 0;
}

}  // namespace jshookz::provider::bindings
