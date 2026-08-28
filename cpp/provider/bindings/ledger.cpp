#include "common.hpp"
#include "hook_imports.hpp"
#include "../provider_internal.hpp"
#include "object/object.hpp"

namespace jshookz::provider::bindings {
namespace {

JSValue originatingTransaction = JS_UNDEFINED;

[[nodiscard]] JSValue
slotFailure(JSContext *ctx, std::uint32_t slot, char const *stage,
            std::int64_t result)
{
    std::int64_t const cleared = hook_slot_clear(slot);
    if (cleared != 1)
        return JS_ThrowInternalError(
            ctx,
            "otxn.object: slot_clear returned %lld after %s returned %lld",
            (long long)cleared,
            stage,
            (long long)result);
    return JS_ThrowInternalError(
        ctx, "otxn.object: %s returned %lld",
        stage, (long long)result);
}

JSValue
// @binding provider:ledger.sequence
js_ledger_sequence(JSContext *ctx, JSValueConst this_val)
{
    return JS_NewInt64(ctx, hook_ledger_seq());
}

JSValue
// @binding provider:ledger.lastTime
js_ledger_last_time(JSContext *ctx, JSValueConst this_val)
{
    return JS_NewInt64(ctx, hook_ledger_last_time());
}

JSValue
// @binding provider:ledger.lastHash
js_ledger_last_hash(JSContext *ctx, JSValueConst this_val)
{
    uint8_t bytes[32];
    int64_t result = hook_ledger_last_hash(
        (uint32_t)(uintptr_t)bytes, sizeof(bytes));
    if (result != (int64_t)sizeof(bytes))
        return JS_ThrowInternalError(
            ctx, "ledger.lastHash: host returned %lld, expected 32",
            (long long)result);
    return makeHash256(ctx, bytes, sizeof(bytes));
}

JSValue
// @binding provider:otxn.type
js_otxn_type(JSContext *ctx, JSValueConst this_val,
             int argc, JSValueConst *argv)
{
    int64_t result = hook_otxn_type();
    return result < 0
        ? host_failure(ctx, result)
        : host_success(ctx, JS_NewInt64(ctx, result));
}

JSValue
// @binding provider:otxn.object
js_otxn_object(JSContext *ctx, JSValueConst this_val,
               int argc, JSValueConst *argv)
{
    if (!JS_IsUndefined(originatingTransaction))
        return JS_DupValue(ctx, originatingTransaction);

    std::int64_t const measurementSlotResult = hook_otxn_slot(0);
    if (measurementSlotResult <= 0 || measurementSlotResult > 255)
        return JS_ThrowInternalError(
            ctx, "otxn.object: otxn_slot returned %lld",
            (long long)measurementSlotResult);
    std::uint32_t const measurementSlot =
        static_cast<std::uint32_t>(measurementSlotResult);

    std::int64_t const measured = hook_slot_size(measurementSlot);
    if (measured <= 0 ||
        measured > static_cast<std::int64_t>(
            types::certifiedObjectMaxBytes()))
        return slotFailure(ctx, measurementSlot, "slot_size", measured);
    std::uint32_t const size = static_cast<std::uint32_t>(measured);
    std::int64_t const measurementCleared =
        hook_slot_clear(measurementSlot);
    if (measurementCleared != 1)
        return JS_ThrowInternalError(
            ctx, "otxn.object: measurement slot_clear returned %lld",
            (long long)measurementCleared);

    auto *bytes = static_cast<std::uint8_t *>(js_malloc(ctx, size));
    if (bytes == nullptr)
        return JS_HasException(ctx) ? JS_EXCEPTION : JS_ThrowOutOfMemory(ctx);

    std::int64_t const copySlotResult = hook_otxn_slot(0);
    if (copySlotResult <= 0 || copySlotResult > 255) {
        js_free(ctx, bytes);
        return JS_ThrowInternalError(
            ctx, "otxn.object: second otxn_slot returned %lld",
            (long long)copySlotResult);
    }
    std::uint32_t const copySlot =
        static_cast<std::uint32_t>(copySlotResult);

    std::int64_t const copied = hook_slot(
        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(bytes)),
        size,
        copySlot);
    if (copied != static_cast<std::int64_t>(size)) {
        js_free(ctx, bytes);
        return slotFailure(ctx, copySlot, "slot", copied);
    }
    std::int64_t const cleared = hook_slot_clear(copySlot);
    if (cleared != 1) {
        js_free(ctx, bytes);
        return JS_ThrowInternalError(
            ctx, "otxn.object: slot_clear returned %lld",
            (long long)cleared);
    }

    JSValue object = types::makeCertifiedObjectOwned(ctx, bytes, size);
    if (JS_IsException(object))
        return object;
    originatingTransaction = JS_DupValue(ctx, object);
    return object;
}

JSValue
// @binding provider:hook.account
js_hook_account(JSContext *ctx, JSValueConst this_val,
                int argc, JSValueConst *argv)
{
    uint8_t bytes[20];
    int64_t result = hook_hook_account(
        (uint32_t)(uintptr_t)bytes, sizeof(bytes));
    if (result < 0)
        return JS_ThrowInternalError(
            ctx, "hook.account: host returned %lld", (long long)result);
    if (result != (int64_t)sizeof(bytes))
        return JS_ThrowInternalError(
            ctx, "hook.account: host returned %lld, expected 20",
            (long long)result);
    return makeAccountID(ctx, bytes, sizeof(bytes));
}

JSCFunctionListEntry const js_ledger_properties[] = {
    JS_CGETSET_DEF("sequence", js_ledger_sequence, NULL),
    JS_CGETSET_DEF("lastTime", js_ledger_last_time, NULL),
    JS_CGETSET_DEF("lastHash", js_ledger_last_hash, NULL),
};

}  // namespace

void
resetOriginatingTransactionCache(JSContext *ctx) noexcept
{
    JS_FreeValue(ctx, originatingTransaction);
    originatingTransaction = JS_UNDEFINED;
}

bool
registerHook(JSContext *ctx, JSValue global)
{
    qjs::OwnedValue hook(ctx, JS_NewObject(ctx));
    if (hook.isException())
        return false;
    if (JS_SetPropertyStr(ctx, hook.get(), "account",
            JS_NewCFunction(ctx, js_hook_account, "account", 0)) < 0)
        return false;
    return JS_SetPropertyStr(ctx, global, "hook", hook.release()) >= 0;
}

bool
registerLedger(JSContext *ctx, JSValue global)
{
    qjs::OwnedValue ledger(ctx, JS_NewObject(ctx));
    if (ledger.isException())
        return false;
    if (!qjs::installFunctions(ctx, ledger.get(), js_ledger_properties) ||
        JS_SetPropertyStr(ctx, global, "ledger", ledger.release()) < 0)
        return false;

    qjs::OwnedValue otxn(ctx, JS_NewObject(ctx));
    if (otxn.isException())
        return false;
    if (JS_SetPropertyStr(ctx, otxn.get(), "type",
            JS_NewCFunction(ctx, js_otxn_type, "type", 0)) < 0)
        return false;
    if (JS_SetPropertyStr(ctx, otxn.get(), "object",
            JS_NewCFunction(ctx, js_otxn_object, "object", 0)) < 0)
        return false;
    return JS_SetPropertyStr(ctx, global, "otxn", otxn.release()) >= 0;
}

}  // namespace jshookz::provider::bindings
