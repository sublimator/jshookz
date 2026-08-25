#include "js.hpp"
#include "leaf/leaf.hpp"
#include "object/object.hpp"
#include "result.hpp"

#include <catl/xdata/recursive_index.h>
#include <jshookz/qjs.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <vector>

extern "C" bool register_uint_types(JSContext *ctx);

namespace {

namespace qjs = jshookz::qjs;
namespace types = jshookz::provider::types;

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

struct AllocatorControl {
  static constexpr std::size_t maxRecordedRequests = 256;

  std::size_t requests = 0;
  std::size_t rejectAt = 0;
  std::size_t rejections = 0;
  std::size_t liveBlocks = 0;
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

class Runtime {
public:
  ~Runtime() { close(); }

  Runtime(Runtime const &) = delete;
  Runtime &operator=(Runtime const &) = delete;

  Runtime() = default;

  [[nodiscard]] bool open() {
    runtime_ = JS_NewRuntime2(&testAllocator, &allocations);
    if (runtime_ == nullptr)
      return false;
    context_ = JS_NewContext(runtime_);
    if (context_ == nullptr)
      return false;
    if (!jshookz::provider::bindings::registerResult(context_))
      return false;
    jshookz::provider::qjs::resetByteClassRegistry();
    qjs::OwnedValue global(context_, JS_GetGlobalObject(context_));
    return !global.isException() &&
           types::registerSTBlob(context_, global.get()) &&
           types::registerHash256(context_, global.get()) &&
           types::registerAccountID(context_, global.get()) &&
           types::registerXFL(context_) &&
           types::registerRichLeafTypes(context_) &&
           types::registerObjectTypes(context_) &&
           register_uint_types(context_) && !JS_HasException(context_);
  }

  void close() {
    if (context_ != nullptr) {
      JS_FreeContext(context_);
      context_ = nullptr;
    }
    if (runtime_ != nullptr) {
      types::unregisterObjectTypes(runtime_);
      types::unregisterRichLeafTypes(runtime_);
      jshookz::provider::qjs::resetByteClassRegistry();
      JS_FreeRuntime(runtime_);
      runtime_ = nullptr;
    }
  }

  [[nodiscard]] JSContext *context() const noexcept { return context_; }

  AllocatorControl allocations;

private:
  JSRuntime *runtime_ = nullptr;
  JSContext *context_ = nullptr;
};

constexpr std::uint32_t sourceFieldCount = 40;

[[nodiscard]] std::vector<std::uint8_t> sourceWire() {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(225);
  for (std::uint8_t nth = 1; nth <= sourceFieldCount; ++nth) {
    if (nth < 16) {
      bytes.push_back(static_cast<std::uint8_t>(0x20U | nth));
    } else {
      bytes.push_back(0x20);
      bytes.push_back(nth);
    }
    bytes.push_back(0);
    bytes.push_back(0);
    bytes.push_back(0);
    bytes.push_back(nth);
  }
  return bytes;
}

constexpr std::uint32_t certifiedArrayElements = 20;

[[nodiscard]] std::vector<std::uint8_t> certifiedArrayPayload() {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(certifiedArrayElements * 2 + 1);
  for (std::uint32_t i = 0; i < certifiedArrayElements; ++i) {
    bytes.push_back(0xEA);
    bytes.push_back(0xE1);
  }
  bytes.push_back(0xF1);
  return bytes;
}

[[nodiscard]] std::vector<std::uint8_t> carrierWire() {
  auto payload = certifiedArrayPayload();
  payload.insert(payload.begin(), 0xF9);
  return payload;
}

[[nodiscard]] std::vector<std::uint8_t> cappedObjectCarrierWire() {
  constexpr std::uint32_t selectedMaximumDepth = 10;
  std::vector<std::uint8_t> bytes(selectedMaximumDepth, 0xEA);
  bytes.insert(bytes.end(), selectedMaximumDepth, 0xE1);
  return bytes;
}

class LocalValue {
public:
  explicit LocalValue(JSContext *context, JSValue value = JS_UNDEFINED)
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

[[nodiscard]] bool copyContiguousBytes(JSContext *ctx, JSValueConst value,
                                       std::vector<std::uint8_t> &output) {
  JSValue backing = JS_UNDEFINED;
  std::uint8_t const *data = nullptr;
  std::size_t size = 0;
  auto status = JS_GetObjectByteSpanNoThrow(ctx, value, &backing, &data, &size);
  if (status == JS_OBJECT_BYTES_WRONG_KIND) {
    status =
        types::getSTBlobByteSpanNoThrow(ctx, value, &backing, &data, &size);
  }
  if (status != JS_OBJECT_BYTES_OK) {
    JS_FreeValue(ctx, backing);
    return false;
  }
  output.assign(data, data + size);
  JS_FreeValue(ctx, backing);
  return true;
}

[[nodiscard]] bool snapshotObject(JSContext *ctx, JSValueConst object,
                                  std::vector<std::uint8_t> &output) {
  LocalValue method(ctx, JS_GetPropertyStr(ctx, object, "toBytes"));
  if (method.isException())
    return false;
  LocalValue bytes(ctx, JS_Call(ctx, method.get(), object, 0, nullptr));
  return !bytes.isException() && copyContiguousBytes(ctx, bytes.get(), output);
}

[[nodiscard]] bool snapshotField(JSContext *ctx, JSValueConst object,
                                 std::uint32_t fieldCode,
                                 std::vector<std::uint8_t> &output) {
  LocalValue method(ctx, JS_GetPropertyStr(ctx, object, "fieldBytes"));
  LocalValue field(ctx, JS_NewUint32(ctx, fieldCode));
  if (method.isException())
    return false;
  JSValueConst arguments[] = {field.get()};
  LocalValue bytes(ctx, JS_Call(ctx, method.get(), object, 1, arguments));
  return !bytes.isException() && copyContiguousBytes(ctx, bytes.get(), output);
}

[[nodiscard]] std::string stringProperty(JSContext *ctx, JSValueConst object,
                                         char const *name) {
  LocalValue value(ctx, JS_GetPropertyStr(ctx, object, name));
  if (value.isException())
    return {};
  char const *text = JS_ToCString(ctx, value.get());
  if (text == nullptr)
    return {};
  std::string output(text);
  JS_FreeCString(ctx, text);
  return output;
}

void expectExactOOM(JSContext *ctx, JSValueConst failed, std::size_t ordinal) {
  ASSERT_TRUE(JS_IsException(failed)) << "allocator ordinal " << ordinal;
  ASSERT_TRUE(JS_HasException(ctx)) << "allocator ordinal " << ordinal;
  LocalValue exception(ctx, JS_GetException(ctx));
  ASSERT_TRUE(JS_IsError(ctx, exception.get()))
      << "allocator ordinal " << ordinal;
  EXPECT_EQ(stringProperty(ctx, exception.get(), "name"), "InternalError")
      << "allocator ordinal " << ordinal;
  EXPECT_EQ(stringProperty(ctx, exception.get(), "message"), "out of memory")
      << "allocator ordinal " << ordinal;
  EXPECT_FALSE(JS_HasException(ctx)) << "allocator ordinal " << ordinal;
}

void warmExactOOMDiagnostic(JSContext *ctx) {
  // New public names can move QuickJS's runtime-owned diagnostic tables across
  // a lazy growth boundary. Stabilize that unrelated state before measuring
  // whether the failed private radix transaction rolls every byte back.
  LocalValue warm(ctx, JS_ThrowOutOfMemory(ctx));
  expectExactOOM(ctx, warm.get(), 0);
  warm.reset();
}

void expectExactTypeError(JSContext *ctx, JSValueConst failed,
                          char const *message, char const *route) {
  ASSERT_TRUE(JS_IsException(failed)) << route;
  ASSERT_TRUE(JS_HasException(ctx)) << route;
  LocalValue exception(ctx, JS_GetException(ctx));
  ASSERT_TRUE(JS_IsError(ctx, exception.get())) << route;
  EXPECT_EQ(stringProperty(ctx, exception.get(), "name"), "TypeError") << route;
  EXPECT_EQ(stringProperty(ctx, exception.get(), "message"), message) << route;
  EXPECT_FALSE(JS_HasException(ctx)) << route;
}

enum class ScenarioKind : std::uint8_t {
  rawLeaf,
  certifiedObject,
  certifiedArray,
  richNominal,
  numberString,
  presentRemoval,
  absentRemoval,
};

[[nodiscard]] char const *scenarioName(ScenarioKind kind) noexcept {
  switch (kind) {
  case ScenarioKind::rawLeaf:
    return "raw leaf withField";
  case ScenarioKind::certifiedObject:
    return "certified STObject withField";
  case ScenarioKind::certifiedArray:
    return "certified STArray withField";
  case ScenarioKind::richNominal:
    return "rich nominal withField";
  case ScenarioKind::numberString:
    return "Number string withField";
  case ScenarioKind::presentRemoval:
    return "present withoutField";
  case ScenarioKind::absentRemoval:
    return "absent withoutField";
  }
  return "unknown";
}

struct ExpectedProfile {
  std::uint32_t outputSize = 0;
  std::uint32_t fields = 0;
  std::uint32_t scopes = 0;
  std::uint32_t fieldScratchCapacity = 0;
  std::uint32_t scopeScratchCapacity = 0;
};

[[nodiscard]] ExpectedProfile expectedProfile(ScenarioKind kind) noexcept {
  switch (kind) {
  case ScenarioKind::rawLeaf:
    return {225, 40, 1, 64, 0};
  case ScenarioKind::certifiedObject:
    return {227, 41, 2, 64, 0};
  case ScenarioKind::certifiedArray:
    return {267, 61, 22, 64, 32};
  case ScenarioKind::richNominal:
    return {247, 41, 1, 64, 0};
  case ScenarioKind::numberString:
    return {238, 41, 1, 64, 0};
  case ScenarioKind::presentRemoval:
    return {220, 39, 1, 64, 0};
  case ScenarioKind::absentRemoval:
    return {};
  }
  return {};
}

class PreparedScenario {
public:
  PreparedScenario(JSContext *context, ScenarioKind scenarioKind)
      : ctx_(context), kind_(scenarioKind), source_(context), method_(context),
        field_(context), argument_(context), argumentIdentity_(context),
        sourceCached_(context), carrier_(context), certifiedArray_(context),
        certifiedObject_(context) {}

