#include "quickjs.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace jshookz::provider::qjs {
namespace {

enum class StringMode : std::uint8_t
{
    reject,
    hex,
    utf8,
};

enum class RichMode : std::uint8_t
{
    reject,
    stBlob,
    serializedType,
};

struct BytePolicySpec
{
    StringMode strings;
    RichMode rich;
    char const *expected;
};

constexpr BytePolicySpec
policySpec(BytePolicy policy) noexcept
{
    switch (policy) {
        case BytePolicy::bytesLike:
            return {
                StringMode::reject,
                RichMode::reject,
                "Uint8Array, ArrayBuffer, or 0..255 byte array"};
        case BytePolicy::hexString:
        case BytePolicy::legacyHexInput:
            return {
                StringMode::hex,
                RichMode::reject,
                "Uint8Array, ArrayBuffer, 0..255 byte array, or hex string"};
        case BytePolicy::bytesLikeOrSTBlob:
            return {
                StringMode::reject,
                RichMode::stBlob,
                "BytesLike or STBlob"};
        case BytePolicy::lifecycleMessage:
            return {
                StringMode::utf8,
                RichMode::stBlob,
                "string, BytesLike, or STBlob"};
        case BytePolicy::stateKeyLike:
            return {
                StringMode::utf8,
                RichMode::serializedType,
                "StateKeyLike"};
        case BytePolicy::stateValueLike:
            return {
                StringMode::utf8,
                RichMode::serializedType,
                "StateValueLike"};
        case BytePolicy::traceLabel:
            return {StringMode::utf8, RichMode::reject, "string"};
        case BytePolicy::traceValue:
            return {
                StringMode::reject,
                RichMode::serializedType,
                "byte-bearing provider value"};
    }
    return {StringMode::reject, RichMode::reject, "byte input"};
}

struct ByteClassEntry
{
    JSClassID classId = JS_INVALID_CLASS_ID;
    ByteClassFamily family = ByteClassFamily::serializedType;
    JSCFunction *toBytes = nullptr;
};

std::array<ByteClassEntry, 32> byteClasses{};
std::size_t byteClassCount = 0;

ByteClassEntry const *
richClassEntry(JSValueConst value, RichMode mode) noexcept
{
    if (mode == RichMode::reject || !JS_IsObject(value))
        return nullptr;
    JSClassID const classId = JS_GetClassID(value);
    for (std::size_t index = 0; index < byteClassCount; ++index) {
        auto const &entry = byteClasses[index];
        if (entry.classId != classId)
            continue;
        if (mode == RichMode::serializedType ||
            entry.family == ByteClassFamily::stBlob)
            return &entry;
        return nullptr;
    }
    return nullptr;
}

int
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

}  // namespace

void
resetByteClassRegistry() noexcept
{
    byteClassCount = 0;
}

bool
registerByteClass(
    JSClassID classId,
    ByteClassFamily family,
    JSCFunction *toBytes) noexcept
{
    if (family == ByteClassFamily::none)
        return true;
    if (classId == JS_INVALID_CLASS_ID ||
        toBytes == nullptr ||
        byteClassCount == byteClasses.size())
        return false;
    byteClasses[byteClassCount++] = {classId, family, toBytes};
    return true;
}

ByteView::ByteView(JSContext *ctx) noexcept
    : ctx_(ctx), backing_(ctx)
{
}

ByteView::~ByteView()
{
    clear();
}

ByteView::ByteView(ByteView&& other) noexcept
    : ctx_(other.ctx_)
    , data_(other.data_)
    , size_(other.size_)
    , allocated_(other.allocated_)
    , string_(other.string_)
    , backing_(std::move(other.backing_))
    , valid_(other.valid_)
{
    other.data_ = nullptr;
    other.size_ = 0;
    other.allocated_ = nullptr;
    other.string_ = nullptr;
    other.valid_ = false;
}

ByteView&
ByteView::operator=(ByteView&& other) noexcept
{
    if (this == &other)
        return *this;

    clear();
    ctx_ = other.ctx_;
    data_ = other.data_;
    size_ = other.size_;
    allocated_ = other.allocated_;
    string_ = other.string_;
    backing_ = std::move(other.backing_);
    valid_ = other.valid_;

    other.data_ = nullptr;
    other.size_ = 0;
    other.allocated_ = nullptr;
    other.string_ = nullptr;
    other.valid_ = false;
    return *this;
}

void
ByteView::clear() noexcept
{
    if (allocated_ != nullptr)
        js_free(ctx_, allocated_);
    if (string_ != nullptr)
        JS_FreeCString(ctx_, string_);
    allocated_ = nullptr;
    string_ = nullptr;
    data_ = nullptr;
    size_ = 0;
    valid_ = false;
    backing_ = OwnedValue(ctx_);
}

bool
ByteView::parseArray(JSValueConst value)
{
    OwnedValue lengthValue = property(ctx_, value, "length");
    if (lengthValue.isException())
        return false;
    std::uint32_t length = 0;
    if (JS_ToUint32(ctx_, &length, lengthValue.get()) < 0)
        return false;
    if (length != 0) {
        allocated_ = static_cast<std::uint8_t *>(js_malloc(ctx_, length));
        if (allocated_ == nullptr)
            return false;
    }
    for (std::uint32_t index = 0; index < length; ++index) {
        OwnedValue valueAtIndex = element(ctx_, value, index);
        double number = 0;
        if (valueAtIndex.isException())
            return false;
        if (!JS_IsNumber(valueAtIndex.get()) ||
            JS_ToFloat64(ctx_, &number, valueAtIndex.get()) < 0)
            return false;
        if (!std::isfinite(number) || std::trunc(number) != number ||
            number < 0 || number > 255)
            return false;
        allocated_[index] = static_cast<std::uint8_t>(number);
    }
    data_ = allocated_;
    size_ = length;
    valid_ = true;
    return true;
}

