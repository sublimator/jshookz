#include "js.hpp"
#include "object/object.hpp"
#include "result.hpp"

#include "catl/xdata/static_protocol.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern "C" bool register_cpp_types(JSContext *ctx);
extern "C" void unregister_cpp_types(JSRuntime *runtime);
extern "C" bool register_uint_types(JSContext *ctx);

namespace {

namespace types = jshookz::provider::types;
namespace xdata = catl::xdata;

struct alignas(std::max_align_t) AllocationHeader {
  std::size_t size = 0;
};

enum class RequestKind : std::uint8_t {
  allocate,
  reallocate,
};

struct AllocationRequest {
  std::size_t size = 0;
  RequestKind kind = RequestKind::allocate;

  friend bool operator==(AllocationRequest const &,
                         AllocationRequest const &) = default;
};

struct AllocationState {
  std::size_t blocks = 0;
  std::size_t bytes = 0;

  friend bool operator==(AllocationState const &,
                         AllocationState const &) = default;
};

struct AllocatorControl {
  static constexpr std::size_t maxRecordedRequests = 8192;

  std::size_t requests = 0;
  std::size_t rejectAt = 0;
  std::size_t rejections = 0;
  std::size_t liveBlocks = 0;
  std::size_t liveBytes = 0;
  bool recording = false;
  std::array<AllocationRequest, maxRecordedRequests> recorded{};
  std::size_t recordedCount = 0;

  void startRecording() noexcept {
    recordedCount = 0;
    recording = true;
  }

  void stopRecording() noexcept { recording = false; }

  void rejectRelative(std::size_t ordinal) noexcept {
    rejectAt = requests + ordinal;
  }

  void disarm() noexcept { rejectAt = 0; }

  [[nodiscard]] AllocationState state() const noexcept {
    return {liveBlocks, liveBytes};
  }
};

[[nodiscard]] bool wouldExceed(JSMallocState const *state, std::size_t oldSize,
                               std::size_t newSize) noexcept {
  if (state->malloc_size < oldSize)
    return true;
  std::size_t const retained = state->malloc_size - oldSize;
  return newSize > state->malloc_limit ||
         retained > state->malloc_limit - newSize;
}

[[nodiscard]] bool recordAndReject(JSMallocState *state, std::size_t size,
                                   RequestKind kind) noexcept {
  auto *control = static_cast<AllocatorControl *>(state->opaque);
  ++control->requests;
  if (control->recording) {
    if (control->recordedCount < control->recorded.size())
      control->recorded[control->recordedCount] = {size, kind};
    ++control->recordedCount;
  }
  if (control->rejectAt == 0 || control->requests != control->rejectAt)
    return false;
  control->rejectAt = 0;
  ++control->rejections;
  return true;
}

void *testMalloc(JSMallocState *state, std::size_t size) {
  auto *control = static_cast<AllocatorControl *>(state->opaque);
  if (recordAndReject(state, size, RequestKind::allocate) || size == 0 ||
      size >
          std::numeric_limits<std::size_t>::max() - sizeof(AllocationHeader) ||
      wouldExceed(state, 0, size))
    return nullptr;
  auto *header = static_cast<AllocationHeader *>(
      std::malloc(sizeof(AllocationHeader) + size));
  if (header == nullptr)
    return nullptr;
  header->size = size;
  ++state->malloc_count;
  state->malloc_size += size;
  ++control->liveBlocks;
  control->liveBytes += size;
  return header + 1;
}

void testFree(JSMallocState *state, void *pointer) {
  if (pointer == nullptr)
    return;
  auto *control = static_cast<AllocatorControl *>(state->opaque);
  auto *header = static_cast<AllocationHeader *>(pointer) - 1;
  --state->malloc_count;
  state->malloc_size -= header->size;
  --control->liveBlocks;
  control->liveBytes -= header->size;
  std::free(header);
}

void *testRealloc(JSMallocState *state, void *pointer, std::size_t size) {
  if (pointer == nullptr)
    return size == 0 ? nullptr : testMalloc(state, size);
  if (size == 0) {
    testFree(state, pointer);
    return nullptr;
  }
  if (recordAndReject(state, size, RequestKind::reallocate))
    return nullptr;
  auto *control = static_cast<AllocatorControl *>(state->opaque);
  auto *header = static_cast<AllocationHeader *>(pointer) - 1;
  std::size_t const oldSize = header->size;
  if (size >
          std::numeric_limits<std::size_t>::max() - sizeof(AllocationHeader) ||
      wouldExceed(state, oldSize, size))
    return nullptr;
  auto *replacement = static_cast<AllocationHeader *>(
      std::realloc(header, sizeof(AllocationHeader) + size));
  if (replacement == nullptr)
    return nullptr;
  replacement->size = size;
  state->malloc_size = state->malloc_size - oldSize + size;
  control->liveBytes = control->liveBytes - oldSize + size;
  return replacement + 1;
}

[[nodiscard]] std::size_t testUsableSize(void const *pointer) {
  if (pointer == nullptr)
    return 0;
  return (static_cast<AllocationHeader const *>(pointer) - 1)->size;
}

constexpr JSMallocFunctions testAllocator = {
    .js_malloc = testMalloc,
    .js_free = testFree,
    .js_realloc = testRealloc,
    .js_malloc_usable_size = testUsableSize,
};

class LocalValue {
public:
  explicit LocalValue(JSContext *context, JSValue value = JS_UNDEFINED) noexcept
      : context_(context), value_(value) {}

  ~LocalValue() { JS_FreeValue(context_, value_); }

  LocalValue(LocalValue const &) = delete;
  LocalValue &operator=(LocalValue const &) = delete;

  LocalValue(LocalValue &&other) noexcept
      : context_(other.context_), value_(other.release()) {}

  LocalValue &operator=(LocalValue &&other) noexcept {
    if (this != &other) {
      JS_FreeValue(context_, value_);
      context_ = other.context_;
      value_ = other.release();
    }
    return *this;
  }

  [[nodiscard]] JSValueConst get() const noexcept { return value_; }

  [[nodiscard]] bool isException() const noexcept {
    return JS_IsException(value_);
  }

  [[nodiscard]] JSValue release() noexcept {
    JSValue value = value_;
    value_ = JS_UNDEFINED;
    return value;
  }

  void reset(JSValue value = JS_UNDEFINED) noexcept {
    JS_FreeValue(context_, value_);
    value_ = value;
  }

private:
  JSContext *context_;
  JSValue value_;
};

class ReflectionRuntime {
public:
  ReflectionRuntime() = default;
  ReflectionRuntime(ReflectionRuntime const &) = delete;
  ReflectionRuntime &operator=(ReflectionRuntime const &) = delete;

  ~ReflectionRuntime() { close(); }

  [[nodiscard]] bool open() {
    runtime_ = JS_NewRuntime2(&testAllocator, &allocator);
    if (runtime_ == nullptr)
      return false;
    context_ = JS_NewContext(runtime_);
    if (context_ == nullptr)
      return false;
    if (!jshookz::provider::bindings::registerResult(context_) ||
        !register_cpp_types(context_) || !register_uint_types(context_) ||
        JS_HasException(context_))
      return false;
    return installRoutes();
  }

  void close() noexcept {
    if (context_ != nullptr) {
      JS_FreeContext(context_);
      context_ = nullptr;
    }
    if (runtime_ != nullptr) {
      unregister_cpp_types(runtime_);
      JS_FreeRuntime(runtime_);
      runtime_ = nullptr;
    }
  }

  [[nodiscard]] JSContext *context() const noexcept { return context_; }
  [[nodiscard]] JSRuntime *runtime() const noexcept { return runtime_; }

  [[nodiscard]] LocalValue route(char const *name) const {
    LocalValue global(context_, JS_GetGlobalObject(context_));
    if (global.isException())
      return LocalValue(context_, JS_EXCEPTION);
    LocalValue routes(context_, JS_GetPropertyStr(context_, global.get(),
                                                  "__reflectionRoutes"));
    if (routes.isException())
      return LocalValue(context_, JS_EXCEPTION);
    return LocalValue(context_,
                      JS_GetPropertyStr(context_, routes.get(), name));
  }

  AllocatorControl allocator;

private:
  [[nodiscard]] bool installRoutes() {
    static constexpr char source[] = R"JS(
          globalThis.__reflectionRoutes = Object.freeze({
            ownKeys: value => Reflect.ownKeys(value),
            ownNames: value => Object.getOwnPropertyNames(value),
            keys: value => Object.keys(value),
            values: value => Object.values(value),
            entries: value => Object.entries(value),
            descriptors: value => Object.getOwnPropertyDescriptors(value),
            spread: value => ({...value}),
            assign: value => Object.assign({}, value),
            forIn: value => {
              let count = 0;
              for (const key in value)
                ++count;
              return count;
            },
            descriptor: (value, key) =>
              Object.getOwnPropertyDescriptor(value, key),
          });
        )JS";
    LocalValue installed(context_, JS_Eval(context_, source, sizeof(source) - 1,
                                           "<object-reflection-oom-routes>",
                                           JS_EVAL_TYPE_GLOBAL));
    return !installed.isException() && !JS_HasException(context_);
  }

