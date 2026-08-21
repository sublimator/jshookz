#include "common.hpp"
#include "quickjs.hpp"
#include "hook_imports.hpp"

#include <cstdio>

namespace jshookz::provider::bindings {
namespace {

JSValue
// @binding provider:accept
js_hook_accept(JSContext *ctx, JSValueConst this_val,
               int argc, JSValueConst *argv)
{
    int64_t code = 0;
    if (argc > 1 && !JS_IsUndefined(argv[1]) &&
        JS_ToInt64(ctx, &code, argv[1]) < 0)
        return JS_EXCEPTION;

    if (argc == 0 || JS_IsUndefined(argv[0]))
        return JS_NewInt64(ctx, hook_accept(0, 0, code));

    /* A lifecycle message string is UTF-8 text, not the hex-string shorthand
       accepted by BytesLike APIs. accept deliberately does not invoke an
       arbitrary rich value's toBytes method before terminating execution. */
    auto message = qjs::ByteView::getBinding(ctx, argv[0], "accept", 0,
                                             qjs::BytePolicy::lifecycleMessage);
    if (!message)
        return qjs::byteInputTypeError(
            ctx, "accept", qjs::BytePolicy::lifecycleMessage);
    int64_t result = hook_accept(
        (uint32_t)(uintptr_t)message.data(), message.size(), code);
    return JS_NewInt64(ctx, result);
}

JSValue
// @binding provider:rollback
js_hook_rollback(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv)
{
    int64_t code = 0;
    if (argc > 1 && !JS_IsUndefined(argv[1]) &&
        JS_ToInt64(ctx, &code, argv[1]) < 0)
        return JS_EXCEPTION;
    if (argc == 0 || JS_IsUndefined(argv[0]))
        return JS_NewInt64(ctx, hook_rollback(0, 0, code));

    auto message = qjs::ByteView::getBinding(ctx, argv[0], "rollback", 0,
                                             qjs::BytePolicy::lifecycleMessage);
    if (!message)
        return qjs::byteInputTypeError(
            ctx, "rollback", qjs::BytePolicy::lifecycleMessage);
    int64_t result = hook_rollback(
        (uint32_t)(uintptr_t)message.data(), message.size(), code);
    return JS_NewInt64(ctx, result);
}

int
get_result_success(JSContext *ctx, JSValueConst value,
                   const char *function_name)
{
    if (!isResult(value)) {
        JS_ThrowTypeError(
            ctx, "%s: expected provider Result", function_name);
        return -1;
    }

    qjs::OwnedValue ok = qjs::property(ctx, value, "ok");
    if (ok.isException())
        return -1;
    if (!JS_IsBool(ok.get())) {
        JS_ThrowTypeError(ctx, "%s: expected boolean ok", function_name);
        return -1;
    }
    return JS_ToBool(ctx, ok.get());
}

int
get_result_array_length(JSContext *ctx, JSValueConst value,
                        const char *function_name, uint32_t *length)
{
    int const is_array = JS_IsArray(ctx, value);
    if (is_array < 0)
        return -1;
    if (!is_array) {
        JS_ThrowTypeError(ctx, "%s: expected Result array", function_name);
        return -1;
    }

    qjs::OwnedValue length_value = qjs::property(ctx, value, "length");
    if (length_value.isException())
        return -1;
    return JS_ToUint32(ctx, length, length_value.get());
}

JSValue
// @binding provider:rollback.onFail
js_rollback_on_fail(JSContext *ctx, JSValueConst this_val,
                    int argc, JSValueConst *argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "rollback.onFail: expected Result");
    int const success =
        get_result_success(ctx, argv[0], "rollback.onFail");
    if (success < 0)
        return JS_EXCEPTION;
    if (success)
        return qjs::property(ctx, argv[0], "value").release();

