#include "common.hpp"
#include "hook_imports.hpp"
#include "../provider_internal.hpp"

namespace jshookz::provider::bindings {
namespace {

JSValue
// @binding provider:emit.prepare
js_emit_prepare(JSContext *ctx, JSValueConst this_val,
                int argc, JSValueConst *argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "emit.prepare: expected transaction bytes");
    auto transaction = qjs::ByteView::getBinding(
        ctx, argv[0], "emit.prepare", 0, qjs::BytePolicy::bytesLikeOrSTBlob);
    if (!transaction)
        return qjs::pendingOrTypeError(
            ctx, "emit.prepare: invalid transaction bytes");

    /* Preparation injects bounded protocol fields into the partial STObject.
       Keep the declared buffer proportional to its input so host-work
       charging reflects this call, while the checked host adapter returns
       TOO_SMALL without writing if this allowance is ever insufficient. */
    static const uint32_t max_transaction_bytes = 1024U * 1024U;
    static const uint32_t preparation_allowance = 4U * 1024U;
    uint64_t requested_capacity =
        (uint64_t)transaction.size() + preparation_allowance;
    uint32_t output_capacity = requested_capacity < max_transaction_bytes
        ? (uint32_t)requested_capacity
        : max_transaction_bytes;
    uint8_t *output = (uint8_t *)js_malloc(ctx, output_capacity);
    if (!output)
        return JS_EXCEPTION;
    int64_t result = hook_prepare(
        (uint32_t)(uintptr_t)output, output_capacity,
        (uint32_t)(uintptr_t)transaction.data(), transaction.size());
    if (result < 0) {
        js_free(ctx, output);
        return host_failure(ctx, result);
    }
    if ((uint64_t)result > output_capacity) {
        js_free(ctx, output);
        return JS_ThrowInternalError(
            ctx, "emit.prepare: host returned oversized length %lld",
            (long long)result);
    }
    JSValue value = makeSTBlob(ctx, output, (uint32_t)result);
    js_free(ctx, output);
    return host_success(ctx, value);
}

JSValue
// @binding provider:emit.reserve
js_emit_reserve(JSContext *ctx, JSValueConst this_val,
                int argc, JSValueConst *argv)
{
    uint32_t count;
    if (argc < 1 || JS_ToUint32(ctx, &count, argv[0]))
        return JS_ThrowTypeError(ctx, "emit.reserve: expected count");
    int64_t result = hook_etxn_reserve(count);
    return result < 0
        ? host_effect_failure(ctx, result)
        : host_effect_success(ctx);
}

JSValue
// @binding provider:emit.tx
js_emit_tx(JSContext *ctx, JSValueConst this_val,
           int argc, JSValueConst *argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "emit.tx: expected transaction bytes");
    auto transaction = qjs::ByteView::getBinding(
        ctx, argv[0], "emit.tx", 0, qjs::BytePolicy::bytesLikeOrSTBlob);
    if (!transaction)
        return qjs::pendingOrTypeError(
            ctx, "emit.tx: invalid transaction bytes");

    uint8_t hash[32];
    int64_t result = hook_emit(
        (uint32_t)(uintptr_t)hash, sizeof(hash),
        (uint32_t)(uintptr_t)transaction.data(), transaction.size());
    if (result < 0)
        return host_failure(ctx, result);
    if (result != (int64_t)sizeof(hash))
        return JS_ThrowInternalError(
            ctx, "emit.tx: host returned length %lld", (long long)result);
    return host_success(ctx, makeHash256(ctx, hash, sizeof(hash)));
}

}  // namespace

bool
registerEmission(JSContext *ctx, JSValue global)
{
    qjs::OwnedValue emit(ctx, JS_NewObject(ctx));
    if (emit.isException())
        return false;
    if (JS_SetPropertyStr(ctx, emit.get(), "reserve",
            JS_NewCFunction(ctx, js_emit_reserve, "reserve", 1)) < 0 ||
        JS_SetPropertyStr(ctx, emit.get(), "prepare",
            JS_NewCFunction(ctx, js_emit_prepare, "prepare", 1)) < 0 ||
        JS_SetPropertyStr(ctx, emit.get(), "tx",
            JS_NewCFunction(ctx, js_emit_tx, "tx", 1)) < 0)
        return false;
    return JS_SetPropertyStr(ctx, global, "emit", emit.release()) >= 0;
}

}  // namespace jshookz::provider::bindings
