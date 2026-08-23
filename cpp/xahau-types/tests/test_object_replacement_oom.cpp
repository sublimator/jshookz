#include "js.hpp"
#include "object/object.hpp"
#include "result.hpp"

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

extern "C" bool register_cpp_types(JSContext *ctx);
extern "C" void unregister_cpp_types(JSRuntime *runtime);
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
    return jshookz::provider::bindings::registerResult(context_) &&
           register_cpp_types(context_) && register_uint_types(context_) &&
           !JS_HasException(context_);
  }

  void close() {
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

  AllocatorControl allocations;

private:
  JSRuntime *runtime_ = nullptr;
  JSContext *context_ = nullptr;
};

[[nodiscard]] std::vector<std::uint8_t> sourceWire() {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(50);
  for (std::uint8_t nth = 1; nth <= 10; ++nth) {
    bytes.push_back(static_cast<std::uint8_t>(0x20U | nth));
    bytes.push_back(0);
    bytes.push_back(0);
    bytes.push_back(0);
    bytes.push_back(nth);
  }
  return bytes;
}

constexpr std::array<std::uint8_t, 9> certifiedArrayPayload = {
    0xEA, 0xE1, 0xEA, 0xE1, 0xEA, 0xE1, 0xEA, 0xE1, 0xF1,
};

[[nodiscard]] std::vector<std::uint8_t> carrierWire() {
  return {
      0xF9,
      certifiedArrayPayload[0],
      certifiedArrayPayload[1],
      certifiedArrayPayload[2],
      certifiedArrayPayload[3],
      certifiedArrayPayload[4],
      certifiedArrayPayload[5],
      certifiedArrayPayload[6],
      certifiedArrayPayload[7],
      certifiedArrayPayload[8],
  };
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

enum class ScenarioKind : std::uint8_t {
  rawLeaf,
  certifiedObject,
  certifiedArray,
  richNominal,
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
  bool fieldScratch = false;
  bool scopeScratch = false;
};

[[nodiscard]] ExpectedProfile expectedProfile(ScenarioKind kind) noexcept {
  switch (kind) {
  case ScenarioKind::rawLeaf:
    return {50, 10, 1, true, false};
  case ScenarioKind::certifiedObject:
    return {52, 11, 2, true, false};
  case ScenarioKind::certifiedArray:
    return {60, 15, 6, true, true};
  case ScenarioKind::richNominal:
    return {72, 11, 1, true, false};
  case ScenarioKind::presentRemoval:
    return {45, 9, 1, true, false};
  case ScenarioKind::absentRemoval:
    return {};
  }
  return {};
}

class PreparedScenario {
public:
  PreparedScenario(JSContext *context, ScenarioKind scenarioKind)
      : ctx_(context), kind_(scenarioKind), source_(context), method_(context),
        field_(context), argument_(context), carrier_(context),
        certifiedArray_(context), certifiedObject_(context) {}

  [[nodiscard]] bool prepare() {
    sourceExpected_ = sourceWire();
    source_.reset(types::makeCertifiedObjectCopy(
        ctx_, sourceExpected_.data(),
        static_cast<std::uint32_t>(sourceExpected_.size())));
    if (source_.isException())
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
        derivedExpected_.insert(derivedExpected_.end(),
                                certifiedArrayPayload.begin(),
                                certifiedArrayPayload.end());
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
    field_.reset(JS_NewUint32(ctx_, fieldCode_));
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
      EXPECT_EQ(bytes,
                (std::vector<std::uint8_t>(certifiedArrayPayload.begin(),
                                           certifiedArrayPayload.end())));
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
  LocalValue carrier_;
  LocalValue certifiedArray_;
  LocalValue certifiedObject_;
  std::uint32_t fieldCode_ = 0;
  std::vector<std::uint8_t> sourceExpected_;
  std::vector<std::uint8_t> derivedExpected_;
  std::array<std::uint8_t, 20> accountExpected_{};
};

[[nodiscard]] bool containsRequest(std::span<AllocationRequest const> requests,
                                   std::size_t size) noexcept {
  return std::ranges::any_of(requests,
                             [size](AllocationRequest const &request) {
                               return request.size == size;
                             });
}

void expectNamedAllocationProfile(ScenarioKind kind,
                                  std::span<AllocationRequest const> requests) {
  auto const expected = expectedProfile(kind);
  ASSERT_FALSE(requests.empty()) << scenarioName(kind);
  EXPECT_EQ(requests.front().size, expected.outputSize)
      << scenarioName(kind) << ": output buffer must be first";
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
  if (expected.fieldScratch) {
    EXPECT_TRUE(containsRequest(requests, 16 * 28))
        << scenarioName(kind) << ": spilled field scratch";
  }
  if (expected.scopeScratch) {
    EXPECT_TRUE(containsRequest(requests, 8 * 16))
        << scenarioName(kind) << ": spilled scope scratch";
  }
}

TEST(ObjectReplacementOOM,
     EverySuccessfulRouteAllocationFailsAtomicallyAndRetriesInSameRuntime) {
  constexpr std::array scenarios = {
      ScenarioKind::rawLeaf,        ScenarioKind::certifiedObject,
      ScenarioKind::certifiedArray, ScenarioKind::richNominal,
      ScenarioKind::presentRemoval,
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

} // namespace
