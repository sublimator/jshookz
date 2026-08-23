#include "quickjs.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

namespace {

struct alignas(std::max_align_t) AllocationHeader {
    std::size_t size;
};

struct AllocatorControl {
    std::size_t requests = 0;
    std::size_t reject_at = 0;
    std::size_t rejections = 0;
    std::size_t live_blocks = 0;

    void reject_relative(std::size_t ordinal)
    {
        reject_at = requests + ordinal;
    }
};

bool allocator_would_exceed(JSMallocState const *state, std::size_t old_size,
                            std::size_t new_size)
{
    if (state->malloc_size < old_size)
        return true;
    std::size_t const retained = state->malloc_size - old_size;
    return new_size > state->malloc_limit ||
           retained > state->malloc_limit - new_size;
}

bool reject_request(JSMallocState *state)
{
    auto *control = static_cast<AllocatorControl *>(state->opaque);
    ++control->requests;
    if (control->reject_at != 0 && control->requests == control->reject_at) {
        control->reject_at = 0;
        ++control->rejections;
        return true;
    }
    return false;
}

void *test_malloc(JSMallocState *state, std::size_t size)
{
    auto *control = static_cast<AllocatorControl *>(state->opaque);
    if (reject_request(state) || size == 0 ||
        size > std::numeric_limits<std::size_t>::max() -
                   sizeof(AllocationHeader) ||
        allocator_would_exceed(state, 0, size)) {
        return nullptr;
    }
    auto *header = static_cast<AllocationHeader *>(
        std::malloc(sizeof(AllocationHeader) + size));
    if (header == nullptr)
        return nullptr;
    header->size = size;
    ++state->malloc_count;
    state->malloc_size += size;
    ++control->live_blocks;
    return header + 1;
}

void test_free(JSMallocState *state, void *pointer)
{
    if (pointer == nullptr)
        return;
    auto *control = static_cast<AllocatorControl *>(state->opaque);
    auto *header = static_cast<AllocationHeader *>(pointer) - 1;
    --state->malloc_count;
    state->malloc_size -= header->size;
    --control->live_blocks;
    std::free(header);
}

void *test_realloc(JSMallocState *state, void *pointer, std::size_t size)
{
    if (pointer == nullptr)
        return size == 0 ? nullptr : test_malloc(state, size);
    if (size == 0) {
        test_free(state, pointer);
        return nullptr;
    }
    if (reject_request(state))
        return nullptr;

    auto *header = static_cast<AllocationHeader *>(pointer) - 1;
    std::size_t const old_size = header->size;
    if (size > std::numeric_limits<std::size_t>::max() -
                   sizeof(AllocationHeader) ||
        allocator_would_exceed(state, old_size, size)) {
        return nullptr;
    }
    auto *replacement = static_cast<AllocationHeader *>(
        std::realloc(header, sizeof(AllocationHeader) + size));
    if (replacement == nullptr)
        return nullptr;
    replacement->size = size;
    state->malloc_size = state->malloc_size - old_size + size;
    return replacement + 1;
}

std::size_t test_malloc_usable_size(void const *pointer)
{
    if (pointer == nullptr)
        return 0;
    return (static_cast<AllocationHeader const *>(pointer) - 1)->size;
}

JSMallocFunctions const test_allocator = {
    .js_malloc = test_malloc,
    .js_free = test_free,
    .js_realloc = test_realloc,
    .js_malloc_usable_size = test_malloc_usable_size,
};

class RuntimeFixture {
public:
    RuntimeFixture()
    {
        runtime_ = JS_NewRuntime2(&test_allocator, &allocator_);
        if (runtime_ != nullptr)
            context_ = JS_NewContext(runtime_);
    }

    ~RuntimeFixture() { close(); }

    RuntimeFixture(RuntimeFixture const &) = delete;
    RuntimeFixture &operator=(RuntimeFixture const &) = delete;

    bool ready() const { return runtime_ != nullptr && context_ != nullptr; }
    JSContext *context() const { return context_; }
    AllocatorControl &allocator() { return allocator_; }