  [[nodiscard]] bool prepare() {
    sourceExpected_ = sourceWire();
    source_.reset(types::makeCertifiedObjectCopy(
        ctx_, sourceExpected_.data(),
        static_cast<std::uint32_t>(sourceExpected_.size())));
    if (source_.isException())
      return false;
    sourceCached_.reset(JS_GetPropertyStr(ctx_, source_.get(), "Flags"));
    if (sourceCached_.isException())
      return false;

    char const *methodName = kind_ == ScenarioKind::presentRemoval ||
                                     kind_ == ScenarioKind::absentRemoval
                                 ? "withoutField"
                                 : "withField";
    method_.reset(JS_GetPropertyStr(ctx_, source_.get(), methodName));
    if (method_.isException())
      return false;

    switch (kind_) {
    case ScenarioKind::rawLeaf: {
      fieldCode_ = 0x00020002;
      field_.reset(JS_NewString(ctx_, "Flags"));
      constexpr std::array<std::uint8_t, 4> payload = {0, 0, 0, 0xEE};
      argument_.reset(qjs::uint8Array(ctx_, payload));
      derivedExpected_ = sourceExpected_;
      derivedExpected_[9] = 0xEE;
      break;
    }
    case ScenarioKind::certifiedObject:
    case ScenarioKind::certifiedArray: {
      auto const wire = carrierWire();
      carrier_.reset(types::makeCertifiedObjectCopy(
          ctx_, wire.data(), static_cast<std::uint32_t>(wire.size())));
      if (carrier_.isException())
        return false;
      certifiedArray_.reset(JS_GetPropertyStr(ctx_, carrier_.get(), "Memos"));
      if (certifiedArray_.isException())
        return false;
      certifiedObject_.reset(
          JS_GetPropertyUint32(ctx_, certifiedArray_.get(), 0));
      if (certifiedObject_.isException())
        return false;
      if (kind_ == ScenarioKind::certifiedObject) {
        fieldCode_ = 0x000E000A;
        argument_.reset(JS_DupValue(ctx_, certifiedObject_.get()));
        derivedExpected_ = sourceExpected_;
        derivedExpected_.push_back(0xEA);
        derivedExpected_.push_back(0xE1);
      } else {
        fieldCode_ = 0x000F0009;
        argument_.reset(JS_DupValue(ctx_, certifiedArray_.get()));
        derivedExpected_ = sourceExpected_;
        derivedExpected_.push_back(0xF9);
        auto const payload = certifiedArrayPayload();
        derivedExpected_.insert(derivedExpected_.end(), payload.begin(),
                                payload.end());
      }
      break;
    }
    case ScenarioKind::richNominal: {
      fieldCode_ = 0x00080001;
      for (std::uint32_t i = 0; i < accountExpected_.size(); ++i)
        accountExpected_[i] = static_cast<std::uint8_t>(0x40 + i);
      argument_.reset(types::makeAccountIDBytes(ctx_, accountExpected_.data(),
                                                accountExpected_.size()));
      derivedExpected_ = sourceExpected_;
      derivedExpected_.push_back(0x81);
      derivedExpected_.push_back(0x14);
      derivedExpected_.insert(derivedExpected_.end(), accountExpected_.begin(),
                              accountExpected_.end());
      break;
    }
    case ScenarioKind::numberString: {
      fieldCode_ = 0x00090001;
      field_.reset(JS_NewString(ctx_, "Number"));
      argument_.reset(JS_NewString(ctx_, "1.25"));
      constexpr std::array<std::uint8_t, 12> payload = {
          0x00, 0x04, 0x70, 0xDE, 0x4D, 0xF8,
          0x20, 0x00, 0xFF, 0xFF, 0xFF, 0xF1,
      };
      derivedExpected_ = sourceExpected_;
      derivedExpected_.push_back(0x91);
      derivedExpected_.insert(derivedExpected_.end(), payload.begin(),
                              payload.end());
      break;
    }
    case ScenarioKind::presentRemoval:
      fieldCode_ = 0x00020002;
      derivedExpected_.reserve(sourceExpected_.size() - 5);
      derivedExpected_.insert(derivedExpected_.end(), sourceExpected_.begin(),
                              sourceExpected_.begin() + 5);
      derivedExpected_.insert(derivedExpected_.end(),
                              sourceExpected_.begin() + 10,
                              sourceExpected_.end());
      break;
    case ScenarioKind::absentRemoval:
      fieldCode_ = 0x00080001;
      derivedExpected_ = sourceExpected_;
      break;
    }
    if (JS_IsUndefined(field_.get()))
      field_.reset(JS_NewUint32(ctx_, fieldCode_));
    argumentIdentity_.reset(JS_DupValue(ctx_, argument_.get()));
    return !field_.isException() && !argument_.isException() &&
           !JS_HasException(ctx_);
  }

  [[nodiscard]] JSValue call() const {
    JSValueConst arguments[] = {field_.get(), argument_.get()};
    int const argc = kind_ == ScenarioKind::presentRemoval ||
                             kind_ == ScenarioKind::absentRemoval
                         ? 1
                         : 2;
    return JS_Call(ctx_, method_.get(), source_.get(), argc, arguments);
  }

  void expectSourceUnchanged() const {
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(snapshotObject(ctx_, source_.get(), bytes));
    EXPECT_EQ(bytes, sourceExpected_);
    LocalValue cached(ctx_, JS_GetPropertyStr(ctx_, source_.get(), "Flags"));
    ASSERT_FALSE(cached.isException());
    EXPECT_EQ(JS_StrictEq(ctx_, cached.get(), sourceCached_.get()), 1);
    EXPECT_EQ(JS_StrictEq(ctx_, argument_.get(), argumentIdentity_.get()), 1);

    switch (kind_) {
    case ScenarioKind::rawLeaf:
      ASSERT_TRUE(copyContiguousBytes(ctx_, argument_.get(), bytes));
      EXPECT_EQ(bytes, (std::vector<std::uint8_t>{0, 0, 0, 0xEE}));
      break;
    case ScenarioKind::certifiedObject: {
      ASSERT_TRUE(snapshotObject(ctx_, argument_.get(), bytes));
      EXPECT_EQ(bytes, (std::vector<std::uint8_t>{0xE1}));
      LocalValue observed(ctx_,
                          JS_GetPropertyUint32(ctx_, certifiedArray_.get(), 0));
      ASSERT_FALSE(observed.isException());
      EXPECT_EQ(JS_StrictEq(ctx_, observed.get(), argument_.get()), 1);
      break;
    }
    case ScenarioKind::certifiedArray: {
      ASSERT_TRUE(snapshotField(ctx_, carrier_.get(), 0x000F0009, bytes));
      EXPECT_EQ(bytes, certifiedArrayPayload());
      LocalValue observed(ctx_,
                          JS_GetPropertyStr(ctx_, carrier_.get(), "Memos"));
      ASSERT_FALSE(observed.isException());
      EXPECT_EQ(JS_StrictEq(ctx_, observed.get(), argument_.get()), 1);
      break;
    }
    case ScenarioKind::richNominal: {
      std::array<std::uint8_t, 20> observed{};
      ASSERT_TRUE(
          types::readAccountIDBytes(ctx_, argument_.get(), observed.data()));
      EXPECT_EQ(observed, accountExpected_);
      break;
    }
    case ScenarioKind::numberString: {
      char const *text = JS_ToCString(ctx_, argument_.get());
      ASSERT_NE(text, nullptr);
      EXPECT_STREQ(text, "1.25");
      JS_FreeCString(ctx_, text);
      break;
    }
    case ScenarioKind::presentRemoval:
    case ScenarioKind::absentRemoval:
      break;
    }
    EXPECT_FALSE(JS_HasException(ctx_));
  }

  void expectDerived(JSValueConst value) const {
    ASSERT_FALSE(JS_IsException(value));
    ASSERT_TRUE(types::isSTObject(value));
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(snapshotObject(ctx_, value, bytes));
    EXPECT_EQ(bytes, derivedExpected_);

    if (kind_ == ScenarioKind::absentRemoval) {
      EXPECT_EQ(JS_StrictEq(ctx_, value, source_.get()), 1);
      return;
    }
    EXPECT_EQ(JS_StrictEq(ctx_, value, source_.get()), 0);

    if (kind_ == ScenarioKind::certifiedObject) {
      LocalValue observed(ctx_, JS_GetPropertyStr(ctx_, value, "Memo"));
      ASSERT_FALSE(observed.isException());
      EXPECT_EQ(JS_StrictEq(ctx_, observed.get(), argument_.get()), 0);
      ASSERT_TRUE(snapshotObject(ctx_, observed.get(), bytes));
      EXPECT_EQ(bytes, (std::vector<std::uint8_t>{0xE1}));
    } else if (kind_ == ScenarioKind::certifiedArray) {
      LocalValue observed(ctx_, JS_GetPropertyStr(ctx_, value, "Memos"));
      ASSERT_FALSE(observed.isException());
      EXPECT_EQ(JS_StrictEq(ctx_, observed.get(), argument_.get()), 0);
      LocalValue element(ctx_, JS_GetPropertyUint32(ctx_, observed.get(), 0));
      ASSERT_FALSE(element.isException());
      EXPECT_EQ(JS_StrictEq(ctx_, element.get(), certifiedObject_.get()), 0);
    } else if (kind_ == ScenarioKind::richNominal) {
      LocalValue observed(ctx_, JS_GetPropertyStr(ctx_, value, "Account"));
      ASSERT_FALSE(observed.isException());
      EXPECT_EQ(JS_StrictEq(ctx_, observed.get(), argument_.get()), 0);
      std::array<std::uint8_t, 20> account{};
      ASSERT_TRUE(
          types::readAccountIDBytes(ctx_, observed.get(), account.data()));
      EXPECT_EQ(account, accountExpected_);
    }
    EXPECT_FALSE(JS_HasException(ctx_));
  }

private:
  JSContext *ctx_;
  ScenarioKind kind_;
  LocalValue source_;
  LocalValue method_;
  LocalValue field_;
  LocalValue argument_;
  LocalValue argumentIdentity_;
  LocalValue sourceCached_;
  LocalValue carrier_;
  LocalValue certifiedArray_;
  LocalValue certifiedObject_;
  std::uint32_t fieldCode_ = 0;
  std::vector<std::uint8_t> sourceExpected_;
  std::vector<std::uint8_t> derivedExpected_;
  std::array<std::uint8_t, 20> accountExpected_{};
};

enum class DiagnosticKind : std::uint8_t {
  unknownStringField,
  malformedRawPayload,
  nonASCIIInvalidNumber,
  cappedObjectDepth,
};

[[nodiscard]] char const *diagnosticName(DiagnosticKind kind) noexcept {
  switch (kind) {
  case DiagnosticKind::unknownStringField:
    return "unknown string field atom";
  case DiagnosticKind::malformedRawPayload:
    return "malformed raw replacement";
  case DiagnosticKind::nonASCIIInvalidNumber:
    return "non-ASCII Number conversion";
  case DiagnosticKind::cappedObjectDepth:
    return "capped object-depth replacement";
  }
  return "unknown diagnostic";
}

[[nodiscard]] char const *diagnosticMessage(DiagnosticKind kind) noexcept {
  switch (kind) {
  case DiagnosticKind::unknownStringField:
    return "STObject.withField: unknown field";
  case DiagnosticKind::malformedRawPayload:
  case DiagnosticKind::nonASCIIInvalidNumber:
    return "STObject.withField: invalid canonical field value";
  case DiagnosticKind::cappedObjectDepth:
    return "STObject.withField: replacement exceeds object limits";
  }
  return "";
}

class PreparedDiagnostic {
public:
  PreparedDiagnostic(JSContext *context, DiagnosticKind kind)
      : ctx_(context), kind_(kind), source_(context), sourceCached_(context),
        method_(context), field_(context), argument_(context),
        argumentIdentity_(context), carrier_(context) {}

