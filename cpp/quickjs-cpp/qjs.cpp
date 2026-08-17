#include <jshookz/qjs.hpp>

namespace jshookz::qjs {

bool
freezeObject(JSContext *ctx, JSValueConst value)
{
    OwnedValue global(ctx, JS_GetGlobalObject(ctx));
    if (global.isException())
        return false;
    OwnedValue object = property(ctx, global.get(), "Object");
    if (object.isException())
        return false;
    OwnedValue freeze = property(ctx, object.get(), "freeze");
    if (freeze.isException() || !JS_IsFunction(ctx, freeze.get()))
        return false;
    JSValueConst arguments[] = {value};
    OwnedValue result(
        ctx, JS_Call(ctx, freeze.get(), object.get(), 1, arguments));
    return !result.isException();
}

JSValue
uint8Array(JSContext *ctx, std::span<std::uint8_t const> bytes)
{
    OwnedValue buffer(
        ctx, JS_NewArrayBufferCopy(ctx, bytes.data(), bytes.size()));
    if (buffer.isException())
        return buffer.release();
    OwnedValue offset(ctx, JS_NewInt32(ctx, 0));
    OwnedValue length(ctx, JS_NewUint32(ctx, bytes.size()));
    JSValueConst args[3] = {buffer.get(), offset.get(), length.get()};
    return JS_NewTypedArray(ctx, 3, args, JS_TYPED_ARRAY_UINT8);
}

JSValue
pendingOrTypeError(JSContext *ctx, char const *message)
{
    if (JS_HasException(ctx))
        return JS_EXCEPTION;
    return JS_ThrowTypeError(ctx, "%s", message);
}

}  // namespace jshookz::qjs