    void close()
    {
        if (context_ != nullptr) {
            JS_FreeContext(context_);
            context_ = nullptr;
        }
        if (runtime_ != nullptr) {
            JS_FreeRuntime(runtime_);
            runtime_ = nullptr;
        }
    }

private:
    AllocatorControl allocator_;
    JSRuntime *runtime_ = nullptr;
    JSContext *context_ = nullptr;
};

class LocalValue {
public:
    explicit LocalValue(JSContext *context,
                        JSValue value = JS_UNDEFINED)
        : context_(context), value_(value)
    {
    }

    ~LocalValue() { JS_FreeValue(context_, value_); }

    LocalValue(LocalValue const &) = delete;
    LocalValue &operator=(LocalValue const &) = delete;

    JSValueConst get() const { return value_; }
    bool is_exception() const { return JS_IsException(value_); }

    JSValue release()
    {
        JSValue value = value_;
        value_ = JS_UNDEFINED;
        return value;
    }

private:
    JSContext *context_;
    JSValue value_;
};

JSValue eval(JSContext *context, char const *source)
{
    return JS_Eval(context, source, std::strlen(source), "<helper-test>",
                   JS_EVAL_TYPE_GLOBAL);
}

std::string string_property(JSContext *context, JSValueConst object,
                            char const *name)
{
    LocalValue value(context, JS_GetPropertyStr(context, object, name));
    if (value.is_exception())
        return {};
    char const *text = JS_ToCString(context, value.get());
    if (text == nullptr)
        return {};
    std::string result(text);
    JS_FreeCString(context, text);
    return result;
}

void expect_no_owned_span(JSValueConst owned, std::uint8_t const *data,
                          std::size_t size)
{
    EXPECT_TRUE(JS_IsUndefined(owned));
    EXPECT_EQ(data, nullptr);
    EXPECT_EQ(size, 0u);
}

void expect_bare_type_error(JSContext *context, JSValueConst error)
{
    ASSERT_TRUE(JS_IsError(context, error));
    ASSERT_FALSE(JS_HasException(context));

    LocalValue global(context, JS_GetGlobalObject(context));
    ASSERT_FALSE(global.is_exception());
    LocalValue constructor(
        context, JS_GetPropertyStr(context, global.get(), "TypeError"));
    ASSERT_FALSE(constructor.is_exception());
    LocalValue expected_prototype(
        context, JS_GetPropertyStr(context, constructor.get(), "prototype"));
    ASSERT_FALSE(expected_prototype.is_exception());
    LocalValue actual_prototype(context, JS_GetPrototype(context, error));
    ASSERT_FALSE(actual_prototype.is_exception());

    EXPECT_EQ(JS_StrictEq(context, expected_prototype.get(),
                          actual_prototype.get()),
              1);
    EXPECT_EQ(JS_IsInstanceOf(context, error, constructor.get()), 1);

    JSPropertyEnum *properties = nullptr;
    std::uint32_t property_count = 99;
    ASSERT_EQ(JS_GetOwnPropertyNames(
                  context, &properties, &property_count, error,
                  JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK |
                      JS_GPN_PRIVATE_MASK),
              0);
    EXPECT_EQ(property_count, 0u);
    JS_FreePropertyEnum(context, properties, property_count);
    EXPECT_FALSE(JS_HasException(context));
}

JSValue make_typed_array(JSContext *context, JSValueConst buffer,
                         std::uint32_t offset, std::uint32_t length,
                         JSTypedArrayEnum type = JS_TYPED_ARRAY_UINT8)
{
    std::array<JSValue, 3> arguments{
        JS_DupValue(context, buffer), JS_NewUint32(context, offset),
        JS_NewUint32(context, length)};
    JSValue result = JS_NewTypedArray(context, arguments.size(),
                                      arguments.data(), type);
    for (JSValue argument : arguments)
        JS_FreeValue(context, argument);
    return result;
}

void expect_status_without_exception(JSContext *context, JSValueConst input,
                                     JSObjectByteSpanStatus expected)
{
    JSValue owned = JS_NULL;
    auto const *data = reinterpret_cast<std::uint8_t const *>(1);
    std::size_t size = 99;
    EXPECT_EQ(JS_GetObjectByteSpanNoThrow(context, input, &owned, &data, &size),
              expected);
    EXPECT_FALSE(JS_HasException(context));
    if (expected != JS_OBJECT_BYTES_OK)
        expect_no_owned_span(owned, data, size);
    JS_FreeValue(context, owned);
}

TEST(QuickJSObjectByteSpan, ExactArrayBufferReturnsOwnedInputAndExactSpan)
{
    RuntimeFixture fixture;
    ASSERT_TRUE(fixture.ready());
    JSContext *context = fixture.context();
    std::array<std::uint8_t, 4> const bytes{0x10, 0x20, 0x30, 0x40};
    LocalValue buffer(
        context, JS_NewArrayBufferCopy(context, bytes.data(), bytes.size()));
    ASSERT_FALSE(buffer.is_exception());

    JSValue owned = JS_UNDEFINED;
    std::uint8_t const *data = nullptr;
    std::size_t size = 0;
    ASSERT_EQ(JS_GetObjectByteSpanNoThrow(context, buffer.get(), &owned, &data,
                                         &size),
              JS_OBJECT_BYTES_OK);
    LocalValue backing(context, owned);
    EXPECT_EQ(JS_StrictEq(context, backing.get(), buffer.get()), 1);
    ASSERT_EQ(size, bytes.size());
    ASSERT_NE(data, nullptr);
    EXPECT_TRUE(std::equal(bytes.begin(), bytes.end(), data));
    EXPECT_FALSE(JS_HasException(context));
}

TEST(QuickJSObjectByteSpan, ExactUint8ArrayReturnsOwnedBackingAndViewSpan)
{
    RuntimeFixture fixture;
    ASSERT_TRUE(fixture.ready());
    JSContext *context = fixture.context();
    std::array<std::uint8_t, 5> const bytes{1, 2, 3, 4, 5};
    LocalValue buffer(
        context, JS_NewArrayBufferCopy(context, bytes.data(), bytes.size()));
    ASSERT_FALSE(buffer.is_exception());
    LocalValue view(context, make_typed_array(context, buffer.get(), 1, 3));
    ASSERT_FALSE(view.is_exception());

    JSValue owned = JS_UNDEFINED;
    std::uint8_t const *data = nullptr;
    std::size_t size = 0;
    ASSERT_EQ(JS_GetObjectByteSpanNoThrow(context, view.get(), &owned, &data,
                                         &size),
              JS_OBJECT_BYTES_OK);
    LocalValue backing(context, owned);
    EXPECT_EQ(JS_StrictEq(context, backing.get(), buffer.get()), 1);
    EXPECT_EQ(JS_StrictEq(context, backing.get(), view.get()), 0);
    ASSERT_EQ(size, 3u);
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data[0], 2u);
    EXPECT_EQ(data[1], 3u);
    EXPECT_EQ(data[2], 4u);
    EXPECT_FALSE(JS_HasException(context));
}