  [[nodiscard]] bool prepare() {
    sourceExpected_ = sourceWire();
    source_.reset(types::makeCertifiedObjectCopy(
        ctx_, sourceExpected_.data(),
        static_cast<std::uint32_t>(sourceExpected_.size())));
    if (source_.isException())
      return false;
    sourceCached_.reset(JS_GetPropertyStr(ctx_, source_.get(), "Flags"));
    method_.reset(JS_GetPropertyStr(ctx_, source_.get(), "withField"));
    if (sourceCached_.isException() || method_.isException())
      return false;

    switch (kind_) {
    case DiagnosticKind::unknownStringField: {
      field_.reset(JS_NewString(ctx_, "unknown-field-\xC3\xA9"));
      constexpr std::array<std::uint8_t, 4> payload = {0, 0, 0, 9};
      argumentExpected_.assign(payload.begin(), payload.end());
      argument_.reset(qjs::uint8Array(ctx_, payload));
      break;
    }
    case DiagnosticKind::malformedRawPayload: {
      field_.reset(JS_NewString(ctx_, "Flags"));
      constexpr std::array<std::uint8_t, 3> payload = {0, 0, 1};
      argumentExpected_.assign(payload.begin(), payload.end());
      argument_.reset(qjs::uint8Array(ctx_, payload));
      break;
    }
    case DiagnosticKind::nonASCIIInvalidNumber:
      field_.reset(JS_NewString(ctx_, "Number"));
      argument_.reset(JS_NewString(ctx_, "1.\xC3\xA9"));
      break;
    case DiagnosticKind::cappedObjectDepth: {
      field_.reset(JS_NewString(ctx_, "Memo"));
      auto const wire = cappedObjectCarrierWire();
      carrier_.reset(types::makeCertifiedObjectCopy(
          ctx_, wire.data(), static_cast<std::uint32_t>(wire.size())));
      if (carrier_.isException())
        return false;
      argument_.reset(JS_DupValue(ctx_, carrier_.get()));
      if (argument_.isException() ||
          !snapshotObject(ctx_, argument_.get(), argumentExpected_))
        return false;
      break;
    }
    }
    argumentIdentity_.reset(JS_DupValue(ctx_, argument_.get()));
    return !field_.isException() && !argument_.isException() &&
           !JS_HasException(ctx_);
  }

  [[nodiscard]] JSValue call() const {
    JSValueConst arguments[] = {field_.get(), argument_.get()};
    return JS_Call(ctx_, method_.get(), source_.get(), 2, arguments);
  }

  void expectUnchanged() const {
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(snapshotObject(ctx_, source_.get(), bytes));
    EXPECT_EQ(bytes, sourceExpected_);
    LocalValue cached(ctx_, JS_GetPropertyStr(ctx_, source_.get(), "Flags"));
    ASSERT_FALSE(cached.isException());
    EXPECT_EQ(JS_StrictEq(ctx_, cached.get(), sourceCached_.get()), 1);
    EXPECT_EQ(JS_StrictEq(ctx_, argument_.get(), argumentIdentity_.get()), 1);

    switch (kind_) {
    case DiagnosticKind::unknownStringField:
    case DiagnosticKind::malformedRawPayload:
      ASSERT_TRUE(copyContiguousBytes(ctx_, argument_.get(), bytes));
      EXPECT_EQ(bytes, argumentExpected_);
      break;
    case DiagnosticKind::nonASCIIInvalidNumber: {
      char const *text = JS_ToCString(ctx_, argument_.get());
      ASSERT_NE(text, nullptr);
      EXPECT_STREQ(text, "1.\xC3\xA9");
      JS_FreeCString(ctx_, text);
      break;
    }
    case DiagnosticKind::cappedObjectDepth: {
      ASSERT_TRUE(snapshotObject(ctx_, argument_.get(), bytes));
      EXPECT_EQ(bytes, argumentExpected_);
      EXPECT_EQ(JS_StrictEq(ctx_, carrier_.get(), argument_.get()), 1);
      break;
    }
    }
    EXPECT_FALSE(JS_HasException(ctx_));
  }

private:
  JSContext *ctx_;
  DiagnosticKind kind_;
  LocalValue source_;
  LocalValue sourceCached_;
  LocalValue method_;
  LocalValue field_;
  LocalValue argument_;
  LocalValue argumentIdentity_;
  LocalValue carrier_;
  std::vector<std::uint8_t> sourceExpected_;
  std::vector<std::uint8_t> argumentExpected_;
};

class PreparedFieldAtomLookup {
public:
  explicit PreparedFieldAtomLookup(JSContext *context)
      : ctx_(context), source_(context), sourceCached_(context),
        method_(context), field_(context), fieldIdentity_(context) {}

  [[nodiscard]] bool prepare() {
    sourceExpected_ = sourceWire();
    source_.reset(types::makeCertifiedObjectCopy(
        ctx_, sourceExpected_.data(),
        static_cast<std::uint32_t>(sourceExpected_.size())));
    if (source_.isException())
      return false;
    sourceCached_.reset(JS_GetPropertyStr(ctx_, source_.get(), "Flags"));
    method_.reset(JS_GetPropertyStr(ctx_, source_.get(), "has"));
    field_.reset(JS_NewString(ctx_, "unknown-atom-\xC3\xA9"));
    fieldIdentity_.reset(JS_DupValue(ctx_, field_.get()));
    return !sourceCached_.isException() && !method_.isException() &&
           !field_.isException() && !JS_HasException(ctx_);
  }

  [[nodiscard]] JSValue call() const {
    JSValueConst arguments[] = {field_.get()};
    return JS_Call(ctx_, method_.get(), source_.get(), 1, arguments);
  }

