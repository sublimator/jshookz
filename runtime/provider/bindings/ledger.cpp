#include "common.hpp"
#include "hook_imports.hpp"
#include "../provider_internal.hpp"

namespace jshookz::provider::bindings {
namespace {

JSValue
js_ledger_sequence(JSContext *ctx, JSValueConst this_val)
{
    return JS_NewInt64(ctx, hook_ledger_seq());
}

JSValue
js_ledger_last_time(JSContext *ctx, JSValueConst this_val)
{
    return JS_NewInt64(ctx, hook_ledger_last_time());
}

JSValue
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
js_otxn_type(JSContext *ctx, JSValueConst this_val,
             int argc, JSValueConst *argv)
{
    int64_t result = hook_otxn_type();
    return result < 0
        ? host_failure(ctx, result)
        : host_success(ctx, JS_NewInt64(ctx, result));
}

JSValue
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
    return JS_SetPropertyStr(ctx, global, "otxn", otxn.release()) >= 0;
}

}  // namespace jshookz::provider::bindings