struct ExternalBufferProbe {
    std::size_t frees = 0;
};

void free_external_buffer(JSRuntime *, void *opaque, void *pointer)
{
    auto *probe = static_cast<ExternalBufferProbe *>(opaque);
    ++probe->frees;
    std::free(pointer);
}

TEST(QuickJSObjectByteSpan, OwnedBackingKeepsArrayBufferStorageAlive)
{
    RuntimeFixture fixture;
    ASSERT_TRUE(fixture.ready());
    JSContext *context = fixture.context();
    ExternalBufferProbe probe;
    auto *bytes = static_cast<std::uint8_t *>(std::malloc(3));
    ASSERT_NE(bytes, nullptr);
    bytes[0] = 0x21;
    bytes[1] = 0x43;
    bytes[2] = 0x65;

    JSValue buffer = JS_NewArrayBuffer(context, bytes, 3, free_external_buffer,
                                       &probe, false);
    ASSERT_FALSE(JS_IsException(buffer));
    JSValue owned = JS_UNDEFINED;
    std::uint8_t const *data = nullptr;
    std::size_t size = 0;
    ASSERT_EQ(JS_GetObjectByteSpanNoThrow(context, buffer, &owned, &data, &size),
              JS_OBJECT_BYTES_OK);
    JS_FreeValue(context, buffer);
    EXPECT_EQ(probe.frees, 0u);
    ASSERT_EQ(size, 3u);
    EXPECT_EQ(data[0], 0x21u);
    EXPECT_EQ(data[1], 0x43u);
    EXPECT_EQ(data[2], 0x65u);

    JS_FreeValue(context, owned);
    EXPECT_EQ(probe.frees, 1u);
}