  void expectUnchanged() const {
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(snapshotObject(ctx_, source_.get(), bytes));
    EXPECT_EQ(bytes, sourceExpected_);
    LocalValue cached(ctx_, JS_GetPropertyStr(ctx_, source_.get(), "Flags"));
    ASSERT_FALSE(cached.isException());
    EXPECT_EQ(JS_StrictEq(ctx_, cached.get(), sourceCached_.get()), 1);
    EXPECT_EQ(JS_StrictEq(ctx_, field_.get(), fieldIdentity_.get()), 1);
    char const *text = JS_ToCString(ctx_, field_.get());
    ASSERT_NE(text, nullptr);
    EXPECT_STREQ(text, "unknown-atom-\xC3\xA9");
    JS_FreeCString(ctx_, text);
    EXPECT_FALSE(JS_HasException(ctx_));
  }

private:
  JSContext *ctx_;
  LocalValue source_;
  LocalValue sourceCached_;
  LocalValue method_;
  LocalValue field_;
  LocalValue fieldIdentity_;
  std::vector<std::uint8_t> sourceExpected_;
};

[[nodiscard]] bool containsRequest(std::span<AllocationRequest const> requests,
                                   std::size_t size) noexcept {
  return std::ranges::any_of(requests,
                             [size](AllocationRequest const &request) {
                               return request.size == size;
                             });
}

[[nodiscard]] bool containsRequest(std::span<AllocationRequest const> requests,
                                   std::size_t size,
                                   RequestKind kind) noexcept {
  return std::ranges::any_of(
      requests, [size, kind](AllocationRequest const &request) {
        return request.size == size && request.kind == kind;
      });
}

void expectNamedAllocationProfile(ScenarioKind kind,
                                  std::span<AllocationRequest const> requests) {
  auto const expected = expectedProfile(kind);
  ASSERT_FALSE(requests.empty()) << scenarioName(kind);
  EXPECT_TRUE(
      containsRequest(requests, expected.outputSize, RequestKind::allocate))
      << scenarioName(kind) << ": exact output buffer";
  EXPECT_EQ(requests.front().size, expected.outputSize)
      << scenarioName(kind)
      << ": recognized field atoms and ASCII Number conversion are "
         "allocation-free";
  EXPECT_EQ(requests.front().kind, RequestKind::allocate) << scenarioName(kind);
  std::size_t const indexSize =
      16 + 16 * expected.scopes + 20 * expected.fields;
  EXPECT_TRUE(containsRequest(requests, indexSize))
      << scenarioName(kind) << ": exact final index " << indexSize;
  EXPECT_TRUE(containsRequest(requests, sizeof(void *) * 2 + 8))
      << scenarioName(kind) << ": native owner state";
  EXPECT_TRUE(containsRequest(requests, sizeof(void *) == 8 ? std::size_t{32}
                                                            : std::size_t{16}))
      << scenarioName(kind) << ": top wrapper state";
  if (expected.fieldScratchCapacity != 0) {
    EXPECT_TRUE(containsRequest(requests, 16 * 28, RequestKind::allocate))
        << scenarioName(kind) << ": first field scratch spill";
    for (std::uint32_t capacity = 32; capacity <= expected.fieldScratchCapacity;
         capacity *= 2) {
      EXPECT_TRUE(
          containsRequest(requests, capacity * 28, RequestKind::reallocate))
          << scenarioName(kind) << ": field scratch growth to " << capacity;
    }
  }
  if (expected.scopeScratchCapacity != 0) {
    EXPECT_TRUE(containsRequest(requests, 8 * 16, RequestKind::allocate))
        << scenarioName(kind) << ": first scope scratch spill";
    for (std::uint32_t capacity = 16; capacity <= expected.scopeScratchCapacity;
         capacity *= 2) {
      EXPECT_TRUE(
          containsRequest(requests, capacity * 16, RequestKind::reallocate))
          << scenarioName(kind) << ": scope scratch growth to " << capacity;
    }
  }
}

TEST(ObjectReplacementOOM,
     EverySuccessfulRouteAllocationFailsAtomicallyAndRetriesInSameRuntime) {
  constexpr std::array scenarios = {
      ScenarioKind::rawLeaf,        ScenarioKind::certifiedObject,
      ScenarioKind::certifiedArray, ScenarioKind::richNominal,
      ScenarioKind::numberString,   ScenarioKind::presentRemoval,
  };

  for (ScenarioKind kind : scenarios) {
    SCOPED_TRACE(scenarioName(kind));
    std::vector<AllocationRequest> successfulRequests;
    Runtime measured;
    ASSERT_TRUE(measured.open());
    {
      PreparedScenario scenario(measured.context(), kind);
      ASSERT_TRUE(scenario.prepare());
      scenario.expectSourceUnchanged();
      std::size_t const liveBefore = measured.allocations.liveBlocks;

      measured.allocations.startRecording();
      LocalValue result(measured.context(), scenario.call());
      measured.allocations.stopRecording();
      ASSERT_FALSE(result.isException());
      ASSERT_LE(measured.allocations.recordedCount,
                measured.allocations.recorded.size());
      successfulRequests.assign(measured.allocations.recorded.begin(),
                                measured.allocations.recorded.begin() +
                                    measured.allocations.recordedCount);
      expectNamedAllocationProfile(kind, successfulRequests);
      scenario.expectDerived(result.get());
      result.reset();
      EXPECT_EQ(measured.allocations.liveBlocks, liveBefore);
      scenario.expectSourceUnchanged();
      EXPECT_EQ(measured.allocations.liveBlocks, liveBefore);
    }
    measured.close();
    EXPECT_EQ(measured.allocations.liveBlocks, 0u);

    ASSERT_FALSE(successfulRequests.empty());
    for (std::size_t ordinal = 1; ordinal <= successfulRequests.size();
         ++ordinal) {
      SCOPED_TRACE("allocator ordinal " + std::to_string(ordinal));
      Runtime injected;
      ASSERT_TRUE(injected.open());
      {
        PreparedScenario scenario(injected.context(), kind);
        ASSERT_TRUE(scenario.prepare());
        scenario.expectSourceUnchanged();
        std::size_t const liveBefore = injected.allocations.liveBlocks;
        std::size_t const rejectedBefore = injected.allocations.rejections;

        injected.allocations.startRecording();
        injected.allocations.rejectRelative(ordinal);
        LocalValue failed(injected.context(), scenario.call());
        injected.allocations.stopRecording();
        ASSERT_EQ(injected.allocations.rejections, rejectedBefore + 1);
        ASSERT_GE(injected.allocations.recordedCount, ordinal);
        for (std::size_t i = 0; i < ordinal; ++i) {
          EXPECT_EQ(injected.allocations.recorded[i], successfulRequests[i])
              << "request prefix at ordinal " << ordinal;
        }
        expectExactOOM(injected.context(), failed.get(), ordinal);
        EXPECT_EQ(injected.allocations.liveBlocks, liveBefore)
            << "partial replacement escaped";
        scenario.expectSourceUnchanged();
        EXPECT_EQ(injected.allocations.liveBlocks, liveBefore);

        LocalValue retry(injected.context(), scenario.call());
        ASSERT_FALSE(retry.isException())
            << "same-runtime retry after ordinal " << ordinal;
        scenario.expectDerived(retry.get());
        retry.reset();
        EXPECT_EQ(injected.allocations.liveBlocks, liveBefore);
        scenario.expectSourceUnchanged();
        EXPECT_EQ(injected.allocations.liveBlocks, liveBefore);
      }
      injected.close();
      EXPECT_EQ(injected.allocations.liveBlocks, 0u);
    }
  }
}

TEST(ObjectReplacementOOM,
     UnknownStringFieldAtomLookupIsAllocationFreeAndPreservesState) {
  Runtime measured;
  ASSERT_TRUE(measured.open());
  {
    PreparedFieldAtomLookup scenario(measured.context());
    ASSERT_TRUE(scenario.prepare());
    scenario.expectUnchanged();
    std::size_t const liveBefore = measured.allocations.liveBlocks;

    measured.allocations.startRecording();
    LocalValue result(measured.context(), scenario.call());
    measured.allocations.stopRecording();
    ASSERT_FALSE(result.isException());
    EXPECT_EQ(JS_ToBool(measured.context(), result.get()), 0);
    EXPECT_EQ(measured.allocations.recordedCount, 0u)
        << "string-to-atom lookup reuses the existing string storage";
    result.reset();
    EXPECT_EQ(measured.allocations.liveBlocks, liveBefore);
    scenario.expectUnchanged();
    EXPECT_EQ(measured.allocations.liveBlocks, liveBefore);
  }
  measured.close();
  EXPECT_EQ(measured.allocations.liveBlocks, 0u);
}

TEST(ObjectReplacementOOM,
     DiagnosticPublicationAllocationFailsAtomicallyAndRetriesInSameRuntime) {
  constexpr std::array scenarios = {
      DiagnosticKind::unknownStringField,
      DiagnosticKind::malformedRawPayload,
      DiagnosticKind::nonASCIIInvalidNumber,
      DiagnosticKind::cappedObjectDepth,
  };

  std::size_t malformedRequestCount = 0;
  for (DiagnosticKind kind : scenarios) {
    SCOPED_TRACE(diagnosticName(kind));
    std::vector<AllocationRequest> diagnosticRequests;
    Runtime measured;
    ASSERT_TRUE(measured.open());
    {
      PreparedDiagnostic scenario(measured.context(), kind);
      ASSERT_TRUE(scenario.prepare());
      scenario.expectUnchanged();
      std::size_t const liveBefore = measured.allocations.liveBlocks;

      measured.allocations.startRecording();
      LocalValue diagnostic(measured.context(), scenario.call());
      measured.allocations.stopRecording();
      ASSERT_LE(measured.allocations.recordedCount,
                measured.allocations.recorded.size());
      diagnosticRequests.assign(measured.allocations.recorded.begin(),
                                measured.allocations.recorded.begin() +
                                    measured.allocations.recordedCount);
      expectExactTypeError(measured.context(), diagnostic.get(),
                           diagnosticMessage(kind), diagnosticName(kind));
      diagnostic.reset();
      EXPECT_EQ(measured.allocations.liveBlocks, liveBefore);
      scenario.expectUnchanged();
      EXPECT_EQ(measured.allocations.liveBlocks, liveBefore);

      LocalValue retry(measured.context(), scenario.call());
      expectExactTypeError(measured.context(), retry.get(),
                           diagnosticMessage(kind), diagnosticName(kind));
      retry.reset();
      EXPECT_EQ(measured.allocations.liveBlocks, liveBefore);
      scenario.expectUnchanged();
      EXPECT_EQ(measured.allocations.liveBlocks, liveBefore);
    }
    measured.close();
    EXPECT_EQ(measured.allocations.liveBlocks, 0u);

    ASSERT_FALSE(diagnosticRequests.empty())
        << diagnosticName(kind) << ": diagnostic publication must be fallible";
    if (kind == DiagnosticKind::malformedRawPayload)
      malformedRequestCount = diagnosticRequests.size();
    if (kind == DiagnosticKind::nonASCIIInvalidNumber)
      EXPECT_EQ(diagnosticRequests.size(), malformedRequestCount + 1)
          << "UTF-8 Number conversion must add one allocator-visible request";

    for (std::size_t ordinal = 1; ordinal <= diagnosticRequests.size();
         ++ordinal) {
      SCOPED_TRACE("allocator ordinal " + std::to_string(ordinal));
      Runtime injected;
      ASSERT_TRUE(injected.open());
      {
        PreparedDiagnostic scenario(injected.context(), kind);
        ASSERT_TRUE(scenario.prepare());
        scenario.expectUnchanged();
        std::size_t const liveBefore = injected.allocations.liveBlocks;
        std::size_t const rejectedBefore = injected.allocations.rejections;

        injected.allocations.startRecording();
        injected.allocations.rejectRelative(ordinal);
        LocalValue failed(injected.context(), scenario.call());
        injected.allocations.stopRecording();
        ASSERT_EQ(injected.allocations.rejections, rejectedBefore + 1);
        ASSERT_GE(injected.allocations.recordedCount, ordinal);
        for (std::size_t i = 0; i < ordinal; ++i) {
          EXPECT_EQ(injected.allocations.recorded[i], diagnosticRequests[i])
              << "request prefix at ordinal " << ordinal;
        }
        expectExactOOM(injected.context(), failed.get(), ordinal);
        failed.reset();
        EXPECT_EQ(injected.allocations.liveBlocks, liveBefore)
            << "partial diagnostic/replacement escaped";
        scenario.expectUnchanged();
        EXPECT_EQ(injected.allocations.liveBlocks, liveBefore);

        LocalValue retry(injected.context(), scenario.call());
        expectExactTypeError(injected.context(), retry.get(),
                             diagnosticMessage(kind), diagnosticName(kind));
        retry.reset();
        EXPECT_EQ(injected.allocations.liveBlocks, liveBefore);
        scenario.expectUnchanged();
        EXPECT_EQ(injected.allocations.liveBlocks, liveBefore);
      }
      injected.close();
      EXPECT_EQ(injected.allocations.liveBlocks, 0u);
    }
  }
}

TEST(ObjectReplacementOOM,
     AbsentWithoutFieldIsAllocationFreeAndPreservesExactWrapperIdentity) {
  Runtime runtime;
  ASSERT_TRUE(runtime.open());
  {
    PreparedScenario scenario(runtime.context(), ScenarioKind::absentRemoval);
    ASSERT_TRUE(scenario.prepare());
    scenario.expectSourceUnchanged();
    std::size_t const requestsBefore = runtime.allocations.requests;
    std::size_t const liveBefore = runtime.allocations.liveBlocks;
    std::size_t const rejectedBefore = runtime.allocations.rejections;

    runtime.allocations.startRecording();
    runtime.allocations.rejectRelative(1);
    LocalValue result(runtime.context(), scenario.call());
    runtime.allocations.stopRecording();
    ASSERT_FALSE(result.isException());
    EXPECT_EQ(runtime.allocations.requests, requestsBefore);
    EXPECT_EQ(runtime.allocations.recordedCount, 0u);
    EXPECT_EQ(runtime.allocations.rejections, rejectedBefore);
    runtime.allocations.disarm();
    scenario.expectDerived(result.get());
    result.reset();
    EXPECT_EQ(runtime.allocations.liveBlocks, liveBefore);
    scenario.expectSourceUnchanged();
    EXPECT_EQ(runtime.allocations.liveBlocks, liveBefore);

    LocalValue retry(runtime.context(), scenario.call());
    ASSERT_FALSE(retry.isException());
    scenario.expectDerived(retry.get());
    retry.reset();
    EXPECT_EQ(runtime.allocations.liveBlocks, liveBefore);
  }
  runtime.close();
  EXPECT_EQ(runtime.allocations.liveBlocks, 0u);
}

void expectInputBytes(JSContext *ctx, JSValueConst input,
                      std::span<std::uint8_t const> expected) {
  JSValue backing = JS_UNDEFINED;
  std::uint8_t const *data = nullptr;
  std::size_t size = 0;
  ASSERT_EQ(JS_GetObjectByteSpanNoThrow(ctx, input, &backing, &data, &size),
            JS_OBJECT_BYTES_OK);
  LocalValue ownedBacking(ctx, backing);
  ASSERT_EQ(size, expected.size());
  EXPECT_EQ(std::memcmp(data, expected.data(), expected.size()), 0);
  EXPECT_FALSE(JS_HasException(ctx));
}

void expectSafeDecodeSuccess(JSContext *ctx, JSValueConst value,
                             std::span<std::uint8_t const> expected) {
  ASSERT_FALSE(JS_IsException(value));
  ASSERT_TRUE(jshookz::provider::bindings::isResult(value));
  LocalValue ok(ctx, JS_GetPropertyStr(ctx, value, "ok"));
  LocalValue object(ctx, JS_GetPropertyStr(ctx, value, "value"));
  ASSERT_FALSE(ok.isException());
  ASSERT_FALSE(object.isException());
  EXPECT_EQ(JS_ToBool(ctx, ok.get()), 1);
  ASSERT_TRUE(types::isSTObject(object.get()));
  std::vector<std::uint8_t> observed;
  ASSERT_TRUE(snapshotObject(ctx, object.get(), observed));
  EXPECT_EQ(observed,
            (std::vector<std::uint8_t>{expected.begin(), expected.end()}));
  EXPECT_EQ(JS_IsExtensible(ctx, value), 0);
  EXPECT_FALSE(JS_HasException(ctx));
}

void expectSafeDecodeFailure(JSContext *ctx, JSValueConst value) {
  ASSERT_FALSE(JS_IsException(value));
  ASSERT_TRUE(jshookz::provider::bindings::isResult(value));
  LocalValue ok(ctx, JS_GetPropertyStr(ctx, value, "ok"));
  LocalValue error(ctx, JS_GetPropertyStr(ctx, value, "error"));
  ASSERT_FALSE(ok.isException());
  ASSERT_FALSE(error.isException());
  EXPECT_EQ(JS_ToBool(ctx, ok.get()), 0);
  ASSERT_TRUE(JS_IsObject(error.get()));
  LocalValue prototype(ctx, JS_GetPrototype(ctx, error.get()));
  ASSERT_FALSE(prototype.isException());
  EXPECT_TRUE(JS_IsNull(prototype.get()));
  EXPECT_EQ(stringProperty(ctx, error.get(), "domain"), "parse");
  EXPECT_EQ(stringProperty(ctx, error.get(), "issue"), "invalid-field");
  LocalValue offset(ctx, JS_GetPropertyStr(ctx, error.get(), "offset"));
  LocalValue fieldCode(ctx, JS_GetPropertyStr(ctx, error.get(), "fieldCode"));
  std::uint32_t observedOffset = 0;
  std::uint32_t observedFieldCode = 0;
  ASSERT_EQ(JS_ToUint32(ctx, &observedOffset, offset.get()), 0);
  ASSERT_EQ(JS_ToUint32(ctx, &observedFieldCode, fieldCode.get()), 0);
  EXPECT_EQ(observedOffset, 1u);
  EXPECT_EQ(observedFieldCode, 0x00020002u);
  EXPECT_EQ(JS_IsExtensible(ctx, error.get()), 0);
  EXPECT_EQ(JS_IsExtensible(ctx, value), 0);
  EXPECT_FALSE(JS_HasException(ctx));
}

void expectByteLimitFailure(JSContext *ctx, JSValueConst value,
                            std::uint32_t maximum, std::uint32_t actual) {
  ASSERT_FALSE(JS_IsException(value));
  ASSERT_TRUE(jshookz::provider::bindings::isResult(value));
  LocalValue ok(ctx, JS_GetPropertyStr(ctx, value, "ok"));
  LocalValue error(ctx, JS_GetPropertyStr(ctx, value, "error"));
  ASSERT_FALSE(ok.isException());
  ASSERT_FALSE(error.isException());
  EXPECT_EQ(JS_ToBool(ctx, ok.get()), 0);
  EXPECT_EQ(stringProperty(ctx, error.get(), "domain"), "parse");
  EXPECT_EQ(stringProperty(ctx, error.get(), "issue"), "resource-limit");
  EXPECT_EQ(stringProperty(ctx, error.get(), "limit"), "bytes");
  LocalValue observedMaximum(ctx,
                             JS_GetPropertyStr(ctx, error.get(), "maximum"));
  LocalValue observedActualAtLeast(
      ctx, JS_GetPropertyStr(ctx, error.get(), "actualAtLeast"));
  std::uint32_t maximumValue = 0;
  std::uint32_t actualValue = 0;
  ASSERT_EQ(JS_ToUint32(ctx, &maximumValue, observedMaximum.get()), 0);
  ASSERT_EQ(JS_ToUint32(ctx, &actualValue, observedActualAtLeast.get()), 0);
  EXPECT_EQ(maximumValue, maximum);
  EXPECT_EQ(actualValue, actual);
  EXPECT_FALSE(JS_HasException(ctx));
}

template <class Call, class Expect, class InspectProfile>
void exerciseEveryAllocationRequest(Runtime &runtime, char const *name,
                                    Call call, Expect expect,
                                    InspectProfile inspectProfile) {
  SCOPED_TRACE(name);
  auto *ctx = runtime.context();

  // Stabilize atoms and object shapes without retaining any route result.
  for (int pass = 0; pass < 2; ++pass) {
    LocalValue warm(ctx, call());
    expect(warm.get());
  }
  std::size_t const liveBefore = runtime.allocations.liveBlocks;

  runtime.allocations.startRecording();
  LocalValue measured(ctx, call());
  runtime.allocations.stopRecording();
  ASSERT_LE(runtime.allocations.recordedCount,
            runtime.allocations.recorded.size());
  std::size_t const requestCount = runtime.allocations.recordedCount;
  auto const successfulRequests = runtime.allocations.recorded;
  ASSERT_GT(requestCount, 0u);
  inspectProfile(std::span<AllocationRequest const>{successfulRequests.data(),
                                                    requestCount});
  expect(measured.get());
  measured.reset();
  ASSERT_EQ(runtime.allocations.liveBlocks, liveBefore);

  for (std::size_t ordinal = 1; ordinal <= requestCount; ++ordinal) {
    SCOPED_TRACE("allocator ordinal " + std::to_string(ordinal));
    std::size_t const rejectionsBefore = runtime.allocations.rejections;
    runtime.allocations.startRecording();
    runtime.allocations.rejectRelative(ordinal);
    LocalValue failed(ctx, call());
    runtime.allocations.stopRecording();
    runtime.allocations.disarm();

    ASSERT_EQ(runtime.allocations.rejections, rejectionsBefore + 1);
    ASSERT_GE(runtime.allocations.recordedCount, ordinal);
    for (std::size_t i = 0; i < ordinal; ++i)
      EXPECT_EQ(runtime.allocations.recorded[i], successfulRequests[i])
          << "request prefix at ordinal " << ordinal;
    expectExactOOM(ctx, failed.get(), ordinal);
    failed.reset();
    EXPECT_EQ(runtime.allocations.liveBlocks, liveBefore)
        << "partial utility result escaped at ordinal " << ordinal;

    LocalValue retry(ctx, call());
    expect(retry.get());
    retry.reset();
    EXPECT_EQ(runtime.allocations.liveBlocks, liveBefore)
        << "same-runtime retry leaked at ordinal " << ordinal;
  }
}

TEST(ObjectUtilityOOM,
     SuccessAndDataFailureResultsPublishAtomicallyAndRetryInSameRuntime) {
  Runtime runtime;
  ASSERT_TRUE(runtime.open());
  auto *ctx = runtime.context();
  constexpr std::array<std::uint8_t, 5> valid = {0x22, 0, 0, 0, 9};
  constexpr std::array<std::uint8_t, 2> malformed = {0x22, 0};
  LocalValue validInput(ctx, qjs::uint8Array(ctx, valid));
  LocalValue malformedInput(ctx, qjs::uint8Array(ctx, malformed));
  ASSERT_FALSE(validInput.isException());
  ASSERT_FALSE(malformedInput.isException());

  exerciseEveryAllocationRequest(
      runtime, "safeDecodeObject success",
      [&] { return types::safeDecodeObjectBytes(ctx, validInput.get()); },
      [&](JSValueConst value) {
        expectSafeDecodeSuccess(ctx, value, valid);
        expectInputBytes(ctx, validInput.get(), valid);
      },
      [&](std::span<AllocationRequest const> requests) {
        EXPECT_TRUE(
            containsRequest(requests, valid.size(), RequestKind::allocate));
        EXPECT_TRUE(
            containsRequest(requests, 16u + 16u + 20u, RequestKind::allocate));
        EXPECT_TRUE(containsRequest(requests, sizeof(void *) * 2 + 8,
                                    RequestKind::allocate));
        EXPECT_TRUE(containsRequest(
            requests, sizeof(void *) == 8 ? std::size_t{32} : std::size_t{16},
            RequestKind::allocate));
      });

  exerciseEveryAllocationRequest(
      runtime, "safeDecodeObject data failure",
      [&] { return types::safeDecodeObjectBytes(ctx, malformedInput.get()); },
      [&](JSValueConst value) {
        expectSafeDecodeFailure(ctx, value);
        expectInputBytes(ctx, malformedInput.get(), malformed);
      },
      [&](std::span<AllocationRequest const> requests) {
        EXPECT_TRUE(
            containsRequest(requests, malformed.size(), RequestKind::allocate));
        EXPECT_GE(requests.size(), 6u)
            << "parse error and failure Result envelopes must be visible";
      });

  validInput.reset();
  malformedInput.reset();
  runtime.close();
  EXPECT_EQ(runtime.allocations.liveBlocks, 0u);
}

TEST(ObjectUtilityOOM,
     AssertionAndContractTypeErrorsAreExactOOMTotalAndRetryable) {
  Runtime runtime;
  ASSERT_TRUE(runtime.open());
  auto *ctx = runtime.context();
  constexpr std::array<std::uint8_t, 2> malformed = {0x22, 0};
  LocalValue malformedInput(ctx, qjs::uint8Array(ctx, malformed));
  LocalValue wrongKind(ctx, JS_NewArray(ctx));
  LocalValue detached(
      ctx, JS_NewArrayBufferCopy(ctx, malformed.data(), malformed.size()));
  ASSERT_FALSE(malformedInput.isException());
  ASSERT_FALSE(wrongKind.isException());
  ASSERT_FALSE(detached.isException());
  JS_DetachArrayBuffer(ctx, detached.get());
  ASSERT_FALSE(JS_HasException(ctx));

  exerciseEveryAllocationRequest(
      runtime, "decodeObject malformed assertion",
      [&] { return types::decodeObjectBytes(ctx, malformedInput.get()); },
      [&](JSValueConst value) {
        expectExactTypeError(ctx, value, "truncated field", "decode malformed");
        expectInputBytes(ctx, malformedInput.get(), malformed);
      },
      [&](std::span<AllocationRequest const> requests) {
        EXPECT_TRUE(
            containsRequest(requests, malformed.size(), RequestKind::allocate));
        EXPECT_GE(requests.size(), 5u)
            << "bare TypeError, message, and diagnostics must be visible";
      });

  struct ContractCase {
    char const *name;
    JSValueConst input;
    char const *message;
  };
  std::array const cases = {
      ContractCase{"wrong kind", wrongKind.get(),
                   "expected Uint8Array, ArrayBuffer, or STBlob"},
      ContractCase{"detached", detached.get(),
                   "object byte backing is detached or unusable"},
  };
  for (auto const &testCase : cases) {
    SCOPED_TRACE(testCase.name);
    exerciseEveryAllocationRequest(
        runtime, "validateObject contract TypeError",
        [&] { return types::validateObjectBytes(ctx, testCase.input); },
        [&](JSValueConst value) {
          expectExactTypeError(ctx, value, testCase.message, testCase.name);
        },
        [&](std::span<AllocationRequest const> requests) {
          EXPECT_GE(requests.size(), 2u)
              << "bare TypeError and message publication must be visible";
        });
    exerciseEveryAllocationRequest(
        runtime, "safeDecodeObject contract TypeError",
        [&] { return types::safeDecodeObjectBytes(ctx, testCase.input); },
        [&](JSValueConst value) {
          expectExactTypeError(ctx, value, testCase.message, testCase.name);
        },
        [&](std::span<AllocationRequest const> requests) {
          EXPECT_GE(requests.size(), 2u)
              << "bare TypeError and message publication must be visible";
        });
    exerciseEveryAllocationRequest(
        runtime, "decodeObject contract TypeError",
        [&] { return types::decodeObjectBytes(ctx, testCase.input); },
        [&](JSValueConst value) {
          expectExactTypeError(ctx, value, testCase.message, testCase.name);
        },
        [&](std::span<AllocationRequest const> requests) {
          EXPECT_GE(requests.size(), 2u)
              << "bare TypeError and message publication must be visible";
        });
  }

  malformedInput.reset();
  wrongKind.reset();
  detached.reset();
  runtime.close();
  EXPECT_EQ(runtime.allocations.liveBlocks, 0u);
}

TEST(ObjectUtilityCaps, OversizeInputsAreRejectedBeforePayloadCopy) {
  Runtime runtime;
  ASSERT_TRUE(runtime.open());
  auto *ctx = runtime.context();
  std::uint32_t const maximum = catl::xdata::RecursiveScanLimits{}.max_bytes;
  std::uint32_t const actual = maximum + 1;
  std::vector<std::uint8_t> oversized(actual, 0);

  auto expectNoPayloadCopy = [&] {
    ASSERT_LE(runtime.allocations.recordedCount,
              runtime.allocations.recorded.size());
    EXPECT_FALSE(containsRequest(
        std::span<AllocationRequest const>{runtime.allocations.recorded.data(),
                                           runtime.allocations.recordedCount},
        actual, RequestKind::allocate));
  };

  runtime.allocations.startRecording();
  LocalValue direct(
      ctx, types::makeCertifiedObjectCopy(ctx, oversized.data(), actual));
  runtime.allocations.stopRecording();
  expectNoPayloadCopy();
  expectExactTypeError(ctx, direct.get(),
                       "serialized object exceeds byte limit", "direct copy");

  LocalValue input(ctx, qjs::uint8Array(ctx, oversized));
  ASSERT_FALSE(input.isException());

  runtime.allocations.startRecording();
  LocalValue safe(ctx, types::safeDecodeObjectBytes(ctx, input.get()));
  runtime.allocations.stopRecording();
  expectNoPayloadCopy();
  expectByteLimitFailure(ctx, safe.get(), maximum, actual);

  runtime.allocations.startRecording();
  LocalValue asserted(ctx, types::decodeObjectBytes(ctx, input.get()));
  runtime.allocations.stopRecording();
  expectNoPayloadCopy();
  expectExactTypeError(ctx, asserted.get(),
                       "serialized object exceeds byte limit", "asserted copy");

  direct.reset();
  safe.reset();
  asserted.reset();
  input.reset();
  runtime.close();
  EXPECT_EQ(runtime.allocations.liveBlocks, 0u);
}

[[nodiscard]] LocalValue iteratorMethodFor(JSContext *ctx, JSValueConst array) {
  LocalValue global(ctx, JS_GetGlobalObject(ctx));
  LocalValue symbol(ctx, JS_GetPropertyStr(ctx, global.get(), "Symbol"));
  LocalValue iteratorSymbol(ctx,
                            JS_GetPropertyStr(ctx, symbol.get(), "iterator"));
  if (global.isException() || symbol.isException() ||
      iteratorSymbol.isException())
    return LocalValue(ctx, JS_EXCEPTION);
  JSAtom atom = JS_ValueToAtom(ctx, iteratorSymbol.get());
  if (atom == JS_ATOM_NULL)
    return LocalValue(ctx, JS_EXCEPTION);
  LocalValue method(ctx, JS_GetProperty(ctx, array, atom));
  JS_FreeAtom(ctx, atom);
  return method;
}

void expectIteratorResult(JSContext *ctx, JSValueConst result, bool done,
                          JSValueConst expectedValue = JS_UNDEFINED) {
  ASSERT_FALSE(JS_IsException(result));
  ASSERT_TRUE(JS_IsObject(result));
  LocalValue observedValue(ctx, JS_GetPropertyStr(ctx, result, "value"));
  LocalValue observedDone(ctx, JS_GetPropertyStr(ctx, result, "done"));
  ASSERT_FALSE(observedValue.isException());
  ASSERT_FALSE(observedDone.isException());
  EXPECT_EQ(JS_ToBool(ctx, observedDone.get()), done ? 1 : 0);
  if (done)
    EXPECT_TRUE(JS_IsUndefined(observedValue.get()));
  else
    EXPECT_EQ(JS_StrictEq(ctx, observedValue.get(), expectedValue), 1);
  EXPECT_FALSE(JS_HasException(ctx));
}

struct IteratorFixture {
  explicit IteratorFixture(JSContext *context)
      : ctx(context), root(context), array(context), element(context),
        iteratorMethod(context) {}