  JSRuntime *runtime_ = nullptr;
  JSContext *context_ = nullptr;
};

enum class ContainerKind : std::uint8_t {
  object,
  array,
};

struct ContainerBlueprint {
  std::string name;
  ContainerKind kind = ContainerKind::object;
  std::vector<std::uint8_t> bytes;
  std::vector<std::string> enumerableKeys;
  std::vector<std::string> ownKeys;
};

void appendHeader(std::vector<std::uint8_t> &bytes, std::uint32_t code) {
  auto const type = static_cast<std::uint8_t>(code >> 16);
  auto const nth = static_cast<std::uint8_t>(code);
  std::uint8_t first = 0;
  if (type < 16)
    first = static_cast<std::uint8_t>(type << 4);
  if (nth < 16)
    first = static_cast<std::uint8_t>(first | nth);
  bytes.push_back(first);
  if (type >= 16)
    bytes.push_back(type);
  if (nth >= 16)
    bytes.push_back(nth);
}

void appendValidPayload(std::vector<std::uint8_t> &bytes,
                        xdata::StaticFieldDescriptor const &descriptor) {
  if ((descriptor.flags & xdata::field_vl_encoded) != 0) {
    // Empty Blob, AccountID, and Vector256 payloads are canonical and keep
    // the all-field maximum fixture compact. Zero is the one-byte VL form.
    bytes.push_back(0);
    return;
  }
  switch (descriptor.wire_type) {
  case 6: // native Amount zero
    bytes.push_back(0x40);
    bytes.insert(bytes.end(), 7, 0);
    return;
  case 9: { // canonical Number 1.25
    static constexpr std::array<std::uint8_t, 12> number = {
        0x00, 0x04, 0x70, 0xDE, 0x4D, 0xF8, 0x20, 0x00, 0xFF, 0xFF, 0xFF, 0xF1,
    };
    bytes.insert(bytes.end(), number.begin(), number.end());
    return;
  }
  case 14: // empty nested STObject
    bytes.push_back(0xE1);
    return;
  case 15: // empty nested STArray
    bytes.push_back(0xF1);
    return;
  case 18: // one AccountID PathHop followed by PathSet end
    bytes.push_back(0x01);
    bytes.insert(bytes.end(), 20, 0);
    bytes.push_back(0x00);
    return;
  case 24: // native Issue: native currency only
    bytes.insert(bytes.end(), 20, 0);
    return;
  case 25: // empty door, native issue, empty door, native issue
    bytes.push_back(0);
    bytes.insert(bytes.end(), 20, 0);
    bytes.push_back(0);
    bytes.insert(bytes.end(), 20, 0);
    return;
  default:
    bytes.insert(bytes.end(), descriptor.fixed_size, 0);
    return;
  }
}

[[nodiscard]] std::string
fieldName(xdata::ProtocolView const &protocol,
          xdata::StaticFieldDescriptor const &descriptor) {
  auto const name = protocol.field_name(descriptor.name_ordinal);
  return {name.data, name.size};
}

[[nodiscard]] ContainerBlueprint
objectBlueprint(std::string name, std::uint32_t uint32FieldCount) {
  ContainerBlueprint result{
      .name = std::move(name),
      .kind = ContainerKind::object,
  };
  auto const &protocol = xdata::xahau_static_protocol();
  struct Row {
    xdata::StaticFieldDescriptor const *descriptor = nullptr;
  };
  std::vector<Row> rows;
  for (std::uint16_t ordinal = 0; ordinal < protocol.material_field_count;
       ++ordinal) {
    auto const *material = protocol.material_field(ordinal);
    auto const *descriptor = material == nullptr
                                 ? nullptr
                                 : protocol.field_by_code(material->field_code);
    if (descriptor != nullptr && descriptor->wire_type == 2)
      rows.push_back({descriptor});
  }
  std::sort(rows.begin(), rows.end(), [](Row const &left, Row const &right) {
    return left.descriptor->code < right.descriptor->code;
  });
  if (uint32FieldCount < rows.size())
    rows.resize(uint32FieldCount);
  for (auto const &row : rows) {
    appendHeader(result.bytes, row.descriptor->code);
    result.bytes.insert(result.bytes.end(), 4, 0);
    result.ownKeys.push_back(fieldName(protocol, *row.descriptor));
  }
  result.enumerableKeys = result.ownKeys;
  return result;
}

[[nodiscard]] ContainerBlueprint maximumObjectBlueprint() {
  ContainerBlueprint result{
      .name = "object-maximum-325",
      .kind = ContainerKind::object,
  };
  auto const &protocol = xdata::xahau_static_protocol();
  std::vector<xdata::StaticFieldDescriptor const *> rows;
  rows.reserve(protocol.material_field_count);
  for (std::uint16_t ordinal = 0; ordinal < protocol.material_field_count;
       ++ordinal) {
    auto const *material = protocol.material_field(ordinal);
    auto const *descriptor = material == nullptr
                                 ? nullptr
                                 : protocol.field_by_code(material->field_code);
    if (descriptor != nullptr)
      rows.push_back(descriptor);
  }
  std::sort(rows.begin(), rows.end(), [](auto const *left, auto const *right) {
    return left->code < right->code;
  });
  for (auto const *descriptor : rows) {
    appendHeader(result.bytes, descriptor->code);
    appendValidPayload(result.bytes, *descriptor);
    result.ownKeys.push_back(fieldName(protocol, *descriptor));
  }
  result.enumerableKeys = result.ownKeys;
  return result;
}

[[nodiscard]] ContainerBlueprint arrayBlueprint(std::string name,
                                                std::uint32_t elements) {
  ContainerBlueprint result{
      .name = std::move(name),
      .kind = ContainerKind::array,
  };
  // Memos (STArray) contains repeated Memo (STObject) elements.
  result.bytes.push_back(0xF9);
  result.bytes.reserve(2 + static_cast<std::size_t>(elements) * 2);
  for (std::uint32_t index = 0; index < elements; ++index) {
    result.bytes.push_back(0xEA);
    result.bytes.push_back(0xE1);
    result.enumerableKeys.push_back(std::to_string(index));
  }
  result.bytes.push_back(0xF1);
  result.ownKeys = result.enumerableKeys;
  result.ownKeys.emplace_back("length");
  return result;
}

class PreparedContainer {
public:
  PreparedContainer(JSContext *context, ContainerBlueprint const &blueprint)
      : context_(context), blueprint_(blueprint), carrier_(context),
        target_(context) {}

  [[nodiscard]] bool prepare() {
    carrier_.reset(types::makeCertifiedObjectCopy(
        context_, blueprint_.bytes.data(),
        static_cast<std::uint32_t>(blueprint_.bytes.size())));
    if (carrier_.isException())
      return false;
    if (blueprint_.kind == ContainerKind::object) {
      target_.reset(JS_DupValue(context_, carrier_.get()));
    } else {
      target_.reset(JS_GetPropertyStr(context_, carrier_.get(), "Memos"));
    }
    return !target_.isException() &&
           (blueprint_.kind == ContainerKind::object
                ? types::isSTObject(target_.get())
                : types::isSTArray(target_.get())) &&
           !JS_HasException(context_);
  }

  [[nodiscard]] JSValueConst carrier() const noexcept { return carrier_.get(); }
  [[nodiscard]] JSValueConst target() const noexcept { return target_.get(); }

  [[nodiscard]] LocalValue valueAt(std::size_t ordinal) const {
    if (blueprint_.kind == ContainerKind::array) {
      return LocalValue(
          context_, JS_GetPropertyUint32(context_, target_.get(),
                                         static_cast<std::uint32_t>(ordinal)));
    }
    return LocalValue(
        context_,
        JS_GetPropertyStr(context_, target_.get(),
                          blueprint_.enumerableKeys[ordinal].c_str()));
  }

private:
  JSContext *context_;
  ContainerBlueprint const &blueprint_;
  LocalValue carrier_;
  LocalValue target_;
};

[[nodiscard]] bool copyContiguousBytes(JSContext *context, JSValueConst value,
                                       std::vector<std::uint8_t> &output) {
  JSValue backing = JS_UNDEFINED;
  std::uint8_t const *data = nullptr;
  std::size_t size = 0;
  auto const status =
      JS_GetObjectByteSpanNoThrow(context, value, &backing, &data, &size);
  if (status != JS_OBJECT_BYTES_OK) {
    JS_FreeValue(context, backing);
    return false;
  }
  if (size == 0)
    output.clear();
  else
    output.assign(data, data + size);
  JS_FreeValue(context, backing);
  return true;
}

