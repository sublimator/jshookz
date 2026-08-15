#include "common.hpp"
#include "../provider_internal.hpp"

namespace jshookz::provider::bindings {
namespace {

JSClassID resultClassId;
JSClassID effectResultClassId;
JSClassDef const resultClass = {"Result"};

JSValue
resultOkOr(
    JSContext* ctx,
    JSValueConst thisValue,
    int argc,
    JSValueConst* argv)
{
    if (!isResult(thisValue))
        return JS_ThrowTypeError(ctx, "Result.okOr: invalid receiver");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Result.okOr: expected fallback value");

    qjs::OwnedValue ok = qjs::property(ctx, thisValue, "ok");
    if (ok.isException())
        return ok.release();
    if (!JS_IsBool(ok.get()))
        return JS_ThrowTypeError(ctx, "Result.okOr: expected boolean ok");
    if (JS_ToBool(ctx, ok.get()))
        return qjs::property(ctx, thisValue, "value").release();
    return JS_DupValue(ctx, argv[0]);
}

JSValue
resultOkOrHandle(
    JSContext* ctx,
    JSValueConst thisValue,
    int argc,
    JSValueConst* argv)
{
    if (!isResult(thisValue))
        return JS_ThrowTypeError(ctx, "Result.okOrHandle: invalid receiver");
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(
            ctx, "Result.okOrHandle: expected error handler");

    qjs::OwnedValue ok = qjs::property(ctx, thisValue, "ok");
    if (ok.isException())
        return ok.release();
    if (!JS_IsBool(ok.get()))
        return JS_ThrowTypeError(
            ctx, "Result.okOrHandle: expected boolean ok");
    if (JS_ToBool(ctx, ok.get()))
        return qjs::property(ctx, thisValue, "value").release();

    qjs::OwnedValue error = qjs::property(ctx, thisValue, "error");
    if (error.isException())
        return error.release();
    JSValueConst arguments[1] = {error.get()};
    return JS_Call(ctx, argv[0], JS_UNDEFINED, 1, arguments);
}

JSValue
resultOkMapOr(
    JSContext* ctx,
    JSValueConst thisValue,
    int argc,
    JSValueConst* argv)
{
    if (!isResult(thisValue))
        return JS_ThrowTypeError(ctx, "Result.okMapOr: invalid receiver");
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(
            ctx, "Result.okMapOr: expected success handler");
    if (argc < 2)
        return JS_ThrowTypeError(
            ctx, "Result.okMapOr: expected fallback value");

    qjs::OwnedValue ok = qjs::property(ctx, thisValue, "ok");
    if (ok.isException())
        return ok.release();
    if (!JS_IsBool(ok.get()))
        return JS_ThrowTypeError(
            ctx, "Result.okMapOr: expected boolean ok");
    if (!JS_ToBool(ctx, ok.get()))
        return JS_DupValue(ctx, argv[1]);

    qjs::OwnedValue value = qjs::property(ctx, thisValue, "value");
    if (value.isException())
        return value.release();
    JSValueConst arguments[1] = {value.get()};
    return JS_Call(ctx, argv[0], JS_UNDEFINED, 1, arguments);
}

JSValue
resultMoot(
    JSContext* ctx,
    JSValueConst thisValue,
    int,
    JSValueConst*)
{
    if (!JS_IsObject(thisValue) ||
        JS_GetClassID(thisValue) != effectResultClassId)
        return JS_ThrowTypeError(
            ctx, "Result.moot: expected void-effect Result");
    return JS_UNDEFINED;
}

#define RESULT_CFUNC_DEF(name, length, callback)                              \
    {                                                                         \
        name, 0, JS_DEF_CFUNC, 0,                                             \
            .u = {.func = {                                                   \
                      length, JS_CFUNC_generic, {.generic = callback}}}       \
    }

JSCFunctionListEntry const resultPrototypeFunctions[] = {
    RESULT_CFUNC_DEF("okOr", 1, resultOkOr),
    RESULT_CFUNC_DEF("okOrHandle", 1, resultOkOrHandle),
    RESULT_CFUNC_DEF("okMapOr", 2, resultOkMapOr),
};

JSCFunctionListEntry const effectResultPrototypeFunctions[] = {
    RESULT_CFUNC_DEF("moot", 0, resultMoot),
};

#undef RESULT_CFUNC_DEF

JSValue
newResult(JSContext* ctx, bool effect)
{
    return JS_NewObjectClass(
        ctx, effect ? effectResultClassId : resultClassId);
}

bool
finishResult(JSContext* ctx, JSValueConst result)
{
    return JS_PreventExtensions(ctx, result) >= 0;
}

JSValue
newError(JSContext* ctx, char const* domain)
{
    qjs::OwnedValue error(ctx, JS_NewObjectProto(ctx, JS_NULL));
    if (error.isException())
        return error.release();
    if (JS_DefinePropertyValueStr(
            ctx,
            error.get(),
            "domain",
            JS_NewString(ctx, domain),
            JS_PROP_ENUMERABLE) < 0)
        return JS_EXCEPTION;
    return error.release();
}

JSValue
resultFailure(JSContext* ctx, JSValue error, bool effect = false)
{
    if (JS_IsException(error))
        return error;
    qjs::OwnedValue ownedError(ctx, error);
    qjs::OwnedValue result(ctx, newResult(ctx, effect));
    if (result.isException())
        return result.release();
    if (JS_DefinePropertyValueStr(
            ctx, result.get(), "ok", JS_FALSE, JS_PROP_ENUMERABLE) < 0 ||
        JS_DefinePropertyValueStr(
            ctx,
            result.get(),
            "error",
            ownedError.release(),
            JS_PROP_ENUMERABLE) < 0 ||
        !finishResult(ctx, result.get()))
        return JS_EXCEPTION;
    return result.release();
}

}  // namespace

bool
registerResult(JSContext* ctx)
{
    JS_NewClassID(&resultClassId);
    if (JS_NewClass(JS_GetRuntime(ctx), resultClassId, &resultClass) < 0)
        return false;
    JS_NewClassID(&effectResultClassId);
    if (JS_NewClass(JS_GetRuntime(ctx), effectResultClassId, &resultClass) < 0)
        return false;
    qjs::OwnedValue prototype(ctx, JS_NewObject(ctx));
    if (prototype.isException())
        return false;
    if (!qjs::installFunctions(
            ctx, prototype.get(), resultPrototypeFunctions))
        return false;
    if (!qjs::freezeObject(ctx, prototype.get()))
        return false;

    qjs::OwnedValue effectPrototype(
        ctx, JS_NewObjectProto(ctx, prototype.get()));
    if (effectPrototype.isException())
        return false;
    if (!qjs::installFunctions(
            ctx, effectPrototype.get(), effectResultPrototypeFunctions) ||
        !qjs::freezeObject(ctx, effectPrototype.get()))
        return false;

    JS_SetClassProto(ctx, resultClassId, prototype.release());
    JS_SetClassProto(ctx, effectResultClassId, effectPrototype.release());
    return !JS_HasException(ctx);
}

bool
isResult(JSValueConst value) noexcept
{
    if (!JS_IsObject(value))
        return false;
    JSClassID const classId = JS_GetClassID(value);
    return classId == resultClassId || classId == effectResultClassId;
}

bool
isEffectResult(JSValueConst value) noexcept
{
    return JS_IsObject(value) &&
        JS_GetClassID(value) == effectResultClassId;
}

JSValue
result_success(JSContext *ctx, JSValue value)
{
    if (JS_IsException(value))
        return value;
    qjs::OwnedValue ownedValue(ctx, value);
    qjs::OwnedValue result(ctx, newResult(ctx, false));
    if (result.isException())
        return result.release();
    if (JS_DefinePropertyValueStr(
            ctx, result.get(), "ok", JS_TRUE, JS_PROP_ENUMERABLE) < 0)
        return JS_EXCEPTION;
    if (JS_DefinePropertyValueStr(
            ctx,
            result.get(),
            "value",
            ownedValue.release(),
            JS_PROP_ENUMERABLE) < 0 ||
        !finishResult(ctx, result.get()))
        return JS_EXCEPTION;
    return result.release();
}

JSValue
uint_failure(
    JSContext *ctx, char const *issue, std::uint32_t bits)
{
    qjs::OwnedValue error(ctx, newError(ctx, "uint"));
    if (error.isException())
        return error.release();
    if (JS_DefinePropertyValueStr(
            ctx,
            error.get(),
            "issue",
            JS_NewString(ctx, issue),
            JS_PROP_ENUMERABLE) < 0 ||
        JS_DefinePropertyValueStr(
            ctx,
            error.get(),
            "bits",
            JS_NewUint32(ctx, bits),
            JS_PROP_ENUMERABLE) < 0 ||
        !finishResult(ctx, error.get()))
        return JS_EXCEPTION;
    return resultFailure(ctx, error.release());
}

JSValue
host_success(JSContext *ctx, JSValue value)
{
    return result_success(ctx, value);
}

JSValue
host_failure(JSContext *ctx, int64_t code)
{
    qjs::OwnedValue error(ctx, newError(ctx, "host"));
    if (error.isException())
        return error.release();
    if (JS_DefinePropertyValueStr(
            ctx,
            error.get(),
            "code",
            JS_NewInt64(ctx, code),
            JS_PROP_ENUMERABLE) < 0 ||
        !finishResult(ctx, error.get()))
        return JS_EXCEPTION;
    return resultFailure(ctx, error.release());
}

JSValue
host_effect_success(JSContext* ctx)
{
    qjs::OwnedValue result(ctx, newResult(ctx, true));
    if (result.isException())
        return result.release();
    if (JS_DefinePropertyValueStr(
            ctx, result.get(), "ok", JS_TRUE, JS_PROP_ENUMERABLE) < 0 ||
        JS_DefinePropertyValueStr(
            ctx,
            result.get(),
            "value",
            JS_UNDEFINED,
            JS_PROP_ENUMERABLE) < 0 ||
        !finishResult(ctx, result.get()))
        return JS_EXCEPTION;
    return result.release();
}

JSValue
host_effect_failure(JSContext* ctx, int64_t code)
{
    qjs::OwnedValue error(ctx, newError(ctx, "host"));
    if (error.isException())
        return error.release();
    if (JS_DefinePropertyValueStr(
            ctx,
            error.get(),
            "code",
            JS_NewInt64(ctx, code),
            JS_PROP_ENUMERABLE) < 0 ||
        !finishResult(ctx, error.get()))
        return JS_EXCEPTION;
    return resultFailure(ctx, error.release(), true);
}

}  // namespace jshookz::provider::bindings