TEST(QuickJSObjectByteSpan, OwnedBackingKeepsUint8ArrayStorageAlive)
{
    RuntimeFixture fixture;
    ASSERT_TRUE(fixture.ready());
    JSContext *context = fixture.context();
    ExternalBufferProbe probe;
    auto *bytes = static_cast<std::uint8_t *>(std::malloc(4));
    ASSERT_NE(bytes, nullptr);
    bytes[0] = 0xaa;
    bytes[1] = 0xbb;
    bytes[2] = 0xcc;
    bytes[3] = 0xdd;

    JSValue buffer = JS_NewArrayBuffer(context, bytes, 4, free_external_buffer,
                                       &probe, false);
    ASSERT_FALSE(JS_IsException(buffer));
    JSValue view = make_typed_array(context, buffer, 1, 2);
    ASSERT_FALSE(JS_IsException(view));
    JS_FreeValue(context, buffer);

    JSValue owned = JS_UNDEFINED;
    std::uint8_t const *data = nullptr;
    std::size_t size = 0;
    ASSERT_EQ(JS_GetObjectByteSpanNoThrow(context, view, &owned, &data, &size),
              JS_OBJECT_BYTES_OK);
    JS_FreeValue(context, view);
    EXPECT_EQ(probe.frees, 0u);
    ASSERT_EQ(size, 2u);
    EXPECT_EQ(data[0], 0xbbu);
    EXPECT_EQ(data[1], 0xccu);

    JS_FreeValue(context, owned);
    EXPECT_EQ(probe.frees, 1u);
}

TEST(QuickJSObjectByteSpan, EmptyArrayBufferAndUint8ArrayAreUsable)
{
    RuntimeFixture fixture;
    ASSERT_TRUE(fixture.ready());
    JSContext *context = fixture.context();
    LocalValue buffer(context,
                      JS_NewArrayBuffer(context, nullptr, 0, nullptr, nullptr,
                                        false));
    ASSERT_FALSE(buffer.is_exception());

    JSValue owned = JS_NULL;
    auto const *data = reinterpret_cast<std::uint8_t const *>(1);
    std::size_t size = 99;
    ASSERT_EQ(JS_GetObjectByteSpanNoThrow(context, buffer.get(), &owned, &data,
                                         &size),
              JS_OBJECT_BYTES_OK);
    EXPECT_TRUE(JS_IsObject(owned));
    EXPECT_EQ(data, nullptr);
    EXPECT_EQ(size, 0u);
    JS_FreeValue(context, owned);

    LocalValue view(context, make_typed_array(context, buffer.get(), 0, 0));
    ASSERT_FALSE(view.is_exception());
    owned = JS_NULL;
    data = reinterpret_cast<std::uint8_t const *>(1);
    size = 99;
    ASSERT_EQ(JS_GetObjectByteSpanNoThrow(context, view.get(), &owned, &data,
                                         &size),
              JS_OBJECT_BYTES_OK);
    EXPECT_TRUE(JS_IsObject(owned));
    EXPECT_EQ(data, nullptr);
    EXPECT_EQ(size, 0u);
    JS_FreeValue(context, owned);
}

