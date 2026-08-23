#pragma once

#include "quickjs.hpp"

#include <new>
#include <span>
#include <utility>

namespace jshookz::provider::types {

template <class T, class... Args>
[[nodiscard]] inline JSValue
nativeNew(JSContext* ctx, JSClassID class_id, Args&&... args)
{
    void* storage = js_mallocz(ctx, sizeof(T));
    if (!storage)
        return JS_ThrowOutOfMemory(ctx);
    T* value = new (storage) T(std::forward<Args>(args)...);
    JSValue object = JS_NewObjectClass(ctx, class_id);
    if (JS_IsException(object)) {
        value->~T();
        js_free(ctx, storage);
        return object;
    }
    JS_SetOpaque(object, value);
    return object;
}

using FactoryInitializer = bool (*)(JSContext*, JSValueConst);

[[nodiscard]] bool
registerClass(
    JSContext* ctx,
    JSValueConst global,
    char const* name,
    JSClassID* class_id,
    JSClassDef const* class_def,
    std::span<JSCFunctionListEntry const> prototypeFunctions,
    std::span<JSCFunctionListEntry const> staticFunctions,
    jshookz::provider::qjs::ByteClassFamily byteFamily =
        jshookz::provider::qjs::ByteClassFamily::none,
    JSCFunction* toBytes = nullptr,
    FactoryInitializer initializeFactory = nullptr);

[[nodiscard]] bool registerSTBlob(JSContext* ctx, JSValueConst global);
[[nodiscard]] bool registerHash256(JSContext* ctx, JSValueConst global);
[[nodiscard]] bool registerAccountID(JSContext* ctx, JSValueConst global);
[[nodiscard]] bool registerXFL(JSContext* ctx, JSValueConst global);

// Provider-only materializer seams. These mint exact nominal values without
// routing through public JavaScript factories.
[[nodiscard]] JSValue makeUIntValue(
    JSContext* ctx, std::uint8_t bits, std::uint64_t value);
[[nodiscard]] JSValue makeSTBlobBytes(
    JSContext* ctx, std::uint8_t const* bytes, std::uint32_t length);
[[nodiscard]] JSValue makeSTBlobUninitialized(
    JSContext* ctx, std::uint32_t length, std::uint8_t** data);
[[nodiscard]] JSValue makeHash256Bytes(
    JSContext* ctx, std::uint8_t const* bytes, std::uint32_t length);
[[nodiscard]] JSValue makeAccountIDBytes(
    JSContext* ctx, std::uint8_t const* bytes, std::uint32_t length);

// Exact STBlob counterpart to JS_GetObjectByteSpanNoThrow. Success returns
// one owned duplicate of the STBlob wrapper. It allocates nothing, invokes no
// JavaScript, initializes every output, and preserves the pending exception.
[[nodiscard]] JSObjectByteSpanStatus getSTBlobByteSpanNoThrow(
    JSContext* ctx, JSValueConst input, JSValue* owned_backing,
    std::uint8_t const** data, std::size_t* size) noexcept;

}  // namespace jshookz::provider::types