bool
ByteView::parseBinary(JSValueConst value)
{
    size_t offset = 0;
    size_t byteLength = 0;
    size_t bufferSize = 0;
    int const typedArrayType = JS_GetTypedArrayType(value);
    if (typedArrayType == JS_TYPED_ARRAY_UINT8 ||
        typedArrayType == JS_TYPED_ARRAY_UINT8C) {
        OwnedValue buffer(
            ctx_,
            JS_GetTypedArrayBuffer(
                ctx_, value, &offset, &byteLength, nullptr));
        if (buffer.isException())
            return false;
        auto *data = JS_GetArrayBuffer(ctx_, &bufferSize, buffer.get());
        if ((data != nullptr || !JS_HasException(ctx_)) &&
            offset <= bufferSize &&
            byteLength <= bufferSize - offset &&
            byteLength <= std::numeric_limits<std::uint32_t>::max()) {
            data_ = data + offset;
            size_ = static_cast<std::uint32_t>(byteLength);
            backing_ = std::move(buffer);
            valid_ = true;
            return true;
        }
        return false;
    }

    auto *data = JS_GetArrayBuffer(ctx_, &bufferSize, value);
    if (data == nullptr && JS_HasException(ctx_)) {
        OwnedValue exception(ctx_, JS_GetException(ctx_));
        return false;
    }
    if (bufferSize > std::numeric_limits<std::uint32_t>::max())
        return false;
    data_ = data;
    size_ = static_cast<std::uint32_t>(bufferSize);
    backing_ = OwnedValue(ctx_, JS_DupValue(ctx_, value));
    valid_ = true;
    return true;
}

bool
ByteView::parseString(JSValueConst value, BytePolicy policy)
{
    StringMode const strings = policySpec(policy).strings;
    if (!JS_IsString(value))
        return false;
    if (strings == StringMode::reject)
        return false;

    size_t length = 0;
    char const *text = JS_ToCStringLen(ctx_, &length, value);
    if (text == nullptr)
        return false;
    if (length > std::numeric_limits<std::uint32_t>::max()) {
        JS_FreeCString(ctx_, text);
        return false;
    }

    if (strings == StringMode::utf8) {
        string_ = text;
        data_ = reinterpret_cast<std::uint8_t const *>(text);
        size_ = static_cast<std::uint32_t>(length);
        valid_ = true;
        return true;
    }

    if ((length & 1U) != 0) {
        JS_FreeCString(ctx_, text);
        return false;
    }

    auto const outputLength = static_cast<std::uint32_t>(length / 2);
    if (outputLength != 0) {
        allocated_ = static_cast<std::uint8_t *>(
            js_malloc(ctx_, outputLength));
        if (allocated_ == nullptr) {
            JS_FreeCString(ctx_, text);
            return false;
        }
    }
    for (std::uint32_t index = 0; index < outputLength; ++index) {
        int const high = hexNibble(text[index * 2]);
        int const low = hexNibble(text[index * 2 + 1]);
        if (high < 0 || low < 0) {
            JS_FreeCString(ctx_, text);
            js_free(ctx_, allocated_);
            allocated_ = nullptr;
            return false;
        }
        allocated_[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    JS_FreeCString(ctx_, text);
    data_ = allocated_;
    size_ = outputLength;
    valid_ = true;
    return true;
}

bool
ByteView::parseRich(JSValueConst value, BytePolicy policy)
{
    auto const *entry = richClassEntry(value, policySpec(policy).rich);
    if (entry == nullptr)
        return false;
    OwnedValue bytes(
        ctx_, entry->toBytes(ctx_, value, 0, nullptr));
    if (bytes.isException())
        return false;
    return parseBinary(bytes.get());
}

ByteView
ByteView::get(
    JSContext *ctx,
    JSValueConst value,
    BytePolicy policy)
{
    ByteView result(ctx);
    /* Strings dominate Hook keys and messages. Avoid using two thrown and
       cleared invalid-class exceptions as routine type probes for them. */
    if (JS_IsString(value)) {
        result.parseString(value, policy);
        return result;
    }
    int const isArray = JS_IsArray(ctx, value);
    if (isArray < 0)
        return result;
    if (isArray) {
        result.parseArray(value);
        return result;
    }
    if (result.parseBinary(value))
        return result;
    if (JS_HasException(ctx))
        return result;
    result.parseRich(value, policy);
    return result;
}

bool
ByteView::snapshot()
{
    if (!valid_)
        return false;
    /* Hex decoding and UTF-8 conversion already own engine-independent
       storage. Only borrowed ArrayBuffer storage needs a defensive copy. */
    if (size_ == 0 || allocated_ != nullptr || string_ != nullptr)
        return true;

    auto *copy = static_cast<std::uint8_t *>(js_malloc(ctx_, size_));
    if (copy == nullptr)
        return false;
    std::memcpy(copy, data_, size_);
    auto const copiedSize = size_;
    clear();
    allocated_ = copy;
    data_ = copy;
    size_ = copiedSize;
    valid_ = true;
    return true;
}

JSValue
byteInputTypeError(
    JSContext *ctx,
    char const *operation,
    BytePolicy policy)
{
    if (JS_HasException(ctx))
        return JS_EXCEPTION;
    return JS_ThrowTypeError(
        ctx, "%s: expected %s", operation, policySpec(policy).expected);
}

}  // namespace jshookz::provider::qjs
