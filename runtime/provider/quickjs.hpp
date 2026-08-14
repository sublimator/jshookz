#pragma once

#include <cstdint>
#include <utility>

extern "C" {
#include "../../engine/quickjs/quickjs.h"
}

namespace jshookz::provider::qjs {

/** One owned QuickJS value tied to the callback-local JSContext lifetime. */
class OwnedValue
{
    JSContext *ctx_;
    JSValue value_;

public:
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

    JSValue
    release() noexcept
    {
        JSValue value = value_;
        value_ = JS_UNDEFINED;
        return value;
    }
};

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

}  // namespace jshookz::provider::qjs