    int64_t code;
    int const explicit_code = argc > 2 && !JS_IsUndefined(argv[2]);
    if (explicit_code) {
        if (argc < 2 || JS_IsUndefined(argv[1]))
            return JS_ThrowTypeError(
                ctx,
                "rollback.onFail: uncoded Result requires message and code");
        if (!JS_IsNumber(argv[2]) || JS_ToInt64(ctx, &code, argv[2]) < 0)
            return JS_ThrowTypeError(
                ctx, "rollback.onFail: expected numeric rollback code");
    } else {
        qjs::OwnedValue error = qjs::property(ctx, argv[0], "error");
        if (error.isException())
            return error.release();
        qjs::OwnedValue code_value = qjs::property(ctx, error.get(), "code");
        if (code_value.isException())
            return code_value.release();
        if (!JS_IsNumber(code_value.get()) ||
            JS_ToInt64(ctx, &code, code_value.get()) < 0) {
            return JS_ThrowTypeError(
                ctx,
                "rollback.onFail: uncoded Result requires explicit code");
        }
    }

    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        qjs::OwnedValue code_arg(ctx, JS_NewInt64(ctx, code));
        JSValueConst rollback_args[2] = { argv[1], code_arg.get() };
        return js_hook_rollback(ctx, this_val, 2, rollback_args);
    }

    char message[64];
    int length = snprintf(
        message, sizeof(message), "host operation failed: %lld",
        (long long)code);
    if (length < 0 || (size_t)length >= sizeof(message))
        return JS_ThrowInternalError(
            ctx, "rollback.onFail: failed to format status");
    return JS_NewInt64(ctx, hook_rollback(
        (uint32_t)(uintptr_t)message, (uint32_t)length, code));
}

JSValue
call_lifecycle(
    JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
    int message_index,
    JSValue (*terminal)(JSContext *, JSValueConst, int, JSValueConst *))
{
    bool const has_code = argc > message_index + 1 &&
        !JS_IsUndefined(argv[message_index + 1]);
    if (argc <= message_index || JS_IsUndefined(argv[message_index])) {
        if (!has_code)
            return terminal(ctx, this_val, 0, nullptr);
        /* No message, explicit contract-owned code: the terminal reads the
         * code from its second slot and treats the undefined message as
         * absent (review finding: the code was silently dropped here). */
        JSValueConst coded[2] = {JS_UNDEFINED, argv[message_index + 1]};
        return terminal(ctx, this_val, 2, coded);
    }
    JSValueConst args[2] = {
        argv[message_index],
        argc > message_index + 1 ? argv[message_index + 1] : JS_UNDEFINED,
    };
    return terminal(ctx, this_val, has_code ? 2 : 1, args);
}

/* The named predicates are total over both carriers (0072:892-935): the
 * failure channel is entailed by the value predicate — a failed Result
 * cannot yield a present or truthy value — so a Result dispatches into
 * its success value and a direct value is tested as itself. Exact-void
 * Results remain ineligible. 1 = continue with owned *out, 0 = predicate
 * false, -1 = exception. */
int
take_present(JSContext *ctx, JSValueConst value, JSValue *out,
             const char *function_name)
{
    if (isEffectResult(value)) {
        JS_ThrowTypeError(
            ctx, "%s: a void-effect Result has no value", function_name);
        return -1;
    }
    if (isResult(value)) {
        int const success = get_result_success(ctx, value, function_name);
        if (success < 0)
            return -1;
        if (!success)
            return 0;
        qjs::OwnedValue inner = qjs::property(ctx, value, "value");
        if (inner.isException())
            return -1;
        if (JS_IsUndefined(inner.get()) || JS_IsNull(inner.get()))
            return 0;
        *out = inner.release();
        return 1;
    }
    if (JS_IsUndefined(value) || JS_IsNull(value))
        return 0;
    *out = JS_DupValue(ctx, value);
    return 1;
}

static int
take_truthy(JSContext *ctx, JSValueConst value, JSValue *out,
            const char *function_name)
{
    if (isEffectResult(value)) {
        JS_ThrowTypeError(
            ctx, "%s: a void-effect Result has no value", function_name);
        return -1;
    }
    if (isResult(value)) {
        int const success = get_result_success(ctx, value, function_name);
        if (success < 0)
            return -1;
        if (!success)
            return 0;
        qjs::OwnedValue inner = qjs::property(ctx, value, "value");
        if (inner.isException())
            return -1;
        int const truthy = JS_ToBool(ctx, inner.get());
        if (truthy < 0)
            return -1;
        if (!truthy)
            return 0;
        *out = inner.release();
        return 1;
    }
    int const truthy = JS_ToBool(ctx, value);
    if (truthy < 0)
        return -1;
    if (!truthy)
        return 0;
    *out = JS_DupValue(ctx, value);
    return 1;
}