[[nodiscard]] bool snapshotCarrier(JSContext *context, JSValueConst carrier,
                                   std::vector<std::uint8_t> &output) {
  LocalValue method(context, JS_GetPropertyStr(context, carrier, "toBytes"));
  if (method.isException())
    return false;
  LocalValue bytes(context,
                   JS_Call(context, method.get(), carrier, 0, nullptr));
  return !bytes.isException() &&
         copyContiguousBytes(context, bytes.get(), output);
}

[[nodiscard]] std::string stringValue(JSContext *context, JSValueConst value) {
  char const *text = JS_ToCString(context, value);
  if (text == nullptr)
    return {};
  std::string result{text};
  JS_FreeCString(context, text);
  return result;
}

[[nodiscard]] std::string
stringProperty(JSContext *context, JSValueConst object, char const *name) {
  LocalValue value(context, JS_GetPropertyStr(context, object, name));
  return value.isException() ? std::string{}
                             : stringValue(context, value.get());
}

void expectOnePendingOOM(JSContext *context, std::size_t ordinal) {
  SCOPED_TRACE(ordinal);
  ASSERT_TRUE(JS_HasException(context));
  LocalValue exception(context, JS_GetException(context));
  ASSERT_TRUE(JS_IsError(context, exception.get()));
  EXPECT_EQ(stringProperty(context, exception.get(), "name"), "InternalError");
  EXPECT_EQ(stringProperty(context, exception.get(), "message"),
            "out of memory");
  EXPECT_FALSE(JS_HasException(context));
}

void expectSourceBytes(JSContext *context, PreparedContainer const &source,
                       ContainerBlueprint const &blueprint) {
  std::vector<std::uint8_t> observed;
  ASSERT_TRUE(snapshotCarrier(context, source.carrier(), observed));
  EXPECT_EQ(observed, blueprint.bytes);
  EXPECT_FALSE(JS_HasException(context));
}

[[nodiscard]] std::uint32_t arrayLength(JSContext *context,
                                        JSValueConst array) {
  LocalValue length(context, JS_GetPropertyStr(context, array, "length"));
  if (length.isException())
    return std::numeric_limits<std::uint32_t>::max();
  std::uint32_t result = 0;
  if (JS_ToUint32(context, &result, length.get()) < 0)
    return std::numeric_limits<std::uint32_t>::max();
  return result;
}

[[nodiscard]] std::array<std::size_t, 3> sentinelOrdinals(std::size_t count) {
  if (count == 0)
    return {0, 0, 0};
  return {0, count / 2, count - 1};
}

void expectPropertyEnum(JSContext *context, JSPropertyEnum const *table,
                        std::uint32_t length,
                        std::span<std::string const> expected) {
  ASSERT_EQ(length, expected.size());
  if (expected.empty())
    return;
  auto const sentinels = sentinelOrdinals(expected.size());
  for (std::size_t ordinal : sentinels) {
    LocalValue atomValue(context, JS_AtomToValue(context, table[ordinal].atom));
    ASSERT_FALSE(atomValue.isException());
    EXPECT_EQ(stringValue(context, atomValue.get()), expected[ordinal]);
  }
}

[[nodiscard]] bool exactOwnNameTrace(std::span<AllocationRequest const> trace,
                                     std::uint32_t keys, bool emptyObject) {
  std::size_t const tableBytes =
      sizeof(JSPropertyEnum) * static_cast<std::size_t>(std::max(keys, 1u));
  if (emptyObject)
    return trace.size() == 1 &&
           trace[0] == AllocationRequest{tableBytes, RequestKind::allocate};
  return trace.size() == 2 &&
         trace[0] == AllocationRequest{sizeof(JSPropertyEnum) *
                                           static_cast<std::size_t>(keys),
                                       RequestKind::allocate} &&
         trace[1] == AllocationRequest{tableBytes, RequestKind::allocate};
}

void expectRuntimeBaseline(ReflectionRuntime &runtime, AllocationState baseline,
                           char const *route) {
  JS_RunGC(runtime.runtime());
  // QuickJS may retain a larger backing store in an already-live runtime
  // table after a successful retry. It may not retain another allocation.
  // Exact byte cleanup for operation-owned blocks is separately checked
  // while the source is live; runtime teardown must still reach zero bytes.
  EXPECT_EQ(runtime.allocator.state().blocks, baseline.blocks) << route;
  EXPECT_FALSE(JS_HasException(runtime.context())) << route;
}

enum class RouteResult : std::uint8_t {
  ownKeys,
  enumerableKeys,
  values,
  entries,
  descriptors,
  copiedObject,
  count,
};

struct ReflectionRoute {
  char const *name;
  RouteResult result;
  bool materializesValues;
};

constexpr std::array<ReflectionRoute, 9> reflectionRoutes = {{
    {"ownKeys", RouteResult::ownKeys, false},
    {"ownNames", RouteResult::ownKeys, false},
    {"keys", RouteResult::enumerableKeys, true},
    {"values", RouteResult::values, true},
    {"entries", RouteResult::entries, true},
    {"descriptors", RouteResult::descriptors, true},
    {"spread", RouteResult::copiedObject, true},
    {"assign", RouteResult::copiedObject, true},
    {"forIn", RouteResult::count, true},
}};

[[nodiscard]] LocalValue
sourcePropertyByKey(JSContext *context, JSValueConst source,
                    ContainerBlueprint const &blueprint, std::size_t ordinal) {
  if (blueprint.kind == ContainerKind::array)
    return LocalValue(
        context, JS_GetPropertyUint32(context, source,
                                      static_cast<std::uint32_t>(ordinal)));
  return LocalValue(
      context, JS_GetPropertyStr(context, source,
                                 blueprint.enumerableKeys[ordinal].c_str()));
}

void expectStringArray(JSContext *context, JSValueConst result,
                       std::span<std::string const> expected) {
  ASSERT_EQ(arrayLength(context, result), expected.size());
  if (expected.empty())
    return;
  auto const sentinels = sentinelOrdinals(expected.size());
  for (std::size_t ordinal : sentinels) {
    LocalValue value(context,
                     JS_GetPropertyUint32(context, result,
                                          static_cast<std::uint32_t>(ordinal)));
    ASSERT_FALSE(value.isException());
    EXPECT_EQ(stringValue(context, value.get()), expected[ordinal]);
  }
}

void expectValueIdentity(JSContext *context, JSValueConst source,
                         JSValueConst observed,
                         ContainerBlueprint const &blueprint,
                         std::size_t ordinal) {
  LocalValue expected(sourcePropertyByKey(context, source, blueprint, ordinal));
  ASSERT_FALSE(expected.isException());
  EXPECT_EQ(JS_StrictEq(context, observed, expected.get()), 1);
}

void expectOwnPropertyCount(JSContext *context, JSValueConst object,
                            std::uint32_t expected) {
  JSPropertyEnum *table = nullptr;
  std::uint32_t length = 0;
  ASSERT_EQ(JS_GetOwnPropertyNames(context, &table, &length, object,
                                   JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK),
            0);
  JS_FreePropertyEnum(context, table, length);
  EXPECT_EQ(length, expected);
}

void expectDescriptor(JSContext *context, JSValueConst descriptor,
                      JSValueConst source, ContainerBlueprint const &blueprint,
                      std::size_t ordinal) {
  ASSERT_TRUE(JS_IsObject(descriptor));
  LocalValue value(context, JS_GetPropertyStr(context, descriptor, "value"));
  LocalValue writable(context,
                      JS_GetPropertyStr(context, descriptor, "writable"));
  LocalValue enumerable(context,
                        JS_GetPropertyStr(context, descriptor, "enumerable"));
  LocalValue configurable(
      context, JS_GetPropertyStr(context, descriptor, "configurable"));
  ASSERT_FALSE(value.isException());
  ASSERT_FALSE(writable.isException());
  ASSERT_FALSE(enumerable.isException());
  ASSERT_FALSE(configurable.isException());
  expectValueIdentity(context, source, value.get(), blueprint, ordinal);
  EXPECT_FALSE(JS_ToBool(context, writable.get()));
  EXPECT_TRUE(JS_ToBool(context, enumerable.get()));
  EXPECT_FALSE(JS_ToBool(context, configurable.get()));
}

