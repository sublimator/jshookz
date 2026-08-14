#include "common.hpp"
#include "hook_imports.hpp"

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
    return rich_from_bytes(ctx, "Hash256", bytes, sizeof(bytes));
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
        return host_failure(ctx, result);
    if (result != (int64_t)sizeof(bytes))
        return JS_ThrowInternalError(
            ctx, "hook.account: host returned %lld, expected 20",
            (long long)result);
    return host_success(ctx,
        rich_from_bytes(ctx, "AccountID", bytes, sizeof(bytes)));
}

JSCFunctionListEntry const js_ledger_properties[] = {
    JS_CGETSET_DEF("sequence", js_ledger_sequence, NULL),
    JS_CGETSET_DEF("lastTime", js_ledger_last_time, NULL),
    JS_CGETSET_DEF("lastHash", js_ledger_last_hash, NULL),
};

}  // namespace

void
registerHook(JSContext *ctx, JSValue global)
{
    JSValue hook = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, hook, "account",
        JS_NewCFunction(ctx, js_hook_account, "account", 0));
    JS_SetPropertyStr(ctx, global, "hook", hook);
}

void
registerLedger(JSContext *ctx, JSValue global)
{
    JSValue ledger = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(
        ctx,
        ledger,
        js_ledger_properties,
        sizeof(js_ledger_properties) / sizeof(js_ledger_properties[0]));
    JS_SetPropertyStr(ctx, global, "ledger", ledger);

    JSValue otxn = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, otxn, "type",
        JS_NewCFunction(ctx, js_otxn_type, "type", 0));
    JS_SetPropertyStr(ctx, global, "otxn", otxn);
}

}  // namespace jshookz::provider::bindings
