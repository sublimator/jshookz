#include "common.hpp"
#include "hook_imports.hpp"
#include "../provider_internal.hpp"
#include "js.hpp"
#include "object/nominal_payload.hpp"
#include "record/record_js.hpp"

#include <cstring>

namespace jshookz::provider::bindings {
namespace {

JSClassID foreignStateAccessorClassId = 0;

struct ForeignStateAccessorState
{
    std::uint8_t account[20];
    std::uint8_t namespaceId[32];
};

void
foreignStateAccessorFinalizer(JSRuntime *runtime, JSValue value)
{
    auto *state = static_cast<ForeignStateAccessorState *>(
        JS_GetOpaque(value, foreignStateAccessorClassId));
    if (state != nullptr)
        js_free_rt(runtime, state);
}

JSClassDef const foreignStateAccessorClass{
    .class_name = "ForeignStateAccessor",
    .finalizer = foreignStateAccessorFinalizer,
};

JSValue
js_foreign_state_get(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv);
JSValue
js_foreign_state_set(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv);
JSValue
js_foreign_state_del(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv);

JSCFunctionListEntry const foreignStateAccessorPrototype[] = {
    JS_CFUNC_DEF("get", 2, js_foreign_state_get),
    JS_CFUNC_DEF("set", 2, js_foreign_state_set),
    JS_CFUNC_DEF("del", 1, js_foreign_state_del),
};

JSValue
// @binding provider:state.get
js_state_get(JSContext *ctx, JSValueConst this_val,
             int argc, JSValueConst *argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "state.get: expected a key");
    auto key = qjs::ByteView::getBinding(ctx, argv[0], "state.get", 0,
                                         qjs::BytePolicy::stateKeyLike);
    if (!key)
        return qjs::pendingOrTypeError(ctx, "state.get: invalid key");

    bool const typed = argc > 1;
    std::uint32_t capacity = 4096;
    if (typed && !types::readBinaryCodecByteLength(
                     ctx, argv[1], &capacity))
        return JS_EXCEPTION;
    if (capacity > 4096)
        return JS_ThrowRangeError(
            ctx, "state.get: schema exceeds the 4096-byte state limit");

    /* Untyped state retains the complete state ceiling. Typed state passes
       the exact schema capacity, preserving TOO_SMALL as a host failure. */
    uint8_t value[4096];
    int64_t result = hook_state(
        (uint32_t)(uintptr_t)value, capacity,
        (uint32_t)(uintptr_t)key.data(), key.size());

    if (result == -5) /* DOESNT_EXIST is typed absence, not host failure. */
        return host_success(ctx, JS_UNDEFINED);
    if (result < 0)
        return host_failure(ctx, result);
    if ((uint64_t)result > capacity)
        return JS_ThrowInternalError(
            ctx, "state.get: host returned oversized length %lld",
            (long long)result);
    if (typed)
        return types::safeParseBinaryCodecBytes(
            ctx, argv[1], value, static_cast<std::uint32_t>(result));
    return host_success(ctx, makeSTBlob(ctx, value, (uint32_t)result));
}

JSValue
// @binding provider:state.set
js_state_set(JSContext *ctx, JSValueConst this_val,
             int argc, JSValueConst *argv)
{
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "state.set: expected key and value");
    auto key = qjs::ByteView::getBinding(ctx, argv[0], "state.set", 0,
                                         qjs::BytePolicy::stateKeyLike);
    if (!key)
        return qjs::pendingOrTypeError(ctx, "state.set: invalid key");
    /* Parsing a rich value executes its toBytes method. Snapshot the key so
       that method cannot detach or resize the key's ArrayBuffer underneath
       the subsequent host call. */
    if (!key.snapshot())
        return JS_EXCEPTION;
    auto value = qjs::ByteView::getBinding(ctx, argv[1], "state.set", 1,
                                           qjs::BytePolicy::stateValueLike);
    if (!value)
        return qjs::pendingOrTypeError(ctx, "state.set: invalid value");

    int64_t result = hook_state_set(
        (uint32_t)(uintptr_t)value.data(), value.size(),
        (uint32_t)(uintptr_t)key.data(), key.size());
    return result < 0
        ? host_effect_failure(ctx, result)
        : host_effect_success(ctx);
}