TEST(QuickJSObjectByteSpan, RejectsEveryOtherTypedArrayAndOrdinaryKinds)
{
    RuntimeFixture fixture;
    ASSERT_TRUE(fixture.ready());
    JSContext *context = fixture.context();

    for (int type = JS_TYPED_ARRAY_UINT8C; type <= JS_TYPED_ARRAY_FLOAT64;
         ++type) {
        if (type == JS_TYPED_ARRAY_UINT8)
            continue;
        JSValue length = JS_NewInt32(context, 4);
        LocalValue value(
            context, JS_NewTypedArray(context, 1, &length,
                                      static_cast<JSTypedArrayEnum>(type)));
        ASSERT_FALSE(value.is_exception()) << type;
        expect_status_without_exception(context, value.get(),
                                        JS_OBJECT_BYTES_WRONG_KIND);
    }

    LocalValue data_view(context,
                         eval(context, "new DataView(new ArrayBuffer(4))"));
    ASSERT_FALSE(data_view.is_exception());
    expect_status_without_exception(context, data_view.get(),
                                    JS_OBJECT_BYTES_WRONG_KIND);

    LocalValue array(context, JS_NewArray(context));
    LocalValue object(context, JS_NewObject(context));
    ASSERT_FALSE(array.is_exception());
    ASSERT_FALSE(object.is_exception());
    expect_status_without_exception(context, array.get(),
                                    JS_OBJECT_BYTES_WRONG_KIND);
    expect_status_without_exception(context, object.get(),
                                    JS_OBJECT_BYTES_WRONG_KIND);
    expect_status_without_exception(context, JS_NewInt32(context, 1),
                                    JS_OBJECT_BYTES_WRONG_KIND);
    expect_status_without_exception(context, JS_NULL,
                                    JS_OBJECT_BYTES_WRONG_KIND);
}

