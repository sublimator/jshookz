#include "common.hpp"
#include "hook_imports.hpp"
#include "../provider_internal.hpp"
#include "keylet/keylet_js.hpp"
#include "object/object.hpp"
#include "record/record_js.hpp"

namespace jshookz::provider::bindings {
namespace {

JSValue originatingTransaction = JS_UNDEFINED;

enum class InvocationMode : std::uint8_t
{
    unavailable,
    strong,
    weak,
    again,
    callback,
};

InvocationMode invocationMode = InvocationMode::unavailable;

using ParameterReader = std::int64_t (*)(
    std::uint32_t,
    std::uint32_t,
    std::uint32_t,
    std::uint32_t);

[[nodiscard]] JSValue
readParameter(
    JSContext *ctx,
    qjs::ByteView const& name,
    char const *operation,
    ParameterReader reader,
    JSValueConst codec = JS_UNDEFINED)
{
    bool const typed = !JS_IsUndefined(codec);
    std::uint32_t codecByteLength = 0;
    if (typed && !types::readBinaryCodecByteLength(
                     ctx, codec, &codecByteLength))
        return JS_EXCEPTION;
    if (typed && codecByteLength > 256)
        return JS_ThrowRangeError(
            ctx, "%s: codec exceeds the 256-byte parameter limit", operation);

    // hook_param truncates to the supplied output capacity. Always fetch the
    // complete parameter ceiling before local decoding so an oversized value
    // cannot masquerade as a valid fixed-width prefix.
    std::uint8_t value[256];
    std::int64_t const result = reader(
        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(value)),
        sizeof(value),
        static_cast<std::uint32_t>(
            reinterpret_cast<std::uintptr_t>(name.data())),
        name.size());
    if (result == -5 || result == 0)
        return host_success(ctx, JS_UNDEFINED);
    if (result < 0)
        return host_failure(ctx, result);
    if (result > static_cast<std::int64_t>(sizeof(value)))
        return JS_ThrowInternalError(
            ctx,
            "%s: host returned oversized length %lld",
            operation,
            (long long)result);
    if (typed)
        return types::safeParseBinaryCodecBytes(
            ctx, codec, value, static_cast<std::uint32_t>(result));
    return host_success(ctx, makeSTBlob(
        ctx, value, static_cast<std::uint32_t>(result)));
}

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

[[nodiscard]] JSValue
lookupSlotFailure(JSContext *ctx, std::uint32_t slot, char const *stage,
                  std::int64_t result)
{
    std::int64_t const cleared = hook_slot_clear(slot);
    if (cleared != 1)
        return JS_ThrowInternalError(
            ctx,
            "ledger.lookup: slot_clear returned %lld after %s returned %lld",
            (long long)cleared,
            stage,
            (long long)result);
    return JS_ThrowInternalError(
        ctx, "ledger.lookup: %s returned %lld",
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
// @binding provider:ledger.feeBase
js_ledger_fee_base(JSContext *ctx, JSValueConst)
{
    std::int64_t const result = hook_fee_base();
    if (result < 0)
        return JS_ThrowInternalError(
            ctx,
            "ledger.feeBase: host violated total invocation fact with %lld",
            (long long)result);
    return JS_NewBigUint64(ctx, static_cast<std::uint64_t>(result));
}

JSValue
// @binding provider:otxn.type
js_otxn_type(JSContext *ctx, JSValueConst this_val,
             int argc, JSValueConst *argv)
{
    int64_t result = hook_otxn_type();
    if (result < 0)
        return JS_ThrowInternalError(
            ctx,
            "otxn.type: host violated total invocation fact with %lld",
            (long long)result);
    return JS_NewInt64(ctx, result);
}

JSValue
// @binding provider:otxn.id
js_otxn_id(JSContext *ctx, JSValueConst, int argc, JSValueConst *)
{
    if (argc > 0)
        return JS_ThrowTypeError(ctx, "otxn.id: takes no arguments");

    // Flagless: the raw flags word selects, inside an EmitFailure callback,
    // between the failed emitted transaction's hash (0, the same transaction
    // otxn.type() reports there) and the wrapper's own id (nonzero). The
    // typed API names the former; the latter is a separate, unearned read.
    // Total, like otxn.type(): an executing Hook always has an originating
    // id, so a negative host status is an invariant violation, not data.
    std::uint8_t bytes[32];
    std::int64_t const result = hook_otxn_id(
        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(bytes)),
        sizeof(bytes),
        0);
    if (result < 0)
        return JS_ThrowInternalError(
            ctx,
            "otxn.id: host violated total invocation fact with %lld",
            (long long)result);
    if (result != static_cast<std::int64_t>(sizeof(bytes)))
        return JS_ThrowInternalError(
            ctx,
            "otxn.id: host returned %lld, expected 32",
            (long long)result);
    return makeHash256(ctx, bytes, sizeof(bytes));
}

JSValue
// @binding provider:otxn.param
js_otxn_param(JSContext *ctx, JSValueConst this_val,
              int argc, JSValueConst *argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "otxn.param: expected a name");
    auto name = qjs::ByteView::getBinding(
        ctx, argv[0], "otxn.param", 0, qjs::BytePolicy::stateKeyLike);
    if (!name)
        return qjs::pendingOrTypeError(ctx, "otxn.param: invalid name");
    return readParameter(
        ctx,
        name,
        "otxn.param",
        hook_otxn_param,
        argc > 1 ? argv[1] : JS_UNDEFINED);
}

JSValue
materializeOriginatingTransaction(JSContext *ctx)
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