JSValue
// @binding provider:rollback.requirePresent
js_rollback_require_present(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(
            ctx, "rollback.requirePresent: expected a Result or value");

    JSValue present = JS_UNDEFINED;
    int const status = take_present(
        ctx, argv[0], &present, "rollback.requirePresent");
    if (status < 0)
        return JS_EXCEPTION;
    if (status > 0)
        return present;

    if (argc < 2 || JS_IsUndefined(argv[1]))
        return JS_ThrowTypeError(
            ctx, "rollback.requirePresent: expected rollback message");
    return call_lifecycle(
        ctx, this_val, argc, argv, 1, js_hook_rollback);
}

JSValue
// @binding provider:rollback.requireTruthy
js_rollback_require_truthy(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(
            ctx, "rollback.requireTruthy: expected a Result or value");

    JSValue present = JS_UNDEFINED;
    int const status = take_truthy(
        ctx, argv[0], &present, "rollback.requireTruthy");
    if (status < 0)
        return JS_EXCEPTION;
    if (status > 0)
        return present;

    if (argc < 2 || JS_IsUndefined(argv[1]))
        return JS_ThrowTypeError(
            ctx, "rollback.requireTruthy: expected rollback message");
    return call_lifecycle(
        ctx, this_val, argc, argv, 1, js_hook_rollback);
}

JSValue
// @binding provider:rollback.when
js_rollback_when(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(
            ctx, "rollback.when: expected condition");
    int const truthy = JS_ToBool(ctx, argv[0]);
    if (truthy < 0)
        return JS_EXCEPTION;
    if (!truthy)
        return JS_UNDEFINED;
    return call_lifecycle(
        ctx, this_val, argc, argv, 1, js_hook_rollback);
}

JSValue
// @binding provider:accept.unlessPresent
js_accept_unless_present(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(
            ctx, "accept.unlessPresent: expected a Result or value");

    JSValue present = JS_UNDEFINED;
    int const status = take_present(
        ctx, argv[0], &present, "accept.unlessPresent");
    if (status < 0)
        return JS_EXCEPTION;
    if (status > 0)
        return present;
    return call_lifecycle(
        ctx, this_val, argc, argv, 1, js_hook_accept);
}

JSValue
// @binding provider:accept.unlessTruthy
js_accept_unless_truthy(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(
            ctx, "accept.unlessTruthy: expected a Result or value");

    JSValue truthy = JS_UNDEFINED;
    int const status = take_truthy(
        ctx, argv[0], &truthy, "accept.unlessTruthy");
    if (status < 0)
        return JS_EXCEPTION;
    if (status > 0)
        return truthy;
    return call_lifecycle(
        ctx, this_val, argc, argv, 1, js_hook_accept);
}

JSValue
// @binding provider:accept.when
js_accept_when(JSContext *ctx, JSValueConst this_val,
               int argc, JSValueConst *argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(
            ctx, "accept.when: expected condition");
    int const truthy = JS_ToBool(ctx, argv[0]);
    if (truthy < 0)
        return JS_EXCEPTION;
    if (!truthy)
        return JS_UNDEFINED;
    return call_lifecycle(
        ctx, this_val, argc, argv, 1, js_hook_accept);
}

JSValue
// @binding provider:rollback.onAnyFail
js_rollback_on_any_fail(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(
            ctx, "rollback.onAnyFail: expected Result array");

    uint32_t length;
    if (get_result_array_length(
            ctx, argv[0], "rollback.onAnyFail", &length) < 0)
        return JS_EXCEPTION;

    qjs::ArrayBuilder values(ctx);
    if (values.isException())
        return values.release();

    for (uint32_t index = 0; index < length; ++index) {
        qjs::OwnedValue item = qjs::element(ctx, argv[0], index);
        if (item.isException())
            return item.release();

        int const success =
            get_result_success(ctx, item.get(), "rollback.onAnyFail");
        if (success < 0)
            return JS_EXCEPTION;
        if (!success) {
            JSValueConst on_fail_args[3] = {
                item.get(),
                argc > 1 ? argv[1] : JS_UNDEFINED,
                argc > 2 ? argv[2] : JS_UNDEFINED,
            };
            int const on_fail_argc = argc > 2 ? 3 : argc > 1 ? 2 : 1;
            return js_rollback_on_fail(
                ctx, this_val, on_fail_argc, on_fail_args);
        }

        qjs::OwnedValue value = qjs::property(ctx, item.get(), "value");
        if (value.isException())
            return value.release();
        if (!values.append(std::move(value)))
            return JS_EXCEPTION;
    }

    return values.release();
}

