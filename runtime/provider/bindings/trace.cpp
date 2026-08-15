#include "common.hpp"
#include "hook_imports.hpp"

namespace jshookz::provider::bindings {
namespace {

JSValue
js_trace(JSContext *ctx, JSValueConst this_val,
         int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "trace: expected a string label");

    auto label = qjs::ByteView::get(
        ctx, argv[0], qjs::BytePolicy::traceLabel);
    if (!label)
        return qjs::byteInputTypeError(
            ctx, "trace label", qjs::BytePolicy::traceLabel);

    qjs::OwnedValue rendered(ctx);
    qjs::ByteView data = qjs::ByteView::get(
        ctx, JS_UNDEFINED, qjs::BytePolicy::traceValue);
    std::uint32_t asHex = 0;

    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        if (JS_IsObject(argv[1])) {
            data = qjs::ByteView::get(
                ctx,
                argv[1],
                qjs::BytePolicy::traceValue);
            if (data) {
                asHex = 1;
            } else if (JS_HasException(ctx)) {
                return JS_EXCEPTION;
            }
        }

        if (!data) {
            rendered = qjs::OwnedValue(
                ctx, JS_ToString(ctx, argv[1]));
            if (rendered.isException())
                return rendered.release();
            data = qjs::ByteView::get(
                ctx, rendered.get(), qjs::BytePolicy::traceLabel);
            if (!data)
                return qjs::byteInputTypeError(
                    ctx, "trace value", qjs::BytePolicy::traceLabel);
        }
    }

    int64_t const result = hook_trace(
        static_cast<std::uint32_t>(
            reinterpret_cast<std::uintptr_t>(label.data())),
        label.size(),
        static_cast<std::uint32_t>(
            reinterpret_cast<std::uintptr_t>(data.data())),
        data.size(),
        asHex);
    if (result < 0)
        return JS_ThrowInternalError(
            ctx, "trace: host returned %lld", (long long)result);
    return JS_UNDEFINED;
}

}  // namespace

bool
registerTrace(JSContext *ctx, JSValue global)
{
    return JS_SetPropertyStr(ctx, global, "trace",
        JS_NewCFunction(ctx, js_trace, "trace", 2)) >= 0;
}

}  // namespace jshookz::provider::bindings
