#pragma once

#include <cstdint>
#include <span>
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

    JSValue
    release() noexcept
    {
        JSValue value = value_;
        value_ = JS_UNDEFINED;
        return value;
    }
};

enum class StringBytes : std::uint8_t
{
    hex,
    utf8,
};

enum class RichBytes : std::uint8_t
{
    reject,
    callToBytes,
};

/**
 * Borrowed bytes whose QuickJS backing values and temporary conversions stay
 * alive for the view's C++ lifetime. A later JavaScript call can still detach
 * or resize an ArrayBuffer; snapshot() before executing JavaScript while the
 * bytes must remain stable.
 */
class ByteView
{
    JSContext *ctx_;
    std::uint8_t const *data_ = nullptr;
    std::uint32_t size_ = 0;
    std::uint8_t *allocated_ = nullptr;
    char const *string_ = nullptr;
    OwnedValue backing_;
    bool valid_ = false;

    explicit ByteView(JSContext *ctx) noexcept;
    void clear() noexcept;
    bool parseBinary(JSValueConst value);
    bool parseString(JSValueConst value, StringBytes strings);
    bool parseRich(JSValueConst value);

public:
    ~ByteView();

    ByteView(ByteView const&) = delete;
    ByteView& operator=(ByteView const&) = delete;
    ByteView(ByteView&& other) noexcept;
    ByteView& operator=(ByteView&& other) noexcept;

    static ByteView
    get(
        JSContext *ctx,
        JSValueConst value,
        StringBytes strings,
        RichBytes rich = RichBytes::reject);

    /** Replace a borrowed view with an engine-owned stable byte snapshot. */
    bool snapshot();

    explicit operator bool() const noexcept
    {
        return valid_;
    }

    std::uint8_t const *
    data() const noexcept
    {
        return data_;
    }

    std::uint32_t
    size() const noexcept
    {
        return size_;
    }

    std::span<std::uint8_t const>
    bytes() const noexcept
    {
        return {data_, size_};
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

/** Copy bytes into a new JavaScript Uint8Array. */
JSValue
uint8Array(JSContext *ctx, std::span<std::uint8_t const> bytes);

/** Preserve a pending JavaScript exception, or create the contract TypeError. */
JSValue
pendingOrTypeError(JSContext *ctx, char const *message);

/** Preserve a pending exception, or describe the selected byte-input policy. */
JSValue
byteInputTypeError(
    JSContext *ctx,
    char const *operation,
    StringBytes strings);

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