JSValue
// @binding provider:rollback.onAllFail
js_rollback_on_all_fail(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(
            ctx, "rollback.onAllFail: expected Result array");

    uint32_t length;
    if (get_result_array_length(
            ctx, argv[0], "rollback.onAllFail", &length) < 0)
        return JS_EXCEPTION;

    qjs::ArrayBuilder values(ctx);
    if (values.isException())
        return values.release();

    for (uint32_t index = 0; index < length; ++index) {
        qjs::OwnedValue item = qjs::element(ctx, argv[0], index);
        if (item.isException())
            return item.release();

        int const success =
            get_result_success(ctx, item.get(), "rollback.onAllFail");
        if (success < 0)
            return JS_EXCEPTION;
        if (!success)
            continue;

        qjs::OwnedValue value = qjs::property(ctx, item.get(), "value");
        if (value.isException())
            return value.release();
        if (!values.append(std::move(value)))
            return JS_EXCEPTION;
    }

    if (values.size() > 0)
        return values.release();

    if (argc < 3 || JS_IsUndefined(argv[1]) ||
        !JS_IsNumber(argv[2]))
        return JS_ThrowTypeError(
            ctx,
            "rollback.onAllFail: expected message and numeric code");
    JSValueConst rollback_args[2] = { argv[1], argv[2] };
    return js_hook_rollback(ctx, this_val, 2, rollback_args);
}

}  // namespace

bool
registerControl(JSContext *ctx, JSValue global)
{
    qjs::OwnedValue accept(
        ctx, JS_NewCFunction(ctx, js_hook_accept, "accept", 2));
    if (accept.isException())
        return false;
    if (JS_SetPropertyStr(ctx, accept.get(), "unlessPresent",
            JS_NewCFunction(
                ctx, js_accept_unless_present, "unlessPresent", 3)) < 0 ||
        JS_SetPropertyStr(ctx, accept.get(), "unlessTruthy",
            JS_NewCFunction(
                ctx, js_accept_unless_truthy, "unlessTruthy", 3)) < 0 ||
        JS_SetPropertyStr(ctx, accept.get(), "when",
            JS_NewCFunction(ctx, js_accept_when, "when", 3)) < 0)
        return false;
    if (JS_SetPropertyStr(ctx, global, "accept", accept.release()) < 0)
        return false;

    qjs::OwnedValue rollback(
        ctx, JS_NewCFunction(ctx, js_hook_rollback, "rollback", 2));
    if (rollback.isException())
        return false;
    if (JS_SetPropertyStr(ctx, rollback.get(), "onFail",
            JS_NewCFunction(ctx, js_rollback_on_fail, "onFail", 3)) < 0 ||
        JS_SetPropertyStr(ctx, rollback.get(), "requirePresent",
            JS_NewCFunction(
                ctx, js_rollback_require_present, "requirePresent", 3)) < 0 ||
        JS_SetPropertyStr(ctx, rollback.get(), "requireTruthy",
            JS_NewCFunction(
                ctx, js_rollback_require_truthy, "requireTruthy", 3)) < 0 ||
        JS_SetPropertyStr(ctx, rollback.get(), "when",
            JS_NewCFunction(ctx, js_rollback_when, "when", 3)) < 0 ||
        JS_SetPropertyStr(ctx, rollback.get(), "onAnyFail",
            JS_NewCFunction(ctx, js_rollback_on_any_fail, "onAnyFail", 3)) < 0 ||
        JS_SetPropertyStr(ctx, rollback.get(), "onAllFail",
            JS_NewCFunction(ctx, js_rollback_on_all_fail, "onAllFail", 3)) < 0)
        return false;
    return JS_SetPropertyStr(
        ctx, global, "rollback", rollback.release()) >= 0;
}

}  // namespace jshookz::provider::bindings