void expectRouteResult(JSContext *context, JSValueConst source,
                       JSValueConst result, ContainerBlueprint const &blueprint,
                       ReflectionRoute const &route) {
  ASSERT_FALSE(JS_IsException(result));
  auto const &enumerable = blueprint.enumerableKeys;
  auto const &own = blueprint.ownKeys;
  switch (route.result) {
  case RouteResult::ownKeys:
    expectStringArray(context, result, own);
    break;
  case RouteResult::enumerableKeys:
    expectStringArray(context, result, enumerable);
    break;
  case RouteResult::values: {
    ASSERT_EQ(arrayLength(context, result), enumerable.size());
    if (!enumerable.empty()) {
      for (std::size_t ordinal : sentinelOrdinals(enumerable.size())) {
        LocalValue observed(
            context, JS_GetPropertyUint32(context, result,
                                          static_cast<std::uint32_t>(ordinal)));
        ASSERT_FALSE(observed.isException());
        expectValueIdentity(context, source, observed.get(), blueprint,
                            ordinal);
      }
    }
    break;
  }
  case RouteResult::entries: {
    ASSERT_EQ(arrayLength(context, result), enumerable.size());
    if (!enumerable.empty()) {
      for (std::size_t ordinal : sentinelOrdinals(enumerable.size())) {
        LocalValue pair(
            context, JS_GetPropertyUint32(context, result,
                                          static_cast<std::uint32_t>(ordinal)));
        LocalValue key(context, JS_GetPropertyUint32(context, pair.get(), 0));
        LocalValue value(context, JS_GetPropertyUint32(context, pair.get(), 1));
        ASSERT_FALSE(pair.isException());
        ASSERT_FALSE(key.isException());
        ASSERT_FALSE(value.isException());
        EXPECT_EQ(stringValue(context, key.get()), enumerable[ordinal]);
        expectValueIdentity(context, source, value.get(), blueprint, ordinal);
      }
    }
    break;
  }
  case RouteResult::descriptors: {
    expectOwnPropertyCount(context, result,
                           static_cast<std::uint32_t>(own.size()));
    if (!enumerable.empty()) {
      for (std::size_t ordinal : sentinelOrdinals(enumerable.size())) {
        LocalValue descriptor(
            context,
            JS_GetPropertyStr(context, result, enumerable[ordinal].c_str()));
        ASSERT_FALSE(descriptor.isException());
        expectDescriptor(context, descriptor.get(), source, blueprint, ordinal);
      }
    }
    if (blueprint.kind == ContainerKind::array) {
      LocalValue descriptor(context,
                            JS_GetPropertyStr(context, result, "length"));
      LocalValue value(context,
                       JS_GetPropertyStr(context, descriptor.get(), "value"));
      LocalValue enumerableValue(
          context, JS_GetPropertyStr(context, descriptor.get(), "enumerable"));
      std::uint32_t length = 0;
      ASSERT_FALSE(descriptor.isException());
      ASSERT_EQ(JS_ToUint32(context, &length, value.get()), 0);
      EXPECT_EQ(length, enumerable.size());
      EXPECT_FALSE(JS_ToBool(context, enumerableValue.get()));
    }
    break;
  }
  case RouteResult::copiedObject: {
    expectOwnPropertyCount(context, result,
                           static_cast<std::uint32_t>(enumerable.size()));
    if (!enumerable.empty()) {
      for (std::size_t ordinal : sentinelOrdinals(enumerable.size())) {
        LocalValue observed(
            context,
            JS_GetPropertyStr(context, result, enumerable[ordinal].c_str()));
        ASSERT_FALSE(observed.isException());
        expectValueIdentity(context, source, observed.get(), blueprint,
                            ordinal);
      }
    }
    break;
  }
  case RouteResult::count: {
    std::uint32_t count = 0;
    ASSERT_EQ(JS_ToUint32(context, &count, result), 0);
    EXPECT_EQ(count, enumerable.size());
    break;
  }
  }
  EXPECT_FALSE(JS_HasException(context));
}

[[nodiscard]] LocalValue callRoute(JSContext *context, JSValueConst function,
                                   JSValueConst target) {
  JSValueConst arguments[] = {target};
  return LocalValue(context,
                    JS_Call(context, function, JS_UNDEFINED, 1, arguments));
}

struct RouteCoverage {
  bool measured = false;
  bool everyRequestRejected = false;
  bool exactOOM = false;
  bool sourceStable = false;
  bool retryComplete = false;
  bool preexistingIdentityStable = false;
  bool completedInsertIdentityStable = false;
  bool escapedPendingOOM = false;

  [[nodiscard]] bool complete(bool hasValues) const noexcept {
    return !escapedPendingOOM && measured && everyRequestRejected && exactOOM &&
           sourceStable && retryComplete &&
           (!hasValues ||
            (preexistingIdentityStable && completedInsertIdentityStable));
  }
};

void sweepEveryRouteAllocation(ReflectionRuntime &runtime,
                               ContainerBlueprint const &blueprint,
                               ReflectionRoute const &route,
                               RouteCoverage &coverage) {
  JSContext *context = runtime.context();
  LocalValue function(runtime.route(route.name));
  EXPECT_TRUE(JS_IsFunction(context, function.get()));

  // Warm bytecode, route-specific shapes, atoms, and result layouts before
  // the request sequence becomes an asserted transaction.
  {
    PreparedContainer warmSource(context, blueprint);
    EXPECT_TRUE(warmSource.prepare());
    LocalValue warm(callRoute(context, function.get(), warmSource.target()));
    ASSERT_FALSE(warm.isException());
    expectRouteResult(context, warmSource.target(), warm.get(), blueprint,
                      route);
  }
  JS_RunGC(runtime.runtime());
  AllocationState const warmedBaseline = runtime.allocator.state();

  std::size_t requestCount = 0;
  {
    PreparedContainer measuredSource(context, blueprint);
    EXPECT_TRUE(measuredSource.prepare());
    LocalValue preexisting(context);
    if (!blueprint.enumerableKeys.empty()) {
      preexisting = measuredSource.valueAt(0);
      ASSERT_FALSE(preexisting.isException());
    }
    runtime.allocator.startRecording();
    std::size_t const before = runtime.allocator.requests;
    LocalValue measured(
        callRoute(context, function.get(), measuredSource.target()));
    requestCount = runtime.allocator.requests - before;
    runtime.allocator.stopRecording();
    ASSERT_FALSE(measured.isException());
    ASSERT_EQ(runtime.allocator.recordedCount, requestCount);
    ASSERT_LE(runtime.allocator.recordedCount,
              runtime.allocator.recorded.size());
    expectRouteResult(context, measuredSource.target(), measured.get(),
                      blueprint, route);
  }
  ASSERT_GT(requestCount, 0u) << route.name;
  expectRuntimeBaseline(runtime, warmedBaseline, route.name);
  coverage.measured = true;

  bool everyRejected = true;
  bool everyOOM = true;
  bool everySourceStable = true;
  bool everyRetryComplete = true;
  bool everyPreexistingStable = true;
  bool everyCompletedStable = true;
  bool sawCompletedInsertSurvivor =
      !route.materializesValues || blueprint.enumerableKeys.empty();

  for (std::size_t ordinal = 1; ordinal <= requestCount; ++ordinal) {
    SCOPED_TRACE(blueprint.name);
    SCOPED_TRACE(route.name);
    SCOPED_TRACE(ordinal);
    AllocationState const outerBaseline = runtime.allocator.state();
    {
      PreparedContainer source(context, blueprint);
      ASSERT_TRUE(source.prepare());
      LocalValue preexisting(context);
      if (!blueprint.enumerableKeys.empty()) {
        preexisting = source.valueAt(0);
        ASSERT_FALSE(preexisting.isException());
      }

      std::size_t const rejectionsBefore = runtime.allocator.rejections;
      runtime.allocator.rejectRelative(ordinal);
      LocalValue failed(callRoute(context, function.get(), source.target()));
      runtime.allocator.disarm();
      bool const rejected =
          runtime.allocator.rejections == rejectionsBefore + 1;
      everyRejected = everyRejected && rejected;
      EXPECT_TRUE(rejected);
      bool const wasException = failed.isException();
      everyOOM = everyOOM && wasException;
      if (!wasException) {
        bool const pending = JS_HasException(context);
        coverage.escapedPendingOOM = true;
        if (pending)
          expectOnePendingOOM(context, ordinal);
        ADD_FAILURE() << blueprint.name << ' ' << route.name
                      << " returned success after rejecting allocator ordinal "
                      << ordinal << " (size="
                      << runtime.allocator.recorded[ordinal - 1].size
                      << ", kind="
                      << static_cast<int>(
                             runtime.allocator.recorded[ordinal - 1].kind)
                      << ", pending_oom=" << pending << ')';
        expectSourceBytes(context, source, blueprint);
        LocalValue retry(callRoute(context, function.get(), source.target()));
        ASSERT_FALSE(retry.isException());
        expectRouteResult(context, source.target(), retry.get(), blueprint,
                          route);
        continue;
      }
      expectOnePendingOOM(context, ordinal);

      expectSourceBytes(context, source, blueprint);
      everySourceStable = everySourceStable && !JS_HasException(context);

      if (!blueprint.enumerableKeys.empty()) {
        LocalValue preexistingAfter(source.valueAt(0));
        ASSERT_FALSE(preexistingAfter.isException());
        bool const preexistingStable = JS_StrictEq(context, preexisting.get(),
                                                   preexistingAfter.get()) == 1;
        everyPreexistingStable = everyPreexistingStable && preexistingStable;
        EXPECT_TRUE(preexistingStable);

        auto const sentinels =
            sentinelOrdinals(blueprint.enumerableKeys.size());
        std::array<LocalValue, 3> afterFailure = {
            LocalValue(context), LocalValue(context), LocalValue(context)};
        for (std::size_t i = 0; i < sentinels.size(); ++i) {
          std::size_t const before = runtime.allocator.requests;
          afterFailure[i] = source.valueAt(sentinels[i]);
          ASSERT_FALSE(afterFailure[i].isException());
          if (runtime.allocator.requests == before)
            sawCompletedInsertSurvivor = true;
        }

        LocalValue retry(callRoute(context, function.get(), source.target()));
        bool const retryComplete = !retry.isException();
        everyRetryComplete = everyRetryComplete && retryComplete;
        ASSERT_TRUE(retryComplete);
        expectRouteResult(context, source.target(), retry.get(), blueprint,
                          route);
        for (std::size_t i = 0; i < sentinels.size(); ++i) {
          LocalValue afterRetry(source.valueAt(sentinels[i]));
          ASSERT_FALSE(afterRetry.isException());
          bool const stable = JS_StrictEq(context, afterFailure[i].get(),
                                          afterRetry.get()) == 1;
          everyCompletedStable = everyCompletedStable && stable;
          EXPECT_TRUE(stable);
        }
      } else {
        LocalValue retry(callRoute(context, function.get(), source.target()));
        bool const retryComplete = !retry.isException();
        everyRetryComplete = everyRetryComplete && retryComplete;
        ASSERT_TRUE(retryComplete);
        expectRouteResult(context, source.target(), retry.get(), blueprint,
                          route);
      }
    }
    expectRuntimeBaseline(runtime, outerBaseline, route.name);
  }

  coverage.everyRequestRejected = everyRejected;
  coverage.exactOOM = everyOOM;
  coverage.sourceStable = everySourceStable;
  coverage.retryComplete = everyRetryComplete;
  coverage.preexistingIdentityStable = everyPreexistingStable;
  coverage.completedInsertIdentityStable =
      everyCompletedStable && sawCompletedInsertSurvivor;
  if (!coverage.escapedPendingOOM)
    EXPECT_TRUE(coverage.complete(!blueprint.enumerableKeys.empty() &&
                                  route.materializesValues));
}