JSValue
// @binding provider:state.del
js_state_del(JSContext *ctx, JSValueConst this_val,
             int argc, JSValueConst *argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "state.del: expected a key");
    auto key = qjs::ByteView::getBinding(ctx, argv[0], "state.del", 0,
                                         qjs::BytePolicy::stateKeyLike);
    if (!key)
        return qjs::pendingOrTypeError(ctx, "state.del: invalid key");

    int64_t result = hook_state_set(
        0, 0, (uint32_t)(uintptr_t)key.data(), key.size());
    return result < 0
        ? host_effect_failure(ctx, result)
        : host_effect_success(ctx);
}

JSValue
// @binding provider:state.ForeignAccessor.get
js_foreign_state_get(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv)
{
    auto *state = qjs::opaque<ForeignStateAccessorState>(
        ctx, this_val, foreignStateAccessorClassId);
    if (state == nullptr)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(
            ctx, "state.ForeignAccessor.get: expected a key");
    auto key = qjs::ByteView::getBinding(
        ctx, argv[0], "state.ForeignAccessor.get", 0,
        qjs::BytePolicy::stateKeyLike);
    if (!key)
        return qjs::pendingOrTypeError(
            ctx, "state.ForeignAccessor.get: invalid key");

    bool const typed = argc > 1;
    std::uint32_t capacity = 4096;
    if (typed && !types::readBinaryCodecByteLength(
                     ctx, argv[1], &capacity))
        return JS_EXCEPTION;
    if (capacity > 4096)
        return JS_ThrowRangeError(
            ctx,
            "state.ForeignAccessor.get: schema exceeds the 4096-byte state limit");

    uint8_t value[4096];
    int64_t result = hook_state_foreign(
        (uint32_t)(uintptr_t)value, capacity,
        (uint32_t)(uintptr_t)key.data(), key.size(),
        (uint32_t)(uintptr_t)state->namespaceId, sizeof(state->namespaceId),
        (uint32_t)(uintptr_t)state->account, sizeof(state->account));

    if (result == -5)
        return host_success(ctx, JS_UNDEFINED);
    if (result < 0)
        return host_failure(ctx, result);
    if ((uint64_t)result > capacity)
        return JS_ThrowInternalError(
            ctx,
            "state.ForeignAccessor.get: host returned oversized length %lld",
            (long long)result);
    if (typed)
        return types::safeParseBinaryCodecBytes(
            ctx, argv[1], value, static_cast<std::uint32_t>(result));
    return host_success(ctx, makeSTBlob(ctx, value, (uint32_t)result));
}

JSValue
// @binding provider:state.ForeignAccessor.set
js_foreign_state_set(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv)
{
    auto *state = qjs::opaque<ForeignStateAccessorState>(
        ctx, this_val, foreignStateAccessorClassId);
    if (state == nullptr)
        return JS_EXCEPTION;
    if (argc < 2)
        return JS_ThrowTypeError(
            ctx, "state.ForeignAccessor.set: expected key and value");
    auto key = qjs::ByteView::getBinding(
        ctx, argv[0], "state.ForeignAccessor.set", 0,
        qjs::BytePolicy::stateKeyLike);
    if (!key)
        return qjs::pendingOrTypeError(
            ctx, "state.ForeignAccessor.set: invalid key");
    if (!key.snapshot())
        return JS_EXCEPTION;
    auto value = qjs::ByteView::getBinding(
        ctx, argv[1], "state.ForeignAccessor.set", 1,
        qjs::BytePolicy::stateValueLike);
    if (!value)
        return qjs::pendingOrTypeError(
            ctx, "state.ForeignAccessor.set: invalid value");

    std::int64_t const result = hook_state_foreign_set(
        static_cast<std::uint32_t>(
            reinterpret_cast<std::uintptr_t>(value.data())),
        value.size(),
        static_cast<std::uint32_t>(
            reinterpret_cast<std::uintptr_t>(key.data())),
        key.size(),
        static_cast<std::uint32_t>(
            reinterpret_cast<std::uintptr_t>(state->namespaceId)),
        sizeof(state->namespaceId),
        static_cast<std::uint32_t>(
            reinterpret_cast<std::uintptr_t>(state->account)),
        sizeof(state->account));
    return result < 0
        ? host_effect_failure(ctx, result)
        : host_effect_success(ctx);
}

