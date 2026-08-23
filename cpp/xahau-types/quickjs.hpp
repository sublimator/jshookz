#pragma once

#include <jshookz/qjs.hpp>

#include <cstdint>
#include <span>
#include <utility>

namespace jshookz::provider::qjs {

using jshookz::qjs::ArrayBuilder;
using jshookz::qjs::OwnedValue;
using jshookz::qjs::destroyOpaque;
using jshookz::qjs::element;
using jshookz::qjs::freezeObject;
using jshookz::qjs::installFunctions;
using jshookz::qjs::opaque;
using jshookz::qjs::pendingOrTypeError;
using jshookz::qjs::property;
using jshookz::qjs::uint8Array;
using jshookz::qjs::uint8ArrayUninitialized;

enum class BytePolicy : std::uint8_t
{
    bytesLike,
    hexString,
    bytesLikeOrSTBlob,
    lifecycleMessage,
    stateKeyLike,
    stateValueLike,
    traceLabel,
    traceValue,
    legacyHexInput,
};

enum class ByteClassFamily : std::uint8_t
{
    none,
    stBlob,
    serializedType,
};

/** Reset and populate the nominal byte-bearing class registry at startup. */
void
resetByteClassRegistry() noexcept;

[[nodiscard]] bool
registerByteClass(
    JSClassID classId,
    ByteClassFamily family,
    JSCFunction *toBytes) noexcept;

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
    bool parseArray(JSValueConst value);
    bool parseBinary(JSValueConst value);
    bool parseString(JSValueConst value, BytePolicy policy);
    bool parseRich(JSValueConst value, BytePolicy policy);

public:
    ~ByteView();

    ByteView(ByteView const&) = delete;
    ByteView& operator=(ByteView const&) = delete;
    ByteView(ByteView&& other) noexcept;
    ByteView& operator=(ByteView&& other) noexcept;

    [[nodiscard]] static ByteView
    get(
        JSContext *ctx,
        JSValueConst value,
        BytePolicy policy);

    /**
     * Attach the public binding coordinate to a byte-policy use. The string
     * and index are compile-time audit data; the inline runtime path remains
     * the single ByteView::get implementation.
     */
    template <std::size_t N>
    [[nodiscard]] static ByteView
    getBinding(
        JSContext *ctx,
        JSValueConst value,
        char const (&)[N],
        std::uint32_t,
        BytePolicy policy)
    {
        return get(ctx, value, policy);
    }

    /** Replace a borrowed view with an engine-owned stable byte snapshot. */
    [[nodiscard]] bool snapshot();

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

/** Preserve a pending exception, or describe the selected byte-input policy. */
JSValue
byteInputTypeError(
    JSContext *ctx,
    char const *operation,
    BytePolicy policy);

}  // namespace jshookz::provider::qjs
