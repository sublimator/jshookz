#include "quickjs.hpp"

#include <limits>

namespace jshookz::provider::qjs {
namespace {

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
ByteView::parseBinary(JSValueConst value)
{
    size_t offset = 0;
    size_t byteLength = 0;
    size_t bufferSize = 0;
    OwnedValue buffer(
        ctx_,
        JS_GetTypedArrayBuffer(
            ctx_, value, &offset, &byteLength, nullptr));
    if (!buffer.isException()) {
        auto *data = JS_GetArrayBuffer(ctx_, &bufferSize, buffer.get());
        if (data != nullptr &&
            offset <= bufferSize &&
            byteLength <= bufferSize - offset &&
            byteLength <= std::numeric_limits<std::uint32_t>::max()) {
            data_ = data + offset;
            size_ = static_cast<std::uint32_t>(byteLength);
            backing_ = std::move(buffer);
            valid_ = true;
            return true;
        }
    } else {
        OwnedValue exception(ctx_, JS_GetException(ctx_));
    }

    auto *data = JS_GetArrayBuffer(ctx_, &bufferSize, value);
    if (data == nullptr) {
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
ByteView::parseString(JSValueConst value, StringBytes strings)
{
    if (!JS_IsString(value))
        return false;

    size_t length = 0;
    char const *text = JS_ToCStringLen(ctx_, &length, value);
    if (text == nullptr)
        return false;
    if (length > std::numeric_limits<std::uint32_t>::max()) {
        JS_FreeCString(ctx_, text);
        return false;
    }

    if (strings == StringBytes::utf8) {
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
ByteView::parseRich(JSValueConst value)
{
    OwnedValue toBytes = property(ctx_, value, "toBytes");
    if (toBytes.isException() || !JS_IsFunction(ctx_, toBytes.get()))
        return false;
    OwnedValue bytes(
        ctx_, JS_Call(ctx_, toBytes.get(), value, 0, nullptr));
    if (bytes.isException())
        return false;
    /* Rich values normally return a Uint8Array. Preserve the earlier bridge's
       hex-string fallback for custom toBytes implementations. */
    return parseBinary(bytes.get()) ||
        parseString(bytes.get(), StringBytes::hex);
}

ByteView
ByteView::get(
    JSContext *ctx,
    JSValueConst value,
    StringBytes strings,
    RichBytes rich)
{
    ByteView result(ctx);
    if (result.parseBinary(value) || result.parseString(value, strings))
        return result;
    if (rich == RichBytes::callToBytes)
        result.parseRich(value);
    return result;
}

}  // namespace jshookz::provider::qjs