JSValue
// @binding provider:state.ForeignAccessor.del
js_foreign_state_del(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv)
{
    auto *state = qjs::opaque<ForeignStateAccessorState>(
        ctx, this_val, foreignStateAccessorClassId);
    if (state == nullptr)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(
            ctx, "state.ForeignAccessor.del: expected a key");
    auto key = qjs::ByteView::getBinding(
        ctx, argv[0], "state.ForeignAccessor.del", 0,
        qjs::BytePolicy::stateKeyLike);
    if (!key)
        return qjs::pendingOrTypeError(
            ctx, "state.ForeignAccessor.del: invalid key");

    std::int64_t const result = hook_state_foreign_set(
        0, 0,
        static_cast<std::uint32_t>(
            reinterpret_cast<std::uintptr_t>(key.data())),
        key.size(),
        static_cast<std::uint32_t>(
            reinterpret_cast<std::uintptr_t>(state->namespaceId)),
        sizeof(state->namespaceId),
        static_cast<std::uint32_t>(
            reinterpret_cast<std::uintptr_t>(state->account)),
        sizeof(state->account));
    return result < 0
        ? host_effect_failure(ctx, result)
        : host_effect_success(ctx);
}

JSValue
// @binding provider:state.foreign
js_state_foreign(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv)
{
    if (argc < 2)
        return JS_ThrowTypeError(
            ctx, "state.foreign: expected AccountID and Hash256");

    std::uint8_t account[20]{};
    if (!types::readAccountIDBytes(ctx, argv[0], account))
        return JS_ThrowTypeError(
            ctx, "state.foreign: argument 0 must be an AccountID");

    std::uint8_t integerScratch[8]{};
    types::NominalPayloadView namespaceId{};
    if (!types::readNominalPayload(
            ctx,
            argv[1],
            catl::xdata::MaterializerKind::hash256,
            integerScratch,
            namespaceId) ||
        namespaceId.size != 32)
        return JS_ThrowTypeError(
            ctx, "state.foreign: argument 1 must be a Hash256");

    auto *state = static_cast<ForeignStateAccessorState *>(
        js_mallocz(ctx, sizeof(ForeignStateAccessorState)));
    if (state == nullptr)
        return JS_ThrowOutOfMemory(ctx);
    std::memcpy(state->account, account, sizeof(state->account));
    std::memcpy(
        state->namespaceId, namespaceId.data, sizeof(state->namespaceId));

    qjs::OwnedValue accessor(
        ctx, JS_NewObjectClass(ctx, foreignStateAccessorClassId));
    if (accessor.isException()) {
        js_free(ctx, state);
        return accessor.release();
    }
    JS_SetOpaque(accessor.get(), state);
    if (!qjs::freezeObject(ctx, accessor.get()))
        return JS_EXCEPTION;
    return accessor.release();
}

}  // namespace

bool
registerState(JSContext *ctx, JSValue global)
{
    if (!::jshookz::qjs::defineClass(
            JS_GetRuntime(ctx),
            &foreignStateAccessorClassId,
            &foreignStateAccessorClass) ||
        !::jshookz::qjs::installPrototype(
            ctx,
            foreignStateAccessorClassId,
            foreignStateAccessorPrototype))
        return false;

    qjs::OwnedValue state(ctx, JS_NewObject(ctx));
    if (state.isException())
        return false;
    if (JS_SetPropertyStr(ctx, state.get(), "get",
            JS_NewCFunction(ctx, js_state_get, "get", 2)) < 0 ||
        JS_SetPropertyStr(ctx, state.get(), "set",
            JS_NewCFunction(ctx, js_state_set, "set", 2)) < 0 ||
        JS_SetPropertyStr(ctx, state.get(), "del",
            JS_NewCFunction(ctx, js_state_del, "del", 1)) < 0 ||
        JS_SetPropertyStr(ctx, state.get(), "foreign",
            JS_NewCFunction(ctx, js_state_foreign, "foreign", 2)) < 0)
        return false;
    return JS_SetPropertyStr(ctx, global, "state", state.release()) >= 0;
}

}  // namespace jshookz::provider::bindings