    JSValue object = types::makeCertifiedTransactionOwned(ctx, bytes, size);
    if (JS_IsException(object))
        return object;
    originatingTransaction = JS_DupValue(ctx, object);
    return object;
}

JSValue
// @binding provider:otxn.object
js_otxn_object(JSContext *ctx, JSValueConst this_val,
               int argc, JSValueConst *argv)
{
    return materializeOriginatingTransaction(ctx);
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

JSValue
// @binding provider:hook.param
js_hook_param(JSContext *ctx, JSValueConst this_val,
              int argc, JSValueConst *argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "hook.param: expected a name");
    auto name = qjs::ByteView::getBinding(
        ctx, argv[0], "hook.param", 0, qjs::BytePolicy::stateKeyLike);
    if (!name)
        return qjs::pendingOrTypeError(ctx, "hook.param: invalid name");
    return readParameter(
        ctx,
        name,
        "hook.param",
        hook_hook_param,
        argc > 1 ? argv[1] : JS_UNDEFINED);
}

JSValue
// @binding provider:hook.mode
js_hook_mode(JSContext *ctx, JSValueConst this_val,
             int argc, JSValueConst *argv)
{
    char const *value = nullptr;
    switch (invocationMode) {
    case InvocationMode::strong:
        value = "strong";
        break;
    case InvocationMode::weak:
        value = "weak";
        break;
    case InvocationMode::again:
        value = "again";
        break;
    case InvocationMode::callback:
        value = "callback";
        break;
    case InvocationMode::unavailable:
        return JS_ThrowInternalError(
            ctx, "hook.mode: invocation context is unavailable");
    }
    return JS_NewString(ctx, value);
}

JSValue
// @binding provider:hook.again
js_hook_again(JSContext *ctx, JSValueConst this_val,
              int argc, JSValueConst *argv)
{
    std::int64_t const result = hook_hook_again();
    if (result < 0)
        return host_effect_failure(ctx, result);
    if (result != 1)
        return JS_ThrowInternalError(
            ctx,
            "hook.again: host returned %lld, expected 1",
            (long long)result);
    return host_effect_success(ctx);
}

JSValue
// @binding provider:ledger.nonce
js_ledger_nonce(JSContext *ctx, JSValueConst this_val,
                int argc, JSValueConst *argv)
{
    std::uint8_t bytes[32];
    std::int64_t const result = hook_ledger_nonce(
        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(bytes)),
        sizeof(bytes));
    if (result < 0)
        return host_failure(ctx, result);
    if (result != static_cast<std::int64_t>(sizeof(bytes)))
        return JS_ThrowInternalError(
            ctx,
            "ledger.nonce: host returned %lld, expected 32",
            (long long)result);
    return host_success(ctx, makeHash256(ctx, bytes, sizeof(bytes)));
}

