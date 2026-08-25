#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <quickjs.h>

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

/** Non-throwing probe. Null if the value is not this class. */
template <class T>
T *
tryOpaque(JSValueConst value, JSClassID classId) noexcept
{
    return static_cast<T *>(JS_GetOpaque(value, classId));
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

/** Build a complete class prototype without publishing it to the context. */
[[nodiscard]] inline JSValue
makePrototype(
    JSContext *ctx,
    std::span<JSCFunctionListEntry const> functions,
    bool freeze = true)
{
    OwnedValue prototype(ctx, JS_NewObject(ctx));
    if (prototype.isException())
        return JS_EXCEPTION;
    if (!functions.empty() &&
        !installFunctions(ctx, prototype.get(), functions))
        return JS_EXCEPTION;
    if (freeze && !freezeObject(ctx, prototype.get()))
        return JS_EXCEPTION;
    return prototype.release();
}

/** Copy bytes into a new JavaScript Uint8Array. */
JSValue
uint8Array(JSContext *ctx, std::span<std::uint8_t const> bytes);

/** Allocate a fresh Uint8Array and expose its private writable backing until
 * the next JavaScript entry. The returned array owns that backing. */
JSValue
uint8ArrayUninitialized(
    JSContext *ctx, std::size_t size, std::uint8_t **data);

/** Preserve a pending JavaScript exception, or create the contract TypeError. */
JSValue
pendingOrTypeError(JSContext *ctx, char const *message);

[[nodiscard]] inline bool
defineClass(JSRuntime *rt, JSClassID *classId, JSClassDef const *def)
{
    if (rt == nullptr || classId == nullptr || def == nullptr)
        return false;
    if (*classId == 0)
        JS_NewClassID(classId);
    if (JS_IsRegisteredClass(rt, *classId))
        return true;
    return JS_NewClass(rt, *classId, def) >= 0;
}

[[nodiscard]] inline bool
installPrototype(
    JSContext *ctx,
    JSClassID classId,
    std::span<JSCFunctionListEntry const> functions,
    bool freeze = true)
{
    OwnedValue prototype(ctx, makePrototype(ctx, functions, freeze));
    if (prototype.isException())
        return false;
    JS_SetClassProto(ctx, classId, prototype.release());
    return true;
}

[[nodiscard]] inline bool
installFactory(
    JSContext *ctx,
    JSValueConst global,
    char const *name,
    std::span<JSCFunctionListEntry const> functions,
    bool freeze = true)
{
    if (name == nullptr || name[0] == '\0')
        return false;
    OwnedValue factory(ctx, JS_NewObject(ctx));
    if (factory.isException())
        return false;
    if (!functions.empty() &&
        !installFunctions(ctx, factory.get(), functions))
        return false;
    if (freeze && !freezeObject(ctx, factory.get()))
        return false;
    return JS_SetPropertyStr(ctx, global, name, factory.release()) >= 0;
}

enum class HexCase : std::uint8_t
{
    Lower,
    Upper,
};

inline int
hexNibble(char value) noexcept
{
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

inline bool
hexDecode(std::string_view hex, std::vector<std::uint8_t> &out)
{
    if (hex.size() % 2 != 0)
        return false;
    out.resize(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        int const hi = hexNibble(hex[i]);
        int const lo = hexNibble(hex[i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out[i / 2] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return true;
}

inline std::string
hexEncode(std::span<std::uint8_t const> bytes, HexCase hexCase)
{
    char const *digits =
        hexCase == HexCase::Upper ? "0123456789ABCDEF" : "0123456789abcdef";
    std::string out;
    out.resize(bytes.size() * 2);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        out[i * 2] = digits[bytes[i] >> 4];
        out[i * 2 + 1] = digits[bytes[i] & 0x0f];
    }
    return out;
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

}  // namespace jshookz::qjs