  [[nodiscard]] bool prepare() {
    constexpr std::array<std::uint8_t, 4> wire = {0xF9, 0xEA, 0xE1, 0xF1};
    root.reset(types::makeCertifiedObjectCopy(ctx, wire.data(), wire.size()));
    if (root.isException())
      return false;
    array.reset(JS_GetPropertyStr(ctx, root.get(), "Memos"));
    if (array.isException() || !types::isSTArray(array.get()))
      return false;
    element.reset(JS_GetPropertyUint32(ctx, array.get(), 0));
    if (element.isException() || !types::isSTObject(element.get()))
      return false;
    iteratorMethod = iteratorMethodFor(ctx, array.get());
    return !iteratorMethod.isException() &&
           JS_IsFunction(ctx, iteratorMethod.get());
  }

  [[nodiscard]] JSValue iterator() const {
    return JS_Call(ctx, iteratorMethod.get(), array.get(), 0, nullptr);
  }

  JSContext *ctx;
  LocalValue root;
  LocalValue array;
  LocalValue element;
  LocalValue iteratorMethod;
};

TEST(ObjectIteratorOOM,
     IteratorObjectAndStateAllocationsAreAtomicAndRetryable) {
  Runtime runtime;
  ASSERT_TRUE(runtime.open());
  IteratorFixture fixture(runtime.context());
  ASSERT_TRUE(fixture.prepare());
  auto *ctx = runtime.context();

  for (int pass = 0; pass < 2; ++pass) {
    LocalValue warm(ctx, fixture.iterator());
    ASSERT_FALSE(warm.isException());
  }
  std::size_t const liveBefore = runtime.allocations.liveBlocks;
  runtime.allocations.startRecording();
  LocalValue measured(ctx, fixture.iterator());
  runtime.allocations.stopRecording();
  ASSERT_FALSE(measured.isException());
  ASSERT_LE(runtime.allocations.recordedCount,
            runtime.allocations.recorded.size());
  std::size_t const requestCount = runtime.allocations.recordedCount;
  auto const successfulRequests = runtime.allocations.recorded;
  ASSERT_GE(requestCount, 2u);
  measured.reset();
  ASSERT_EQ(runtime.allocations.liveBlocks, liveBefore);

  for (std::size_t ordinal = 1; ordinal <= requestCount; ++ordinal) {
    SCOPED_TRACE(ordinal);
    std::size_t const rejectionsBefore = runtime.allocations.rejections;
    runtime.allocations.startRecording();
    runtime.allocations.rejectRelative(ordinal);
    LocalValue failed(ctx, fixture.iterator());
    runtime.allocations.stopRecording();
    runtime.allocations.disarm();
    ASSERT_EQ(runtime.allocations.rejections, rejectionsBefore + 1);
    ASSERT_GE(runtime.allocations.recordedCount, ordinal);
    for (std::size_t i = 0; i < ordinal; ++i)
      EXPECT_EQ(runtime.allocations.recorded[i], successfulRequests[i]);
    expectExactOOM(ctx, failed.get(), ordinal);
    failed.reset();
    EXPECT_EQ(runtime.allocations.liveBlocks, liveBefore);

    LocalValue retry(ctx, fixture.iterator());
    ASSERT_FALSE(retry.isException());
    LocalValue next(ctx, JS_GetPropertyStr(ctx, retry.get(), "next"));
    EXPECT_TRUE(JS_IsFunction(ctx, next.get()));
    retry.reset();
    next.reset();
    EXPECT_EQ(runtime.allocations.liveBlocks, liveBefore);
  }

  fixture.iteratorMethod.reset();
  fixture.element.reset();
  fixture.array.reset();
  fixture.root.reset();
  runtime.close();
  EXPECT_EQ(runtime.allocations.liveBlocks, 0u);
}

enum class IteratorResultRoute : std::uint8_t { value, terminal };

void exerciseIteratorResultOOM(Runtime &runtime, IteratorFixture &fixture,
                               IteratorResultRoute route) {
  auto *ctx = runtime.context();
  auto prepareIterator = [&]() {
    LocalValue iterator(ctx, fixture.iterator());
    if (iterator.isException())
      return std::pair{std::move(iterator), LocalValue(ctx, JS_EXCEPTION)};
    LocalValue next(ctx, JS_GetPropertyStr(ctx, iterator.get(), "next"));
    if (route == IteratorResultRoute::terminal && !next.isException()) {
      LocalValue first(ctx,
                       JS_Call(ctx, next.get(), iterator.get(), 0, nullptr));
      if (first.isException())
        return std::pair{std::move(iterator), LocalValue(ctx, JS_EXCEPTION)};
      expectIteratorResult(ctx, first.get(), false, fixture.element.get());
    }
    return std::pair{std::move(iterator), std::move(next)};
  };

  // Stabilize ordinary iterator-result shapes before measuring.
  for (int pass = 0; pass < 2; ++pass) {
    auto [iterator, next] = prepareIterator();
    ASSERT_FALSE(iterator.isException());
    ASSERT_FALSE(next.isException());
    LocalValue result(ctx,
                      JS_Call(ctx, next.get(), iterator.get(), 0, nullptr));
    expectIteratorResult(ctx, result.get(),
                         route == IteratorResultRoute::terminal,
                         fixture.element.get());
  }
  std::size_t const fixtureLive = runtime.allocations.liveBlocks;

  auto [measuredIterator, measuredNext] = prepareIterator();
  ASSERT_FALSE(measuredIterator.isException());
  ASSERT_FALSE(measuredNext.isException());
  runtime.allocations.startRecording();
  LocalValue measured(ctx, JS_Call(ctx, measuredNext.get(),
                                   measuredIterator.get(), 0, nullptr));
  runtime.allocations.stopRecording();
  ASSERT_LE(runtime.allocations.recordedCount,
            runtime.allocations.recorded.size());
  std::size_t const requestCount = runtime.allocations.recordedCount;
  auto const successfulRequests = runtime.allocations.recorded;
  ASSERT_GE(requestCount, 1u);
  expectIteratorResult(ctx, measured.get(),
                       route == IteratorResultRoute::terminal,
                       fixture.element.get());
  measured.reset();
  measuredNext.reset();
  measuredIterator.reset();
  ASSERT_EQ(runtime.allocations.liveBlocks, fixtureLive);

  for (std::size_t ordinal = 1; ordinal <= requestCount; ++ordinal) {
    SCOPED_TRACE(ordinal);
    auto [iterator, next] = prepareIterator();
    ASSERT_FALSE(iterator.isException());
    ASSERT_FALSE(next.isException());
    std::size_t const liveBefore = runtime.allocations.liveBlocks;
    std::size_t const rejectionsBefore = runtime.allocations.rejections;
    runtime.allocations.startRecording();
    runtime.allocations.rejectRelative(ordinal);
    LocalValue failed(ctx,
                      JS_Call(ctx, next.get(), iterator.get(), 0, nullptr));
    runtime.allocations.stopRecording();
    runtime.allocations.disarm();
    ASSERT_EQ(runtime.allocations.rejections, rejectionsBefore + 1);
    ASSERT_GE(runtime.allocations.recordedCount, ordinal);
    for (std::size_t i = 0; i < ordinal; ++i)
      EXPECT_EQ(runtime.allocations.recorded[i], successfulRequests[i]);
    expectExactOOM(ctx, failed.get(), ordinal);
    failed.reset();
    EXPECT_EQ(runtime.allocations.liveBlocks, liveBefore);

    LocalValue retry(ctx, JS_Call(ctx, next.get(), iterator.get(), 0, nullptr));
    expectIteratorResult(ctx, retry.get(),
                         route == IteratorResultRoute::terminal,
                         fixture.element.get());
    retry.reset();
    LocalValue following(ctx,
                         JS_Call(ctx, next.get(), iterator.get(), 0, nullptr));
    expectIteratorResult(ctx, following.get(), true);
    following.reset();
    next.reset();
    iterator.reset();
    EXPECT_EQ(runtime.allocations.liveBlocks, fixtureLive);
  }
}

TEST(ObjectIteratorOOM,
     ValueAndTerminalResultEnvelopesPreserveCursorAndCachedIdentity) {
  Runtime runtime;
  ASSERT_TRUE(runtime.open());
  IteratorFixture fixture(runtime.context());
  ASSERT_TRUE(fixture.prepare());
  exerciseIteratorResultOOM(runtime, fixture, IteratorResultRoute::value);
  exerciseIteratorResultOOM(runtime, fixture, IteratorResultRoute::terminal);

  fixture.iteratorMethod.reset();
  fixture.element.reset();
  fixture.array.reset();
  fixture.root.reset();
  runtime.close();
  EXPECT_EQ(runtime.allocations.liveBlocks, 0u);
}

enum class ArrayCacheBankKind : std::uint8_t {
  onePage,
  pagesUnderOneBranch,
  pageUnderEveryBranch,
  valueInEveryPage,
  maximumSequential,
};

struct ArrayCacheBank {
  char const *name;
  ArrayCacheBankKind kind;
  std::uint32_t length;
  std::uint32_t branches;
  std::uint32_t pages;
  std::uint32_t values;
  std::size_t requestedBytes;
  std::size_t allocations;
};

constexpr std::size_t arrayCacheRootBytes = 272;
constexpr std::size_t arrayCacheBranchBytes = 256;
constexpr std::size_t arrayCacheLeafBytes = 512;
constexpr std::size_t objectHeaderBytes = 72;
constexpr std::size_t objectPropertyStorageBytes = 32;
constexpr std::size_t objectStateBytes = 32;
constexpr std::uint32_t maximumArrayCacheLength = 32'767;
constexpr std::uint32_t maximumArrayCacheNopsPerObject = 30;
static_assert(maximumArrayCacheNopsPerObject < 64);
static_assert(maximumArrayCacheNopsPerObject + 1 +
                  maximumArrayCacheLength *
                      (1 + maximumArrayCacheNopsPerObject + 1) +
                  1 ==
              1'048'576);
static_assert(maximumArrayCacheLength + 1 == 32'768);
static_assert(maximumArrayCacheLength + 2 == 32'769);

constexpr std::array arrayCacheBanks = {
    ArrayCacheBank{"one-page", ArrayCacheBankKind::onePage, 32, 1, 1, 32, 1'040,
                   3},
    ArrayCacheBank{"one-branch", ArrayCacheBankKind::pagesUnderOneBranch, 1'024,
                   1, 32, 1'024, 16'912, 34},
    ArrayCacheBank{"all-branches", ArrayCacheBankKind::pageUnderEveryBranch,
                   31'776, 32, 32, 1'024, 24'848, 65},
    ArrayCacheBank{"all-pages", ArrayCacheBankKind::valueInEveryPage, 32'737,
                   32, 1'024, 1'024, 532'752, 1'057},
    ArrayCacheBank{"maximum", ArrayCacheBankKind::maximumSequential,
                   maximumArrayCacheLength, 32, 1'024, maximumArrayCacheLength,
                   532'752, 1'057},
};

[[nodiscard]] std::vector<std::uint8_t>
arrayCacheWire(ArrayCacheBank const &bank) {
  bool const maximum = bank.kind == ArrayCacheBankKind::maximumSequential;
  std::vector<std::uint8_t> bytes;
  bytes.reserve(maximum ? std::size_t{1'048'576}
                        : std::size_t{bank.length} * 2 + 2);
  if (maximum)
    bytes.insert(bytes.end(), maximumArrayCacheNopsPerObject, 0x99);
  bytes.push_back(0xF9);
  for (std::uint32_t index = 0; index < bank.length; ++index) {
    bytes.push_back(0xEA);
    if (maximum)
      bytes.insert(bytes.end(), maximumArrayCacheNopsPerObject, 0x99);
    bytes.push_back(0xE1);
  }
  bytes.push_back(0xF1);
  return bytes;
}

[[nodiscard]] std::vector<std::uint8_t>
simpleArrayCacheWire(std::uint32_t length) {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(std::size_t{length} * 2 + 2);
  bytes.push_back(0xF9);
  for (std::uint32_t index = 0; index < length; ++index) {
    bytes.push_back(0xEA);
    bytes.push_back(0xE1);
  }
  bytes.push_back(0xF1);
  return bytes;
}

template <class Observe>
[[nodiscard]] bool forEachArrayCacheIndex(ArrayCacheBank const &bank,
                                          Observe observe) {
  switch (bank.kind) {
  case ArrayCacheBankKind::onePage:
  case ArrayCacheBankKind::pagesUnderOneBranch:
  case ArrayCacheBankKind::maximumSequential:
    for (std::uint32_t index = 0; index < bank.length; ++index) {
      if (!observe(index))
        return false;
    }
    return true;
  case ArrayCacheBankKind::pageUnderEveryBranch:
    for (std::uint32_t branch = 0; branch < 32; ++branch) {
      for (std::uint32_t slot = 0; slot < 32; ++slot) {
        if (!observe(branch * 1'024 + slot))
          return false;
      }
    }
    return true;
  case ArrayCacheBankKind::valueInEveryPage:
    for (std::uint32_t page = 0; page < 1'024; ++page) {
      if (!observe(page * 32))
        return false;
    }
    return true;
  }
  return false;
}

[[nodiscard]] std::size_t logicalChargedBytes(std::size_t requested) {
  AllocationHeader header{requested};
  return testUsableSize(static_cast<void const *>(&header + 1));
}

[[nodiscard]] std::size_t runtimeMallocSize(JSContext *ctx) {
  JSMemoryUsage usage{};
  JS_ComputeMemoryUsage(JS_GetRuntime(ctx), &usage);
  return static_cast<std::size_t>(usage.malloc_size);
}

[[nodiscard]] std::size_t
requestBytes(std::span<AllocationRequest const> requests) {
  std::size_t total = 0;
  for (auto const &request : requests)
    total += request.size;
  return total;
}

class UntouchedArrayFixture {
public:
  UntouchedArrayFixture(JSContext *context, std::vector<std::uint8_t> wire)
      : ctx_(context), wire_(std::move(wire)), root_(context), array_(context),
        atMethod_(context) {}

  [[nodiscard]] bool prepare() {
    root_.reset(types::makeCertifiedObjectCopy(
        ctx_, wire_.data(), static_cast<std::uint32_t>(wire_.size())));
    if (root_.isException())
      return false;
    array_.reset(JS_GetPropertyStr(ctx_, root_.get(), "Memos"));
    if (array_.isException() || !types::isSTArray(array_.get()))
      return false;
    atMethod_.reset(JS_GetPropertyStr(ctx_, array_.get(), "at"));
    return !atMethod_.isException() && JS_IsFunction(ctx_, atMethod_.get());
  }

  [[nodiscard]] JSValue access(std::uint32_t index) const {
    return JS_GetPropertyUint32(ctx_, array_.get(), index);
  }

  [[nodiscard]] JSValue at(std::uint32_t index) const {
    LocalValue argument(ctx_, JS_NewUint32(ctx_, index));
    JSValueConst arguments[] = {argument.get()};
    return JS_Call(ctx_, atMethod_.get(), array_.get(), 1, arguments);
  }

  [[nodiscard]] JSValueConst array() const noexcept { return array_.get(); }

private:
  JSContext *ctx_;
  std::vector<std::uint8_t> wire_;
  LocalValue root_;
  LocalValue array_;
  LocalValue atMethod_;
};

struct TestRadixLedger {
  bool root = false;
  std::array<bool, 32> branches{};
  std::array<std::array<bool, 32>, 32> pages{};
  std::uint32_t branchCount = 0;
  std::uint32_t pageCount = 0;
  std::uint32_t valueCount = 0;
  std::size_t allocationCount = 0;
  std::size_t requestedBytes = 0;
  std::size_t chargedBytes = 0;
  std::size_t childObjectBytes = 0;
};

[[nodiscard]] std::vector<std::size_t>
expectedRadixRequests(TestRadixLedger const &ledger, std::uint32_t index) {
  std::vector<std::size_t> expected;
  std::uint32_t const rootIndex = (index >> 10) & 31;
  std::uint32_t const pageIndex = (index >> 5) & 31;
  if (!ledger.root)
    expected.push_back(arrayCacheRootBytes);
  if (!ledger.branches[rootIndex])
    expected.push_back(arrayCacheBranchBytes);
  if (!ledger.pages[rootIndex][pageIndex])
    expected.push_back(arrayCacheLeafBytes);
  return expected;
}

void publishTestRadixPath(TestRadixLedger &ledger, std::uint32_t index) {
  std::uint32_t const rootIndex = (index >> 10) & 31;
  std::uint32_t const pageIndex = (index >> 5) & 31;
  ledger.root = true;
  if (!ledger.branches[rootIndex]) {
    ledger.branches[rootIndex] = true;
    ++ledger.branchCount;
  }
  if (!ledger.pages[rootIndex][pageIndex]) {
    ledger.pages[rootIndex][pageIndex] = true;
    ++ledger.pageCount;
  }
  ++ledger.valueCount;
}

void expectAllocationFreeStableHit(Runtime &runtime,
                                   UntouchedArrayFixture const &fixture,
                                   std::uint32_t index, JSValueConst expected) {
  auto *ctx = runtime.context();
  std::size_t const liveBefore = runtime.allocations.liveBlocks;
  std::size_t const chargedBefore = runtimeMallocSize(ctx);
  runtime.allocations.startRecording();
  LocalValue numeric(ctx, fixture.access(index));
  runtime.allocations.stopRecording();
  ASSERT_FALSE(numeric.isException());
  EXPECT_EQ(JS_StrictEq(ctx, numeric.get(), expected), 1);
  EXPECT_EQ(runtime.allocations.recordedCount, 0u);
  EXPECT_EQ(runtime.allocations.liveBlocks, liveBefore);
  EXPECT_EQ(runtimeMallocSize(ctx), chargedBefore);

  runtime.allocations.startRecording();
  LocalValue method(ctx, fixture.at(index));
  runtime.allocations.stopRecording();
  ASSERT_FALSE(method.isException());
  EXPECT_EQ(JS_StrictEq(ctx, method.get(), expected), 1);
  EXPECT_EQ(runtime.allocations.recordedCount, 0u);
  EXPECT_EQ(runtime.allocations.liveBlocks, liveBefore);
  EXPECT_EQ(runtimeMallocSize(ctx), chargedBefore);
}

void exerciseArrayCacheBank(ArrayCacheBank const &bank) {
  SCOPED_TRACE(bank.name);
  Runtime runtime;
  ASSERT_TRUE(runtime.open());
  auto *ctx = runtime.context();
  auto wire = arrayCacheWire(bank);
  std::size_t const expectedWire =
      bank.kind == ArrayCacheBankKind::maximumSequential
          ? std::size_t{1'048'576}
          : std::size_t{bank.length} * 2 + 2;
  ASSERT_EQ(wire.size(), expectedWire);
  if (bank.kind == ArrayCacheBankKind::maximumSequential) {
    EXPECT_EQ(bank.length + 1, 32'768u);
    EXPECT_EQ(bank.length + 2, 32'769u);
  }
  TestRadixLedger ledger;
  {
    UntouchedArrayFixture fixture(ctx, std::move(wire));
    ASSERT_TRUE(fixture.prepare());
    bool const complete =
        forEachArrayCacheIndex(bank, [&](std::uint32_t index) {
          SCOPED_TRACE(index);
          auto const expectedCache = expectedRadixRequests(ledger, index);
          std::size_t const liveBefore = runtime.allocations.liveBlocks;
          std::size_t const chargedBefore = runtimeMallocSize(ctx);
          runtime.allocations.startRecording();
          LocalValue first(ctx, fixture.access(index));
          runtime.allocations.stopRecording();
          if (first.isException() || !types::isSTObject(first.get()))
            return false;
          if (runtime.allocations.recordedCount != expectedCache.size() + 3 ||
              runtime.allocations.recordedCount >
                  runtime.allocations.recorded.size())
            return false;
          auto const requests = std::span<AllocationRequest const>{
              runtime.allocations.recorded.data(),
              runtime.allocations.recordedCount};
          for (auto const &request : requests) {
            if (request.kind != RequestKind::allocate ||
                logicalChargedBytes(request.size) != request.size)
              return false;
          }
          if (ledger.childObjectBytes == 0)
            ledger.childObjectBytes = requests[0].size;
          if (requests[0].size != objectHeaderBytes ||
              requests[0].size != ledger.childObjectBytes ||
              requests[1].size != objectPropertyStorageBytes ||
              requests[2].size != objectStateBytes)
            return false;
          for (std::size_t offset = 0; offset < expectedCache.size();
               ++offset) {
            auto const &request = requests[offset + 3];
            if (request.size != expectedCache[offset])
              return false;
            ++ledger.allocationCount;
            ledger.requestedBytes += request.size;
            ledger.chargedBytes += logicalChargedBytes(request.size);
          }
          std::size_t const chargedAfter = runtimeMallocSize(ctx);
          if (chargedAfter - chargedBefore != requestBytes(requests) ||
              runtime.allocations.liveBlocks - liveBefore != requests.size())
            return false;
          publishTestRadixPath(ledger, index);
          expectAllocationFreeStableHit(runtime, fixture, index, first.get());
          return !::testing::Test::HasFailure();
        });
    ASSERT_TRUE(complete);
  }
  EXPECT_EQ(ledger.childObjectBytes, objectHeaderBytes);
  EXPECT_EQ(ledger.branchCount, bank.branches);
  EXPECT_EQ(ledger.pageCount, bank.pages);
  EXPECT_EQ(ledger.valueCount, bank.values);
  EXPECT_EQ(ledger.allocationCount, bank.allocations);
  EXPECT_EQ(ledger.requestedBytes, bank.requestedBytes);
  EXPECT_EQ(ledger.chargedBytes, bank.requestedBytes)
      << "the custom allocator stores the logical request in AllocationHeader "
         "and testUsableSize returns that exact charge";

  runtime.close();
  EXPECT_EQ(runtime.allocations.liveBlocks, 0u);
}

TEST(ObjectArrayRadixAllocation,
     ExactFiveFiveFiveBanksFreezeChronologyRequestsAndLogicalCharges) {
  ASSERT_EQ(sizeof(void *), 8u);
  ASSERT_EQ(sizeof(JSValue), 16u);
  for (auto const &bank : arrayCacheBanks)
    exerciseArrayCacheBank(bank);
}

enum class ArrayMissShape : std::uint8_t { rootBranchLeaf, branchLeaf, leaf };

[[nodiscard]] char const *arrayMissShapeName(ArrayMissShape shape) noexcept {
  switch (shape) {
  case ArrayMissShape::rootBranchLeaf:
    return "root/branch/leaf";
  case ArrayMissShape::branchLeaf:
    return "branch/leaf";
  case ArrayMissShape::leaf:
    return "leaf";
  }
  return "unknown";
}

[[nodiscard]] std::uint32_t arrayMissTarget(ArrayMissShape shape) noexcept {
  switch (shape) {
  case ArrayMissShape::rootBranchLeaf:
    return 0;
  case ArrayMissShape::branchLeaf:
    return 1'024;
  case ArrayMissShape::leaf:
    return 32;
  }
  return 0;
}

[[nodiscard]] std::vector<std::size_t>
arrayMissCacheRequests(ArrayMissShape shape) {
  switch (shape) {
  case ArrayMissShape::rootBranchLeaf:
    return {arrayCacheRootBytes, arrayCacheBranchBytes, arrayCacheLeafBytes};
  case ArrayMissShape::branchLeaf:
    return {arrayCacheBranchBytes, arrayCacheLeafBytes};
  case ArrayMissShape::leaf:
    return {arrayCacheLeafBytes};
  }
  return {};
}

[[nodiscard]] bool seedArrayMissShape(JSContext *ctx,
                                      UntouchedArrayFixture const &fixture,
                                      ArrayMissShape shape,
                                      LocalValue &seedIdentity) {
  if (shape == ArrayMissShape::rootBranchLeaf)
    return true;
  seedIdentity.reset(fixture.access(0));
  return !seedIdentity.isException() && types::isSTObject(seedIdentity.get()) &&
         !JS_HasException(ctx);
}

void expectExactMissProfile(std::span<AllocationRequest const> requests,
                            ArrayMissShape shape,
                            std::size_t childObjectBytes) {
  auto const cache = arrayMissCacheRequests(shape);
  ASSERT_EQ(requests.size(), cache.size() + 3);
  EXPECT_EQ(requests[0],
            (AllocationRequest{childObjectBytes, RequestKind::allocate}));
  EXPECT_EQ(requests[1], (AllocationRequest{objectPropertyStorageBytes,
                                            RequestKind::allocate}));
  EXPECT_EQ(requests[2],
            (AllocationRequest{objectStateBytes, RequestKind::allocate}));
  for (std::size_t index = 0; index < cache.size(); ++index) {
    EXPECT_EQ(requests[index + 3],
              (AllocationRequest{cache[index], RequestKind::allocate}));
  }
  for (auto const &request : requests) {
    EXPECT_EQ(logicalChargedBytes(request.size), request.size)
        << "logical usable-size charging must equal the chronological request";
  }
}

void exerciseArrayMissOOM(ArrayMissShape shape) {
  SCOPED_TRACE(arrayMissShapeName(shape));
  std::vector<AllocationRequest> successfulRequests;
  std::size_t childObjectBytes = 0;
  {
    Runtime measured;
    ASSERT_TRUE(measured.open());
    auto *ctx = measured.context();
    {
      UntouchedArrayFixture fixture(ctx, simpleArrayCacheWire(1'025));
      ASSERT_TRUE(fixture.prepare());
      LocalValue seed(ctx);
      ASSERT_TRUE(seedArrayMissShape(ctx, fixture, shape, seed));
      std::size_t const chargedBefore = runtimeMallocSize(ctx);
      measured.allocations.startRecording();
      LocalValue value(ctx, fixture.access(arrayMissTarget(shape)));
      measured.allocations.stopRecording();
      ASSERT_FALSE(value.isException());
      ASSERT_LE(measured.allocations.recordedCount,
                measured.allocations.recorded.size());
      successfulRequests.assign(measured.allocations.recorded.begin(),
                                measured.allocations.recorded.begin() +
                                    measured.allocations.recordedCount);
      ASSERT_GE(successfulRequests.size(), 2u);
      childObjectBytes = successfulRequests[0].size;
      EXPECT_EQ(childObjectBytes, objectHeaderBytes);
      expectExactMissProfile(successfulRequests, shape, childObjectBytes);
      EXPECT_EQ(runtimeMallocSize(ctx) - chargedBefore,
                requestBytes(successfulRequests));
    }
    measured.close();
    EXPECT_EQ(measured.allocations.liveBlocks, 0u);
  }

  for (std::size_t ordinal = 1; ordinal <= successfulRequests.size();
       ++ordinal) {
    SCOPED_TRACE(ordinal);
    Runtime injected;
    ASSERT_TRUE(injected.open());
    auto *ctx = injected.context();
    {
      UntouchedArrayFixture fixture(ctx, simpleArrayCacheWire(1'025));
      ASSERT_TRUE(fixture.prepare());
      LocalValue seed(ctx);
      ASSERT_TRUE(seedArrayMissShape(ctx, fixture, shape, seed));
      warmExactOOMDiagnostic(ctx);
      std::size_t const liveBefore = injected.allocations.liveBlocks;
      std::size_t const chargedBefore = runtimeMallocSize(ctx);
      std::size_t const rejectionsBefore = injected.allocations.rejections;

      injected.allocations.startRecording();
      injected.allocations.rejectRelative(ordinal);
      LocalValue failed(ctx, fixture.access(arrayMissTarget(shape)));
      injected.allocations.stopRecording();
      injected.allocations.disarm();
      ASSERT_EQ(injected.allocations.rejections, rejectionsBefore + 1);
      ASSERT_GE(injected.allocations.recordedCount, ordinal);
      for (std::size_t index = 0; index < ordinal; ++index)
        EXPECT_EQ(injected.allocations.recorded[index],
                  successfulRequests[index]);
      expectExactOOM(ctx, failed.get(), ordinal);
      failed.reset();
      EXPECT_EQ(injected.allocations.liveBlocks, liveBefore)
          << "no child or partial radix path may publish";
      EXPECT_EQ(runtimeMallocSize(ctx), chargedBefore)
          << "every private child/root/branch/leaf allocation must roll back";
      if (shape != ArrayMissShape::rootBranchLeaf) {
        LocalValue seedHit(ctx, fixture.access(0));
        ASSERT_FALSE(seedHit.isException());
        EXPECT_EQ(JS_StrictEq(ctx, seedHit.get(), seed.get()), 1)
            << "the previously published identity must survive miss OOM";
      }

      injected.allocations.startRecording();
      std::size_t const retryChargedBefore = runtimeMallocSize(ctx);
      LocalValue retry(ctx, fixture.access(arrayMissTarget(shape)));
      injected.allocations.stopRecording();
      ASSERT_FALSE(retry.isException());
      ASSERT_EQ(injected.allocations.recordedCount, successfulRequests.size())
          << "same-runtime retry must rebuild the complete missing path";
      auto const retryRequests = std::span<AllocationRequest const>{
          injected.allocations.recorded.data(),
          injected.allocations.recordedCount};
      expectExactMissProfile(retryRequests, shape, childObjectBytes);
      EXPECT_TRUE(std::equal(retryRequests.begin(), retryRequests.end(),
                             successfulRequests.begin(),
                             successfulRequests.end()));
      EXPECT_EQ(runtimeMallocSize(ctx) - retryChargedBefore,
                requestBytes(retryRequests));
      expectAllocationFreeStableHit(injected, fixture, arrayMissTarget(shape),
                                    retry.get());
    }
    injected.close();
    EXPECT_EQ(injected.allocations.liveBlocks, 0u);
  }
}

TEST(ObjectArrayRadixOOM,
     ChildObjectStateAndRootBranchLeafFailuresAreAtomicAndRetryable) {
  for (auto const shape : {ArrayMissShape::rootBranchLeaf,
                           ArrayMissShape::branchLeaf, ArrayMissShape::leaf})
    exerciseArrayMissOOM(shape);
}

} // namespace
