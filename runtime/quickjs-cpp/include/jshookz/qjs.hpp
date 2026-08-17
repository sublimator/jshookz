#pragma once

#include <cstdint>
#include <span>
#include <utility>

extern "C" {
#include "../../../../engine/quickjs/quickjs.h"
}

namespace jshookz::qjs {

/** One owned QuickJS value tied to the callback-local JSContext lifetime. */
class OwnedValue
{
    JSContext *ctx_;
    JSValue value_;

public:
    explicit OwnedValue(JSContext *ctx) noexcept
        : OwnedValue(ctx, JS_UNDEFINED)
    {
    }

    OwnedValue(JSContext *ctx, JSValue value) noexcept
        : ctx_(ctx), value_(value)
    {
    }

    ~OwnedValue()
    {
        JS_FreeValue(ctx_, value_);
    }

    OwnedValue(OwnedValue const&) = delete;
    OwnedValue& operator=(OwnedValue const&) = delete;

    OwnedValue(OwnedValue&& other) noexcept
        : ctx_(other.ctx_), value_(other.release())
    {
    }

    OwnedValue&
    operator=(OwnedValue&& other) noexcept
    {
        if (this != &other) {
            JS_FreeValue(ctx_, value_);
            ctx_ = other.ctx_;
            value_ = other.release();
        }
        return *this;
    }

    JSValueConst
    get() const noexcept
    {
        return value_;
    }

    bool
    isException() const noexcept
    {
        return JS_IsException(value_);
    }

    [[nodiscard]] JSValue
    release() noexcept
    {
        JSValue value = value_;
        value_ = JS_UNDEFINED;
        return value;
    }
};

template <class T>
T *
opaque(JSContext *ctx, JSValueConst value, JSClassID classId) noexcept
{
    return static_cast<T *>(JS_GetOpaque2(ctx, value, classId));
}

template <class T>
void
destroyOpaque(JSRuntime *rt, JSValueConst value, JSClassID classId) noexcept
{
    auto *native = static_cast<T *>(JS_GetOpaque(value, classId));
    if (native == nullptr)
        return;
    native->~T();
    js_free_rt(rt, native);
}

inline OwnedValue
property(JSContext *ctx, JSValueConst object, char const *name)
{
    return OwnedValue(ctx, JS_GetPropertyStr(ctx, object, name));
}

inline OwnedValue
element(JSContext *ctx, JSValueConst array, std::uint32_t index)
{
    return OwnedValue(ctx, JS_GetPropertyUint32(ctx, array, index));
}

[[nodiscard]] inline bool
installFunctions(
    JSContext *ctx,
    JSValueConst object,
    std::span<JSCFunctionListEntry const> functions)
{
    return JS_SetPropertyFunctionList(
        ctx,
        object,
        functions.data(),
        static_cast<int>(functions.size())) >= 0;
}

/** Apply JavaScript Object.freeze and preserve any pending exception. */
[[nodiscard]] bool
freezeObject(JSContext *ctx, JSValueConst value);

/** Copy bytes into a new JavaScript Uint8Array. */
JSValue
uint8Array(JSContext *ctx, std::span<std::uint8_t const> bytes);

/** Preserve a pending JavaScript exception, or create the contract TypeError. */
JSValue
pendingOrTypeError(JSContext *ctx, char const *message);

/** Sequential array construction with explicit ownership transfer to JS. */
class ArrayBuilder
{
    JSContext *ctx_;
    OwnedValue array_;
    std::uint32_t size_ = 0;

public:
    explicit ArrayBuilder(JSContext *ctx)
        : ctx_(ctx), array_(ctx, JS_NewArray(ctx))
    {
    }

    bool
    isException() const noexcept
    {
        return array_.isException();
    }

    std::uint32_t
    size() const noexcept
    {
        return size_;
    }

    bool
    append(OwnedValue value)
    {
        if (JS_SetPropertyUint32(
                ctx_, array_.get(), size_, value.release()) < 0)
            return false;
        ++size_;
        return true;
    }

    JSValue
    release() noexcept
    {
        return array_.release();
    }
};

}  // namespace jshookz::qjs