class ObjectReflectionOOM : public ::testing::Test {
protected:
  ReflectionRuntime runtime;

  void SetUp() override { ASSERT_TRUE(runtime.open()); }

  void TearDown() override {
    EXPECT_FALSE(JS_HasException(runtime.context()));
    runtime.close();
    EXPECT_EQ(runtime.allocator.liveBlocks, 0u);
    EXPECT_EQ(runtime.allocator.liveBytes, 0u);
  }
};

TEST_F(ObjectReflectionOOM,
       EmptyAndMaximumOwnNameTablesHaveExactCallbackAndMergedOrdinals) {
  ContainerBlueprint const emptyObject = objectBlueprint("object-empty", 0);
  ContainerBlueprint const maximumObject = maximumObjectBlueprint();
  ContainerBlueprint const maximumArray =
      arrayBlueprint("array-maximum-32767", 32'767);
  ASSERT_EQ(maximumObject.ownKeys.size(), 325u);
  ASSERT_EQ(maximumArray.ownKeys.size(), 32'768u);

  struct Bank {
    ContainerBlueprint const *blueprint;
    bool emptyObject;
  };
  std::array<Bank, 3> const banks = {{
      {&emptyObject, true},
      {&maximumObject, false},
      {&maximumArray, false},
  }};

  JSContext *context = runtime.context();
  constexpr int flags = JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK;
  for (auto const &bank : banks) {
    auto const &blueprint = *bank.blueprint;
    SCOPED_TRACE(blueprint.name);

    AllocationState const outerBaseline = runtime.allocator.state();
    std::vector<AllocationRequest> trace;
    {
      PreparedContainer measured(context, blueprint);
      ASSERT_TRUE(measured.prepare());
      runtime.allocator.startRecording();
      JSPropertyEnum *table = nullptr;
      std::uint32_t length = 0;
      ASSERT_EQ(JS_GetOwnPropertyNames(context, &table, &length,
                                       measured.target(), flags),
                0);
      runtime.allocator.stopRecording();
      ASSERT_LE(runtime.allocator.recordedCount,
                runtime.allocator.recorded.size());
      trace.assign(runtime.allocator.recorded.begin(),
                   runtime.allocator.recorded.begin() +
                       runtime.allocator.recordedCount);
      expectPropertyEnum(context, table, length, blueprint.ownKeys);
      JS_FreePropertyEnum(context, table, length);
    }
    expectRuntimeBaseline(runtime, outerBaseline, blueprint.name.c_str());
    ASSERT_TRUE(exactOwnNameTrace(
        trace, static_cast<std::uint32_t>(blueprint.ownKeys.size()),
        bank.emptyObject));

    for (std::size_t ordinal = 1; ordinal <= trace.size(); ++ordinal) {
      SCOPED_TRACE(ordinal);
      AllocationState const iterationBaseline = runtime.allocator.state();
      {
        PreparedContainer source(context, blueprint);
        ASSERT_TRUE(source.prepare());
        // Source verification is part of the asserted transaction. Stabilize
        // its lazy engine tables before attributing any retained state to the
        // deliberately failed operation below.
        expectSourceBytes(context, source, blueprint);
        AllocationState const sourceState = runtime.allocator.state();
        std::size_t const rejectionsBefore = runtime.allocator.rejections;
        runtime.allocator.rejectRelative(ordinal);
        JSPropertyEnum *failedTable = nullptr;
        std::uint32_t failedLength = 77;
        int const failed = JS_GetOwnPropertyNames(
            context, &failedTable, &failedLength, source.target(), flags);
        runtime.allocator.disarm();
        ASSERT_EQ(runtime.allocator.rejections, rejectionsBefore + 1);
        EXPECT_EQ(failed, -1);
        EXPECT_EQ(failedTable, nullptr);
        EXPECT_EQ(failedLength, 0u);
        expectOnePendingOOM(context, ordinal);
        expectSourceBytes(context, source, blueprint);
        // Neither callback nor engine merge may materialize a value or
        // retain either transient table.
        EXPECT_EQ(runtime.allocator.state(), sourceState);

        LocalValue identity(context);
        if (!blueprint.enumerableKeys.empty()) {
          std::size_t const before = runtime.allocator.requests;
          identity = source.valueAt(0);
          ASSERT_FALSE(identity.isException());
          EXPECT_GT(runtime.allocator.requests, before)
              << "plain own-name failure populated an exotic cache";
        }

        JSPropertyEnum *retryTable = nullptr;
        std::uint32_t retryLength = 0;
        ASSERT_EQ(JS_GetOwnPropertyNames(context, &retryTable, &retryLength,
                                         source.target(), flags),
                  0);
        expectPropertyEnum(context, retryTable, retryLength, blueprint.ownKeys);
        JS_FreePropertyEnum(context, retryTable, retryLength);
        if (!blueprint.enumerableKeys.empty()) {
          LocalValue after(source.valueAt(0));
          ASSERT_FALSE(after.isException());
          EXPECT_EQ(JS_StrictEq(context, identity.get(), after.get()), 1);
        }
        EXPECT_FALSE(JS_HasException(context));
      }
      expectRuntimeBaseline(runtime, iterationBaseline, blueprint.name.c_str());
    }
  }
}

TEST_F(
    ObjectReflectionOOM,
    SetEnumEveryRealRequestRollsBackAndMergedFailureReusesAllInsertedValues) {
  ContainerBlueprint const denseObject = objectBlueprint("object-dense-9", 9);
  ContainerBlueprint const denseArray = arrayBlueprint("array-dense-65", 65);
  std::array<ContainerBlueprint const *, 2> const banks = {&denseObject,
                                                           &denseArray};
  JSContext *context = runtime.context();
  constexpr int flags = JS_GPN_STRING_MASK | JS_GPN_SET_ENUM;

  for (auto const *blueprintPointer : banks) {
    auto const &blueprint = *blueprintPointer;
    SCOPED_TRACE(blueprint.name);

    // Warm all materializer classes and cache layouts, then discard the
    // source so the measured source itself starts with no value cache.
    {
      PreparedContainer warm(context, blueprint);
      ASSERT_TRUE(warm.prepare());
      JSPropertyEnum *table = nullptr;
      std::uint32_t length = 0;
      ASSERT_EQ(JS_GetOwnPropertyNames(context, &table, &length, warm.target(),
                                       flags),
                0);
      expectPropertyEnum(context, table, length, blueprint.ownKeys);
      JS_FreePropertyEnum(context, table, length);
    }
    JS_RunGC(runtime.runtime());
    AllocationState const warmedBaseline = runtime.allocator.state();

    std::vector<AllocationRequest> trace;
    {
      PreparedContainer measured(context, blueprint);
      ASSERT_TRUE(measured.prepare());
      runtime.allocator.startRecording();
      JSPropertyEnum *table = nullptr;
      std::uint32_t length = 0;
      ASSERT_EQ(JS_GetOwnPropertyNames(context, &table, &length,
                                       measured.target(), flags),
                0);
      runtime.allocator.stopRecording();
      ASSERT_LE(runtime.allocator.recordedCount,
                runtime.allocator.recorded.size());
      trace.assign(runtime.allocator.recorded.begin(),
                   runtime.allocator.recorded.begin() +
                       runtime.allocator.recordedCount);
      expectPropertyEnum(context, table, length, blueprint.ownKeys);
      JS_FreePropertyEnum(context, table, length);
    }
    expectRuntimeBaseline(runtime, warmedBaseline, blueprint.name.c_str());
    ASSERT_GT(trace.size(), 2u);
    std::size_t const callbackBytes =
        blueprint.ownKeys.size() * sizeof(JSPropertyEnum);
    std::size_t const mergedBytes =
        blueprint.ownKeys.size() * sizeof(JSPropertyEnum);
    EXPECT_EQ(trace.front(),
              (AllocationRequest{callbackBytes, RequestKind::allocate}));
    EXPECT_EQ(trace.back(),
              (AllocationRequest{mergedBytes, RequestKind::allocate}));

    auto const sentinels = sentinelOrdinals(blueprint.enumerableKeys.size());
    for (std::size_t ordinal = 1; ordinal <= trace.size(); ++ordinal) {
      SCOPED_TRACE(ordinal);
      AllocationState const iterationBaseline = runtime.allocator.state();
      {
        PreparedContainer source(context, blueprint);
        ASSERT_TRUE(source.prepare());
        std::size_t const rejectionsBefore = runtime.allocator.rejections;
        runtime.allocator.rejectRelative(ordinal);
        JSPropertyEnum *failedTable = nullptr;
        std::uint32_t failedLength = 99;
        int const failed = JS_GetOwnPropertyNames(
            context, &failedTable, &failedLength, source.target(), flags);
        runtime.allocator.disarm();
        ASSERT_EQ(runtime.allocator.rejections, rejectionsBefore + 1);
        EXPECT_EQ(failed, -1);
        EXPECT_EQ(failedTable, nullptr);
        EXPECT_EQ(failedLength, 0u);
        expectOnePendingOOM(context, ordinal);
        expectSourceBytes(context, source, blueprint);

        std::array<LocalValue, 3> afterFailure = {
            LocalValue(context), LocalValue(context), LocalValue(context)};
        for (std::size_t i = 0; i < sentinels.size(); ++i) {
          std::size_t const before = runtime.allocator.requests;
          afterFailure[i] = source.valueAt(sentinels[i]);
          ASSERT_FALSE(afterFailure[i].isException());
          if (ordinal == 1)
            EXPECT_GT(runtime.allocator.requests, before)
                << "callback-table OOM published a cache value";
          if (ordinal == trace.size())
            EXPECT_EQ(runtime.allocator.requests, before)
                << "merged-table OOM lost a complete cache value";
        }

        JSPropertyEnum *retryTable = nullptr;
        std::uint32_t retryLength = 0;
        ASSERT_EQ(JS_GetOwnPropertyNames(context, &retryTable, &retryLength,
                                         source.target(), flags),
                  0);
        expectPropertyEnum(context, retryTable, retryLength, blueprint.ownKeys);
        JS_FreePropertyEnum(context, retryTable, retryLength);
        for (std::size_t i = 0; i < sentinels.size(); ++i) {
          LocalValue afterRetry(source.valueAt(sentinels[i]));
          ASSERT_FALSE(afterRetry.isException());
          EXPECT_EQ(
              JS_StrictEq(context, afterFailure[i].get(), afterRetry.get()), 1);
        }
      }
      expectRuntimeBaseline(runtime, iterationBaseline, blueprint.name.c_str());
    }
  }
}

[[nodiscard]] JSAtom propertyAtom(JSContext *context,
                                  ContainerBlueprint const &blueprint,
                                  std::size_t ordinal) {
  if (blueprint.kind == ContainerKind::array)
    return JS_NewAtomUInt32(context, static_cast<std::uint32_t>(ordinal));
  return JS_NewAtom(context, blueprint.enumerableKeys[ordinal].c_str());
}

void freeDescriptor(JSContext *context, JSPropertyDescriptor &descriptor) {
  JS_FreeValue(context, descriptor.value);
  JS_FreeValue(context, descriptor.getter);
  JS_FreeValue(context, descriptor.setter);
  descriptor = {
      .flags = 0,
      .value = JS_UNDEFINED,
      .getter = JS_UNDEFINED,
      .setter = JS_UNDEFINED,
  };
}

TEST_F(ObjectReflectionOOM,
       ExoticDescriptorCallbacksAreAtomicAtFirstMiddleAndLast) {
  ContainerBlueprint const denseObject = objectBlueprint("object-dense-9", 9);
  ContainerBlueprint const denseArray = arrayBlueprint("array-dense-65", 65);
  std::array<ContainerBlueprint const *, 2> const banks = {&denseObject,
                                                           &denseArray};
  JSContext *context = runtime.context();

  for (auto const *blueprintPointer : banks) {
    auto const &blueprint = *blueprintPointer;
    for (std::size_t position :
         sentinelOrdinals(blueprint.enumerableKeys.size())) {
      SCOPED_TRACE(blueprint.name);
      SCOPED_TRACE(position);
      JSAtom const atom = propertyAtom(context, blueprint, position);
      ASSERT_NE(atom, JS_ATOM_NULL);

      // Warm the selected materializer and exact object/radix path on a
      // disposable source before recording a fresh miss.
      {
        PreparedContainer warm(context, blueprint);
        ASSERT_TRUE(warm.prepare());
        JSPropertyDescriptor descriptor{
            .flags = 0,
            .value = JS_UNDEFINED,
            .getter = JS_UNDEFINED,
            .setter = JS_UNDEFINED,
        };
        ASSERT_EQ(JS_GetOwnProperty(context, &descriptor, warm.target(), atom),
                  1);
        freeDescriptor(context, descriptor);
      }
      JS_RunGC(runtime.runtime());
      AllocationState const warmedBaseline = runtime.allocator.state();

      std::size_t requestCount = 0;
      {
        PreparedContainer measured(context, blueprint);
        ASSERT_TRUE(measured.prepare());
        JSPropertyDescriptor descriptor{
            .flags = 0,
            .value = JS_UNDEFINED,
            .getter = JS_UNDEFINED,
            .setter = JS_UNDEFINED,
        };
        std::size_t const before = runtime.allocator.requests;
        ASSERT_EQ(
            JS_GetOwnProperty(context, &descriptor, measured.target(), atom),
            1);
        requestCount = runtime.allocator.requests - before;
        EXPECT_EQ(descriptor.flags, JS_PROP_ENUMERABLE);
        freeDescriptor(context, descriptor);
      }
      ASSERT_GT(requestCount, 0u);
      expectRuntimeBaseline(runtime, warmedBaseline, blueprint.name.c_str());

      for (std::size_t ordinal = 1; ordinal <= requestCount; ++ordinal) {
        SCOPED_TRACE(ordinal);
        AllocationState const iterationBaseline = runtime.allocator.state();
        {
          PreparedContainer source(context, blueprint);
          ASSERT_TRUE(source.prepare());
          expectSourceBytes(context, source, blueprint);
          AllocationState const sourceState = runtime.allocator.state();
          JSPropertyDescriptor failedDescriptor{
              .flags = 0,
              .value = JS_UNDEFINED,
              .getter = JS_UNDEFINED,
              .setter = JS_UNDEFINED,
          };
          std::size_t const rejectionsBefore = runtime.allocator.rejections;
          runtime.allocator.rejectRelative(ordinal);
          int const failed = JS_GetOwnProperty(context, &failedDescriptor,
                                               source.target(), atom);
          runtime.allocator.disarm();
          ASSERT_EQ(runtime.allocator.rejections, rejectionsBefore + 1);
          EXPECT_EQ(failed, -1);
          EXPECT_EQ(failedDescriptor.flags, 0);
          EXPECT_TRUE(JS_IsUndefined(failedDescriptor.value));
          EXPECT_TRUE(JS_IsUndefined(failedDescriptor.getter));
          EXPECT_TRUE(JS_IsUndefined(failedDescriptor.setter));
          expectOnePendingOOM(context, ordinal);
          expectSourceBytes(context, source, blueprint);
          EXPECT_EQ(runtime.allocator.state(), sourceState)
              << "descriptor callback published a half cache";

          JSPropertyDescriptor retryDescriptor{
              .flags = 0,
              .value = JS_UNDEFINED,
              .getter = JS_UNDEFINED,
              .setter = JS_UNDEFINED,
          };
          ASSERT_EQ(JS_GetOwnProperty(context, &retryDescriptor,
                                      source.target(), atom),
                    1);
          EXPECT_EQ(retryDescriptor.flags, JS_PROP_ENUMERABLE);
          LocalValue observed(source.valueAt(position));
          ASSERT_FALSE(observed.isException());
          EXPECT_EQ(JS_StrictEq(context, retryDescriptor.value, observed.get()),
                    1);
          freeDescriptor(context, retryDescriptor);
        }
        expectRuntimeBaseline(runtime, iterationBaseline,
                              blueprint.name.c_str());
      }
      JS_FreeAtom(context, atom);
    }
  }

  // Empty object absence and STArray.length are immediate callback routes:
  // neither owns a real allocator ordinal.
  {
    ContainerBlueprint const emptyObject = objectBlueprint("object-empty", 0);
    PreparedContainer source(context, emptyObject);
    ASSERT_TRUE(source.prepare());
    JSAtom const atom = JS_NewAtom(context, "Flags");
    ASSERT_NE(atom, JS_ATOM_NULL);
    JSPropertyDescriptor descriptor{
        .flags = 0,
        .value = JS_UNDEFINED,
        .getter = JS_UNDEFINED,
        .setter = JS_UNDEFINED,
    };
    std::size_t const before = runtime.allocator.requests;
    EXPECT_EQ(JS_GetOwnProperty(context, &descriptor, source.target(), atom),
              0);
    EXPECT_EQ(runtime.allocator.requests, before);
    freeDescriptor(context, descriptor);
    JS_FreeAtom(context, atom);
  }
  {
    ContainerBlueprint const array = arrayBlueprint("array-dense-65", 65);
    PreparedContainer source(context, array);
    ASSERT_TRUE(source.prepare());
    JSAtom const atom = JS_NewAtom(context, "length");
    ASSERT_NE(atom, JS_ATOM_NULL);
    JSPropertyDescriptor descriptor{
        .flags = 0,
        .value = JS_UNDEFINED,
        .getter = JS_UNDEFINED,
        .setter = JS_UNDEFINED,
    };
    std::size_t const before = runtime.allocator.requests;
    ASSERT_EQ(JS_GetOwnProperty(context, &descriptor, source.target(), atom),
              1);
    EXPECT_EQ(runtime.allocator.requests, before);
    EXPECT_EQ(descriptor.flags, 0);
    std::uint32_t length = 0;
    ASSERT_EQ(JS_ToUint32(context, &length, descriptor.value), 0);
    EXPECT_EQ(length, 65u);
    freeDescriptor(context, descriptor);
    JS_FreeAtom(context, atom);
  }
}

[[nodiscard]] LocalValue descriptorKey(JSContext *context,
                                       ContainerBlueprint const &blueprint,
                                       std::size_t ordinal,
                                       bool length = false) {
  if (length)
    return LocalValue(context, JS_NewString(context, "length"));
  if (blueprint.kind == ContainerKind::array)
    return LocalValue(
        context, JS_NewUint32(context, static_cast<std::uint32_t>(ordinal)));
  return LocalValue(
      context,
      JS_NewString(context, blueprint.enumerableKeys[ordinal].c_str()));
}

void expectLengthDescriptor(JSContext *context, JSValueConst descriptor,
                            std::uint32_t expectedLength) {
  ASSERT_TRUE(JS_IsObject(descriptor));
  LocalValue value(context, JS_GetPropertyStr(context, descriptor, "value"));
  LocalValue writable(context,
                      JS_GetPropertyStr(context, descriptor, "writable"));
  LocalValue enumerable(context,
                        JS_GetPropertyStr(context, descriptor, "enumerable"));
  LocalValue configurable(
      context, JS_GetPropertyStr(context, descriptor, "configurable"));
  std::uint32_t actualLength = 0;
  ASSERT_FALSE(value.isException());
  ASSERT_EQ(JS_ToUint32(context, &actualLength, value.get()), 0);
  EXPECT_EQ(actualLength, expectedLength);
  EXPECT_FALSE(JS_ToBool(context, writable.get()));
  EXPECT_FALSE(JS_ToBool(context, enumerable.get()));
  EXPECT_FALSE(JS_ToBool(context, configurable.get()));
}

void sweepSingularDescriptorEnvelope(ReflectionRuntime &runtime,
                                     ContainerBlueprint const &blueprint,
                                     std::size_t position, bool length) {
  JSContext *context = runtime.context();
  LocalValue function(runtime.route("descriptor"));
  ASSERT_TRUE(JS_IsFunction(context, function.get()));
  LocalValue key(descriptorKey(context, blueprint, position, length));
  ASSERT_FALSE(key.isException());
  auto call = [&](JSValueConst target) {
    JSValueConst arguments[] = {target, key.get()};
    return LocalValue(
        context, JS_Call(context, function.get(), JS_UNDEFINED, 2, arguments));
  };

  {
    PreparedContainer warm(context, blueprint);
    ASSERT_TRUE(warm.prepare());
    LocalValue cached(context);
    if (!length) {
      cached = warm.valueAt(position);
      ASSERT_FALSE(cached.isException());
    }
    LocalValue result(call(warm.target()));
    ASSERT_FALSE(result.isException());
    if (length)
      expectLengthDescriptor(
          context, result.get(),
          static_cast<std::uint32_t>(blueprint.enumerableKeys.size()));
    else
      expectDescriptor(context, result.get(), warm.target(), blueprint,
                       position);
  }
  JS_RunGC(runtime.runtime());
  AllocationState const warmedBaseline = runtime.allocator.state();

  std::size_t requestCount = 0;
  {
    PreparedContainer measured(context, blueprint);
    ASSERT_TRUE(measured.prepare());
    LocalValue cached(context);
    if (!length) {
      cached = measured.valueAt(position);
      ASSERT_FALSE(cached.isException());
    }
    runtime.allocator.startRecording();
    std::size_t const before = runtime.allocator.requests;
    LocalValue result(call(measured.target()));
    requestCount = runtime.allocator.requests - before;
    runtime.allocator.stopRecording();
    ASSERT_FALSE(result.isException());
    ASSERT_EQ(runtime.allocator.recordedCount, requestCount);
    if (length)
      expectLengthDescriptor(
          context, result.get(),
          static_cast<std::uint32_t>(blueprint.enumerableKeys.size()));
    else
      expectDescriptor(context, result.get(), measured.target(), blueprint,
                       position);
  }
  ASSERT_GT(requestCount, 0u);
  // Object + value/writable/enumerable/configurable definition envelopes
  // must expose more than a single undifferentiated request.
  EXPECT_GE(requestCount, 5u);
  expectRuntimeBaseline(runtime, warmedBaseline, "singular descriptor");

  for (std::size_t ordinal = 1; ordinal <= requestCount; ++ordinal) {
    SCOPED_TRACE(blueprint.name);
    SCOPED_TRACE(position);
    SCOPED_TRACE(ordinal);
    AllocationState const iterationBaseline = runtime.allocator.state();
    {
      PreparedContainer source(context, blueprint);
      ASSERT_TRUE(source.prepare());
      LocalValue identity(context);
      if (!length) {
        identity = source.valueAt(position);
        ASSERT_FALSE(identity.isException());
      }
      expectSourceBytes(context, source, blueprint);
      AllocationState const sourceState = runtime.allocator.state();
      std::size_t const rejectionsBefore = runtime.allocator.rejections;
      runtime.allocator.rejectRelative(ordinal);
      LocalValue failed(call(source.target()));
      runtime.allocator.disarm();
      ASSERT_EQ(runtime.allocator.rejections, rejectionsBefore + 1);
      ASSERT_TRUE(failed.isException());
      expectOnePendingOOM(context, ordinal);
      expectSourceBytes(context, source, blueprint);
      EXPECT_EQ(runtime.allocator.state(), sourceState)
          << "singular descriptor leaked a partial ordinary envelope";
      if (!length) {
        LocalValue after(source.valueAt(position));
        ASSERT_FALSE(after.isException());
        EXPECT_EQ(JS_StrictEq(context, identity.get(), after.get()), 1);
      }

      LocalValue retry(call(source.target()));
      ASSERT_FALSE(retry.isException());
      if (length)
        expectLengthDescriptor(
            context, retry.get(),
            static_cast<std::uint32_t>(blueprint.enumerableKeys.size()));
      else
        expectDescriptor(context, retry.get(), source.target(), blueprint,
                         position);
    }
    expectRuntimeBaseline(runtime, iterationBaseline, "singular descriptor");
  }
}

TEST_F(ObjectReflectionOOM,
       SingularDescriptorEngineEnvelopeFailsAtEveryRealAllocation) {
  ContainerBlueprint const denseObject = objectBlueprint("object-dense-9", 9);
  ContainerBlueprint const denseArray = arrayBlueprint("array-dense-65", 65);
  for (std::size_t position :
       sentinelOrdinals(denseObject.enumerableKeys.size()))
    sweepSingularDescriptorEnvelope(runtime, denseObject, position, false);
  for (std::size_t position :
       sentinelOrdinals(denseArray.enumerableKeys.size()))
    sweepSingularDescriptorEnvelope(runtime, denseArray, position, false);
  sweepSingularDescriptorEnvelope(runtime, denseArray, 0, true);
}

TEST_F(ObjectReflectionOOM,
       EveryHighLevelReflectionAndCopyRequestFailsAtomicallyAndRetries) {
  std::array<ContainerBlueprint, 6> const blueprints = {
      objectBlueprint("object-empty", 0),
      objectBlueprint("object-one-key", 1),
      objectBlueprint("object-dense-9", 9),
      arrayBlueprint("array-empty", 0),
      arrayBlueprint("array-one-key", 1),
      arrayBlueprint("array-dense-9", 9),
  };
  std::array<std::array<RouteCoverage, reflectionRoutes.size()>,
             blueprints.size()>
      coverage{};
  for (std::size_t bank = 0; bank < blueprints.size(); ++bank) {
    for (std::size_t route = 0; route < reflectionRoutes.size(); ++route) {
      sweepEveryRouteAllocation(runtime, blueprints[bank],
                                reflectionRoutes[route], coverage[bank][route]);
      if (!coverage[bank][route].escapedPendingOOM)
        EXPECT_TRUE(coverage[bank][route].complete(
            !blueprints[bank].enumerableKeys.empty() &&
            reflectionRoutes[route].materializesValues));
    }
  }
}

void exerciseSelectedMaximumRouteFailures(ReflectionRuntime &runtime,
                                          ContainerBlueprint const &blueprint,
                                          ReflectionRoute const &route) {
  JSContext *context = runtime.context();
  LocalValue function(runtime.route(route.name));
  ASSERT_TRUE(JS_IsFunction(context, function.get()));

  // Maximum-density shapes are warmed separately: otherwise the first clean
  // measurement would include persistent engine shape transitions that a
  // later same-runtime source correctly reuses.
  {
    PreparedContainer warm(context, blueprint);
    ASSERT_TRUE(warm.prepare());
    LocalValue result(callRoute(context, function.get(), warm.target()));
    ASSERT_FALSE(result.isException());
    expectRouteResult(context, warm.target(), result.get(), blueprint, route);
  }
  JS_RunGC(runtime.runtime());
  AllocationState const warmedBaseline = runtime.allocator.state();

  std::size_t requestCount = 0;
  {
    PreparedContainer measured(context, blueprint);
    ASSERT_TRUE(measured.prepare());
    LocalValue preexisting(context);
    if (!blueprint.enumerableKeys.empty()) {
      preexisting = measured.valueAt(0);
      ASSERT_FALSE(preexisting.isException());
    }
    std::size_t const before = runtime.allocator.requests;
    LocalValue result(callRoute(context, function.get(), measured.target()));
    requestCount = runtime.allocator.requests - before;
    ASSERT_FALSE(result.isException());
    expectRouteResult(context, measured.target(), result.get(), blueprint,
                      route);
  }
  ASSERT_GT(requestCount, 0u);
  expectRuntimeBaseline(runtime, warmedBaseline, route.name);

  std::array<std::size_t, 3> const selected = {1, (requestCount + 1) / 2,
                                               requestCount};
  for (std::size_t selection = 0; selection < selected.size(); ++selection) {
    std::size_t const ordinal = selected[selection];
    if (selection != 0 && ordinal == selected[selection - 1])
      continue;
    SCOPED_TRACE(blueprint.name);
    SCOPED_TRACE(route.name);
    SCOPED_TRACE(ordinal);
    AllocationState const iterationBaseline = runtime.allocator.state();
    {
      PreparedContainer source(context, blueprint);
      ASSERT_TRUE(source.prepare());
      LocalValue preexisting(context);
      if (!blueprint.enumerableKeys.empty()) {
        preexisting = source.valueAt(0);
        ASSERT_FALSE(preexisting.isException());
      }
      std::size_t const rejectionsBefore = runtime.allocator.rejections;
      runtime.allocator.rejectRelative(ordinal);
      LocalValue failed(callRoute(context, function.get(), source.target()));
      runtime.allocator.disarm();
      ASSERT_EQ(runtime.allocator.rejections, rejectionsBefore + 1);
      ASSERT_TRUE(failed.isException());
      expectOnePendingOOM(context, ordinal);
      expectSourceBytes(context, source, blueprint);

      std::array<LocalValue, 3> survivor = {
          LocalValue(context), LocalValue(context), LocalValue(context)};
      auto const sentinels = sentinelOrdinals(blueprint.enumerableKeys.size());
      if (!blueprint.enumerableKeys.empty()) {
        LocalValue firstAfter(source.valueAt(0));
        ASSERT_FALSE(firstAfter.isException());
        EXPECT_EQ(JS_StrictEq(context, preexisting.get(), firstAfter.get()), 1);
        for (std::size_t i = 0; i < sentinels.size(); ++i) {
          survivor[i] = source.valueAt(sentinels[i]);
          ASSERT_FALSE(survivor[i].isException());
        }
      }

      LocalValue retry(callRoute(context, function.get(), source.target()));
      ASSERT_FALSE(retry.isException());
      expectRouteResult(context, source.target(), retry.get(), blueprint,
                        route);
      if (!blueprint.enumerableKeys.empty()) {
        for (std::size_t i = 0; i < sentinels.size(); ++i) {
          LocalValue after(source.valueAt(sentinels[i]));
          ASSERT_FALSE(after.isException());
          EXPECT_EQ(JS_StrictEq(context, survivor[i].get(), after.get()), 1);
        }
      }
    }
    expectRuntimeBaseline(runtime, iterationBaseline, route.name);
  }
}

TEST_F(ObjectReflectionOOM,
       MaximumDensityAggregateAndResultEnvelopesFailFirstMiddleLast) {
  ContainerBlueprint const maximumObject = maximumObjectBlueprint();
  ContainerBlueprint const maximumArray =
      arrayBlueprint("array-maximum-32767", 32'767);
  ASSERT_EQ(maximumObject.enumerableKeys.size(), 325u);
  ASSERT_EQ(maximumArray.enumerableKeys.size(), 32'767u);

  // The exact callback/merged maximum tables are injected exhaustively in
  // the direct bank above. Here the high-level ordinary envelopes are hit at
  // the first, midpoint, and final allocator request at both reachable maxima.
  for (auto const &route : reflectionRoutes) {
    exerciseSelectedMaximumRouteFailures(runtime, maximumObject, route);
    exerciseSelectedMaximumRouteFailures(runtime, maximumArray, route);
  }
}

TEST(ObjectReflectionOOMRedControls, RejectsRemovedOrWeakenedExpectedRoutes) {
  std::array<AllocationRequest, 1> const emptyGreen = {{
      {sizeof(JSPropertyEnum), RequestKind::allocate},
  }};
  EXPECT_TRUE(exactOwnNameTrace(emptyGreen, 0, true));

  std::array<AllocationRequest, 0> const removedEmpty{};
  EXPECT_FALSE(exactOwnNameTrace(removedEmpty, 0, true));

  std::array<AllocationRequest, 2> maximumGreen = {{
      {325 * sizeof(JSPropertyEnum), RequestKind::allocate},
      {325 * sizeof(JSPropertyEnum), RequestKind::allocate},
  }};
  EXPECT_TRUE(exactOwnNameTrace(maximumGreen, 325, false));
  maximumGreen[0].size -= sizeof(JSPropertyEnum);
  EXPECT_FALSE(exactOwnNameTrace(maximumGreen, 325, false));
  maximumGreen[0].size += sizeof(JSPropertyEnum);
  maximumGreen[1].kind = RequestKind::reallocate;
  EXPECT_FALSE(exactOwnNameTrace(maximumGreen, 325, false));

  RouteCoverage complete{
      .measured = true,
      .everyRequestRejected = true,
      .exactOOM = true,
      .sourceStable = true,
      .retryComplete = true,
      .preexistingIdentityStable = true,
      .completedInsertIdentityStable = true,
      .escapedPendingOOM = false,
  };
  ASSERT_TRUE(complete.complete(true));
  std::array<bool RouteCoverage::*, 7> const required = {
      &RouteCoverage::measured,
      &RouteCoverage::everyRequestRejected,
      &RouteCoverage::exactOOM,
      &RouteCoverage::sourceStable,
      &RouteCoverage::retryComplete,
      &RouteCoverage::preexistingIdentityStable,
      &RouteCoverage::completedInsertIdentityStable,
  };
  for (auto member : required) {
    RouteCoverage weakened = complete;
    weakened.*member = false;
    EXPECT_FALSE(weakened.complete(true));
  }
  RouteCoverage escaped = complete;
  escaped.escapedPendingOOM = true;
  EXPECT_FALSE(escaped.complete(true));
}

} // namespace