TEST(QuickJSObjectByteSpan, RejectsSharedBackingAndDoesNotRunProxyTraps)
{
    RuntimeFixture fixture;
    ASSERT_TRUE(fixture.ready());
    JSContext *context = fixture.context();

    LocalValue shared(context, eval(context, "new SharedArrayBuffer(4)"));
    ASSERT_FALSE(shared.is_exception());
    expect_status_without_exception(context, shared.get(),
                                    JS_OBJECT_BYTES_WRONG_KIND);

    LocalValue shared_view(
        context,
        eval(context, "new Uint8Array(new SharedArrayBuffer(4))"));
    ASSERT_FALSE(shared_view.is_exception());
    expect_status_without_exception(context, shared_view.get(),
                                    JS_OBJECT_BYTES_WRONG_KIND);

    LocalValue proxy(context, eval(context, R"JS(
        globalThis.__objectByteSpanTrapCount = 0;
        new Proxy(new Uint8Array(4), {
          get(target, key, receiver) {
            ++globalThis.__objectByteSpanTrapCount;
            return Reflect.get(target, key, receiver);
          }
        });
    )JS"));
    ASSERT_FALSE(proxy.is_exception());
    expect_status_without_exception(context, proxy.get(),
                                    JS_OBJECT_BYTES_WRONG_KIND);
    LocalValue global(context, JS_GetGlobalObject(context));
    LocalValue trap_count(
        context,
        JS_GetPropertyStr(context, global.get(), "__objectByteSpanTrapCount"));
    std::int32_t count = -1;
    ASSERT_EQ(JS_ToInt32(context, &count, trap_count.get()), 0);
    EXPECT_EQ(count, 0);
}

TEST(QuickJSObjectByteSpan, DetachedAndOutOfBoundsBackingIsUnusable)
{
    RuntimeFixture fixture;
    ASSERT_TRUE(fixture.ready());
    JSContext *context = fixture.context();
    std::array<std::uint8_t, 4> const bytes{1, 2, 3, 4};

    LocalValue detached_buffer(
        context, JS_NewArrayBufferCopy(context, bytes.data(), bytes.size()));
    ASSERT_FALSE(detached_buffer.is_exception());
    JS_DetachArrayBuffer(context, detached_buffer.get());
    expect_status_without_exception(context, detached_buffer.get(),
                                    JS_OBJECT_BYTES_UNUSABLE);

    LocalValue backing(
        context, JS_NewArrayBufferCopy(context, bytes.data(), bytes.size()));
    ASSERT_FALSE(backing.is_exception());
    LocalValue detached_view(
        context, make_typed_array(context, backing.get(), 0, bytes.size()));
    ASSERT_FALSE(detached_view.is_exception());
    JS_DetachArrayBuffer(context, backing.get());
    expect_status_without_exception(context, detached_view.get(),
                                    JS_OBJECT_BYTES_UNUSABLE);

    LocalValue out_of_bounds(context, eval(context, R"JS(
        (() => {
          const buffer = new ArrayBuffer(8, { maxByteLength: 16 });
          const view = new Uint8Array(buffer, 4, 4);
          buffer.resize(2);
          return view;
        })()
    )JS"));
    ASSERT_FALSE(out_of_bounds.is_exception());
    expect_status_without_exception(context, out_of_bounds.get(),
                                    JS_OBJECT_BYTES_UNUSABLE);
}

TEST(QuickJSObjectByteSpan, InitializesOutputsAndPreservesPendingException)
{
    RuntimeFixture fixture;
    ASSERT_TRUE(fixture.ready());
    JSContext *context = fixture.context();
    std::array<std::uint8_t, 2> const bytes{7, 9};
    LocalValue buffer(
        context, JS_NewArrayBufferCopy(context, bytes.data(), bytes.size()));
    ASSERT_FALSE(buffer.is_exception());
    LocalValue detached(
        context, JS_NewArrayBufferCopy(context, bytes.data(), bytes.size()));
    ASSERT_FALSE(detached.is_exception());
    JS_DetachArrayBuffer(context, detached.get());

    struct Case {
        JSValueConst input;
        JSObjectByteSpanStatus status;
    } const cases[] = {
        {buffer.get(), JS_OBJECT_BYTES_OK},
        {JS_NewInt32(context, 12), JS_OBJECT_BYTES_WRONG_KIND},
        {detached.get(), JS_OBJECT_BYTES_UNUSABLE},
    };

    for (Case const &test_case : cases) {
        ASSERT_TRUE(JS_IsException(JS_Throw(context, JS_NewInt32(context, 73))));
        JSValue owned = JS_NULL;
        auto const *data = reinterpret_cast<std::uint8_t const *>(1);
        std::size_t size = 99;
        EXPECT_EQ(JS_GetObjectByteSpanNoThrow(context, test_case.input, &owned,
                                             &data, &size),
                  test_case.status);
        ASSERT_TRUE(JS_HasException(context));
        LocalValue pending(context, JS_GetException(context));
        std::int32_t sentinel = 0;
        ASSERT_EQ(JS_ToInt32(context, &sentinel, pending.get()), 0);
        EXPECT_EQ(sentinel, 73);
        if (test_case.status == JS_OBJECT_BYTES_OK) {
            EXPECT_TRUE(JS_IsObject(owned));
            EXPECT_EQ(size, bytes.size());
        } else {
            expect_no_owned_span(owned, data, size);
        }
        JS_FreeValue(context, owned);
        EXPECT_FALSE(JS_HasException(context));
    }
}

TEST(QuickJSObjectByteSpan, PerformsNoAllocatorRequest)
{
    RuntimeFixture fixture;
    ASSERT_TRUE(fixture.ready());
    JSContext *context = fixture.context();
    std::array<std::uint8_t, 3> const bytes{1, 2, 3};
    LocalValue buffer(
        context, JS_NewArrayBufferCopy(context, bytes.data(), bytes.size()));
    LocalValue view(context, make_typed_array(context, buffer.get(), 0, 3));
    ASSERT_FALSE(buffer.is_exception());
    ASSERT_FALSE(view.is_exception());

    std::size_t const before = fixture.allocator().requests;
    for (JSValueConst input : {buffer.get(), view.get(), JS_NULL}) {
        JSValue owned = JS_UNDEFINED;
        std::uint8_t const *data = nullptr;
        std::size_t size = 0;
        (void)JS_GetObjectByteSpanNoThrow(context, input, &owned, &data, &size);
        JS_FreeValue(context, owned);
    }
    EXPECT_EQ(fixture.allocator().requests, before);
    EXPECT_EQ(fixture.allocator().rejections, 0u);
    EXPECT_FALSE(JS_HasException(context));
}

TEST(QuickJSBareTypeError, ReturnsRealmTypeErrorUnthrownAndBare)
{
    RuntimeFixture fixture;
    ASSERT_TRUE(fixture.ready());
    JSContext *context = fixture.context();
    LocalValue error(context, JS_NewBareTypeErrorExact(context));
    ASSERT_FALSE(error.is_exception());
    EXPECT_FALSE(JS_HasException(context));
    expect_bare_type_error(context, error.get());
}

TEST(QuickJSBareTypeError, PreservesAnExistingExceptionOnSuccess)
{
    RuntimeFixture fixture;
    ASSERT_TRUE(fixture.ready());
    JSContext *context = fixture.context();
    ASSERT_TRUE(JS_IsException(JS_Throw(context, JS_NewInt32(context, 91))));

    LocalValue error(context, JS_NewBareTypeErrorExact(context));
    ASSERT_FALSE(error.is_exception());
    ASSERT_TRUE(JS_HasException(context));
    LocalValue pending(context, JS_GetException(context));
    std::int32_t sentinel = 0;
    ASSERT_EQ(JS_ToInt32(context, &sentinel, pending.get()), 0);
    EXPECT_EQ(sentinel, 91);
    expect_bare_type_error(context, error.get());
}

TEST(QuickJSBareTypeError, EveryAllocationOOMPreservesOOMAndRetrySucceeds)
{
    std::size_t allocation_requests = 0;
    {
        RuntimeFixture measured;
        ASSERT_TRUE(measured.ready());
        std::size_t const before = measured.allocator().requests;
        LocalValue error(measured.context(),
                         JS_NewBareTypeErrorExact(measured.context()));
        ASSERT_FALSE(error.is_exception());
        allocation_requests = measured.allocator().requests - before;
    }

    ASSERT_EQ(allocation_requests, 3u)
        << "the pinned helper transaction is shape, object, property store";

    for (std::size_t ordinal = 1; ordinal <= allocation_requests; ++ordinal) {
        RuntimeFixture fixture;
        ASSERT_TRUE(fixture.ready()) << ordinal;
        JSContext *context = fixture.context();
        {
            LocalValue warmup(context, JS_NewBareTypeErrorExact(context));
            ASSERT_FALSE(warmup.is_exception()) << ordinal;
            expect_bare_type_error(context, warmup.get());
        }
        std::size_t const live_before = fixture.allocator().live_blocks;
        fixture.allocator().reject_relative(ordinal);

        JSValue failed = JS_NewBareTypeErrorExact(context);
        EXPECT_TRUE(JS_IsException(failed)) << ordinal;
        EXPECT_EQ(fixture.allocator().rejections, 1u) << ordinal;
        ASSERT_TRUE(JS_HasException(context)) << ordinal;
        {
            LocalValue exception(context, JS_GetException(context));
            EXPECT_TRUE(JS_IsError(context, exception.get())) << ordinal;
            EXPECT_EQ(string_property(context, exception.get(), "name"),
                      "InternalError")
                << ordinal;
            EXPECT_EQ(string_property(context, exception.get(), "message"),
                      "out of memory")
                << ordinal;
        }
        EXPECT_FALSE(JS_HasException(context)) << ordinal;
        EXPECT_EQ(fixture.allocator().live_blocks, live_before) << ordinal;

        {
            LocalValue retry(context, JS_NewBareTypeErrorExact(context));
            ASSERT_FALSE(retry.is_exception()) << ordinal;
            expect_bare_type_error(context, retry.get());
        }
        EXPECT_FALSE(JS_HasException(context)) << ordinal;
        EXPECT_EQ(fixture.allocator().live_blocks, live_before) << ordinal;

        fixture.close();
        EXPECT_EQ(fixture.allocator().live_blocks, 0u) << ordinal;
    }
}

} // namespace
