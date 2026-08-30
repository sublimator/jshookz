#pragma once

#include "quickjs.hpp"
#include "runtime_type.hpp"

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
    if (JS_PreventExtensions(ctx, object) < 0) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
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
    RuntimeTypeId runtimeType,
    jshookz::provider::qjs::ByteClassFamily byteFamily =
        jshookz::provider::qjs::ByteClassFamily::none,
    JSCFunction* toBytes = nullptr,
    FactoryInitializer initializeFactory = nullptr);

// Register an exact provider-minted class and its frozen prototype without
// creating a value-level global or public authoring factory.
[[nodiscard]] bool
registerHiddenClass(
    JSContext* ctx,
    JSClassID* class_id,
    JSClassDef const* class_def,
    std::span<JSCFunctionListEntry const> prototypeFunctions,
    jshookz::provider::qjs::ByteClassFamily byteFamily =
        jshookz::provider::qjs::ByteClassFamily::none,
    JSCFunction* toBytes = nullptr);

[[nodiscard]] bool registerSTBlob(JSContext* ctx, JSValueConst global);
[[nodiscard]] bool registerRecordSchemas(JSContext* ctx, JSValueConst global);
[[nodiscard]] bool registerHash256(JSContext* ctx, JSValueConst global);
[[nodiscard]] bool registerAccountID(JSContext* ctx, JSValueConst global);
[[nodiscard]] bool registerXFL(JSContext* ctx);
[[nodiscard]] bool publishXFLFactory(JSContext* ctx, JSValueConst global);

// Allocation-free exact nominal classifiers used by the one A-prime
// Symbol.hasInstance implementation.
[[nodiscard]] bool isSTBlob(JSValueConst value) noexcept;
[[nodiscard]] bool isHash256(JSValueConst value) noexcept;
[[nodiscard]] bool isAccountID(JSValueConst value) noexcept;
// expectedBits == 0 selects the UInt family root.
[[nodiscard]] bool isUInt(JSValueConst value,
                          std::uint8_t expectedBits) noexcept;
[[nodiscard]] bool isXFLDecimal(JSValueConst value) noexcept;

enum class UIntInputStatus
{
    valid,
    outOfRange,
    invalidType,
    exception,
};

// Provider-only lossless marshalling for raw unsigned Hook arguments.
[[nodiscard]] UIntInputStatus readUIntInput(
    JSContext* ctx,
    JSValueConst input,
    std::uint8_t bits,
    std::uint64_t& output);

// Provider-only materializer seams. These mint exact nominal values without
// routing through public JavaScript factories.
[[nodiscard]] JSValue makeUIntValue(JSContext *ctx, std::uint8_t bits,
                                    std::uint64_t value);
[[nodiscard]] JSValue makeSTBlobBytes(JSContext *ctx, std::uint8_t const *bytes,
                                      std::uint32_t length);
[[nodiscard]] JSValue makeSTBlobView(JSContext *ctx, JSValueConst owner,
                                     std::uint8_t const *bytes,
                                     std::uint32_t length);
[[nodiscard]] JSValue makeSTBlobUninitialized(JSContext *ctx,
                                              std::uint32_t length,
                                              std::uint8_t **data);
[[nodiscard]] JSValue makeHash256Bytes(JSContext *ctx,
                                       std::uint8_t const *bytes,
                                       std::uint32_t length);
[[nodiscard]] JSValue makeHash256View(JSContext *ctx, JSValueConst owner,
                                      std::uint8_t const *bytes,
                                      std::uint32_t length);
[[nodiscard]] JSValue makeAccountIDBytes(JSContext *ctx,
                                         std::uint8_t const *bytes,
                                         std::uint32_t length);
[[nodiscard]] JSValue makeAccountIDView(JSContext *ctx, JSValueConst owner,
                                        std::uint8_t const *bytes,
                                        std::uint32_t encodedLength);

// Provider-only nominal seams used by composite rich types. Readers perform
// no allocation or JavaScript operation, accept only the exact registered
// class/provenance, initialize their outputs, and do not throw on mismatch.
[[nodiscard]] bool readAccountIDBytes(
    JSContext* ctx,
    JSValueConst input,
    std::uint8_t output[20]) noexcept;
[[nodiscard]] JSValue makeXFLDecimalParts(
    JSContext* ctx,
    bool negative,
    std::uint64_t magnitude,
    std::int32_t exponent);
[[nodiscard]] bool readXFLDecimalParts(
    JSContext* ctx,
    JSValueConst input,
    bool* negative,
    std::uint64_t* magnitude,
    std::int32_t* exponent) noexcept;
[[nodiscard]] JSValue makeXFLDecimalRaw(JSContext* ctx, std::int64_t raw);
[[nodiscard]] bool isCanonicalXFLRaw(std::int64_t raw) noexcept;
[[nodiscard]] bool readXFLDecimalRaw(
    JSValueConst input, std::int64_t* raw) noexcept;

// Exact STBlob counterpart to JS_GetObjectByteSpanNoThrow. Success returns
// one owned duplicate of the STBlob wrapper. It allocates nothing, invokes no
// JavaScript, initializes every output, and preserves the pending exception.
[[nodiscard]] JSObjectByteSpanStatus getSTBlobByteSpanNoThrow(
    JSContext* ctx, JSValueConst input, JSValue* owned_backing,
    std::uint8_t const** data, std::size_t* size) noexcept;

}  // namespace jshookz::provider::types