JSValue
// @binding provider:ledger.lookup
js_ledger_lookup(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    std::uint8_t keylet[34];
    types::LedgerKeyletKind kind;
    if (argc < 1 || !types::readLedgerKeylet(
                        argc > 0 ? argv[0] : JS_UNDEFINED, keylet, &kind))
        return JS_ThrowTypeError(
            ctx, "ledger.lookup: expected a provider LedgerKeylet");
    std::int64_t const measurementSlotResult = hook_slot_set(
        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(keylet)),
        sizeof(keylet), 0);
    if (measurementSlotResult == -5)
        return host_success(ctx, JS_UNDEFINED);
    if (measurementSlotResult < 0)
        return host_failure(ctx, measurementSlotResult);
    if (measurementSlotResult == 0 || measurementSlotResult > 255)
        return JS_ThrowInternalError(
            ctx, "ledger.lookup: slot_set returned %lld",
            (long long)measurementSlotResult);
    std::uint32_t const measurementSlot =
        static_cast<std::uint32_t>(measurementSlotResult);

    std::int64_t const measured = hook_slot_size(measurementSlot);
    if (measured <= 0 ||
        measured > static_cast<std::int64_t>(
            types::certifiedObjectMaxBytes()))
        return lookupSlotFailure(
            ctx, measurementSlot, "slot_size", measured);
    std::uint32_t const size = static_cast<std::uint32_t>(measured);
    std::int64_t const measurementCleared = hook_slot_clear(measurementSlot);
    if (measurementCleared != 1)
        return JS_ThrowInternalError(
            ctx, "ledger.lookup: measurement slot_clear returned %lld",
            (long long)measurementCleared);

    auto *bytes = static_cast<std::uint8_t *>(js_malloc(ctx, size));
    if (bytes == nullptr)
        return JS_HasException(ctx) ? JS_EXCEPTION : JS_ThrowOutOfMemory(ctx);

    std::int64_t const copySlotResult = hook_slot_set(
        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(keylet)),
        sizeof(keylet), 0);
    if (copySlotResult <= 0 || copySlotResult > 255) {
        js_free(ctx, bytes);
        return JS_ThrowInternalError(
            ctx, "ledger.lookup: second slot_set returned %lld",
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
        return lookupSlotFailure(ctx, copySlot, "slot", copied);
    }
    std::int64_t const cleared = hook_slot_clear(copySlot);
    if (cleared != 1) {
        js_free(ctx, bytes);
        return JS_ThrowInternalError(
            ctx, "ledger.lookup: slot_clear returned %lld",
            (long long)cleared);
    }

    JSValue object = JS_UNDEFINED;
    switch (kind) {
    case types::LedgerKeyletKind::generic:
    case types::LedgerKeyletKind::fees:
        object = types::makeCertifiedObjectOwned(ctx, bytes, size);
        break;
    case types::LedgerKeyletKind::accountRoot:
        object = types::makeCertifiedAccountRootOwned(ctx, bytes, size);
        break;
    case types::LedgerKeyletKind::uriToken:
        object = types::makeCertifiedURITokenOwned(ctx, bytes, size);
        break;
    case types::LedgerKeyletKind::hookLedger:
        object = types::makeCertifiedHookLedgerOwned(ctx, bytes, size);
        break;
    case types::LedgerKeyletKind::hookDefinition:
        object = types::makeCertifiedHookDefinitionOwned(ctx, bytes, size);
        break;
    }
    if (JS_IsException(object))
        return object;
    return host_success(ctx, object);
}

JSCFunctionListEntry const js_ledger_properties[] = {
    JS_CGETSET_DEF("sequence", js_ledger_sequence, NULL),
    JS_CGETSET_DEF("lastTime", js_ledger_last_time, NULL),
    JS_CGETSET_DEF("lastHash", js_ledger_last_hash, NULL),
    JS_CGETSET_DEF("feeBase", js_ledger_fee_base, NULL),
    JS_CFUNC_DEF("nonce", 0, js_ledger_nonce),
    JS_CFUNC_DEF("lookup", 1, js_ledger_lookup),
};

}  // namespace

JSValue
originatingTransactionObject(JSContext *ctx)
{
    return materializeOriginatingTransaction(ctx);
}

void
resetOriginatingTransactionCache(JSContext *ctx) noexcept
{
    JS_FreeValue(ctx, originatingTransaction);
    originatingTransaction = JS_UNDEFINED;
}

bool
setInvocationMode(
    JSContext *ctx,
    bool callback,
    std::uint32_t rawMode) noexcept
{
    if (invocationMode != InvocationMode::unavailable) {
        JS_ThrowInternalError(ctx, "hook.mode: stale invocation context");
        return false;
    }
    if (callback) {
        invocationMode = InvocationMode::callback;
        return true;
    }
    switch (rawMode) {
    case 0:
        invocationMode = InvocationMode::strong;
        return true;
    case 1:
        invocationMode = InvocationMode::weak;
        return true;
    case 2:
        invocationMode = InvocationMode::again;
        return true;
    default:
        JS_ThrowRangeError(
            ctx, "hook.mode: unsupported invocation mode %u", rawMode);
        return false;
    }
}

void
resetInvocationMode() noexcept
{
    invocationMode = InvocationMode::unavailable;
}

bool
registerHook(JSContext *ctx, JSValue global)
{
    qjs::OwnedValue hook(ctx, JS_NewObject(ctx));
    if (hook.isException())
        return false;
    if (JS_SetPropertyStr(ctx, hook.get(), "account",
            JS_NewCFunction(ctx, js_hook_account, "account", 0)) < 0 ||
        JS_SetPropertyStr(ctx, hook.get(), "param",
            JS_NewCFunction(ctx, js_hook_param, "param", 2)) < 0 ||
        JS_SetPropertyStr(ctx, hook.get(), "mode",
            JS_NewCFunction(ctx, js_hook_mode, "mode", 0)) < 0 ||
        JS_SetPropertyStr(ctx, hook.get(), "again",
            JS_NewCFunction(ctx, js_hook_again, "again", 0)) < 0)
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
    if (JS_SetPropertyStr(ctx, otxn.get(), "id",
            JS_NewCFunction(ctx, js_otxn_id, "id", 0)) < 0)
        return false;
    if (JS_SetPropertyStr(ctx, otxn.get(), "param",
            JS_NewCFunction(ctx, js_otxn_param, "param", 2)) < 0)
        return false;
    if (JS_SetPropertyStr(ctx, otxn.get(), "object",
            JS_NewCFunction(ctx, js_otxn_object, "object", 0)) < 0)
        return false;
    return JS_SetPropertyStr(ctx, global, "otxn", otxn.release()) >= 0;
}

}  // namespace jshookz::provider::bindings
