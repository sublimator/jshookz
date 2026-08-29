#include "amount/amount_js.hpp"
#include "js.hpp"
#include "leaf/leaf.hpp"
#include "object/nominal_payload.hpp"
#include "object/object.hpp"
#include "pathset/pathset_js.hpp"
#include "result.hpp"

#include "runtime_type.hpp"

#include <jshookz/qjs.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

extern "C" bool register_uint_types(JSContext *ctx);

namespace {

namespace qjs = jshookz::qjs;
namespace types = jshookz::provider::types;
namespace xdata = catl::xdata;

struct alignas(std::max_align_t) AllocationHeader {
  std::size_t size = 0;
};

struct AllocationControl {
  std::size_t requests = 0;
  std::size_t liveBlocks = 0;
  std::size_t rejectAt = 0;
  std::size_t rejections = 0;
  bool recording = false;
  std::size_t recordedRequests = 0;
  std::size_t maximumRecordedRequest = 0;
  std::size_t secondLargestRecordedRequest = 0;

  void startRecording() noexcept {
    recordedRequests = 0;
    maximumRecordedRequest = 0;
    secondLargestRecordedRequest = 0;
    recording = true;
  }

  void stopRecording() noexcept { recording = false; }

  void record(std::size_t size) noexcept {
    if (!recording)
      return;
    ++recordedRequests;
    if (size > maximumRecordedRequest) {
      secondLargestRecordedRequest = maximumRecordedRequest;
      maximumRecordedRequest = size;
    } else if (size > secondLargestRecordedRequest) {
      secondLargestRecordedRequest = size;
    }
  }
};

[[nodiscard]] bool rejectRequest(AllocationControl *control) noexcept {
  ++control->requests;
  if (control->rejectAt == 0 || control->requests != control->rejectAt)
    return false;
  control->rejectAt = 0;
  ++control->rejections;
  return true;
}

[[nodiscard]] bool wouldExceed(JSMallocState const *state, std::size_t oldSize,
                               std::size_t newSize) noexcept {
  if (state->malloc_size < oldSize)
    return true;
  std::size_t const retained = state->malloc_size - oldSize;
  return newSize > state->malloc_limit ||
         retained > state->malloc_limit - newSize;
}

void *countingMalloc(JSMallocState *state, std::size_t size) {
  auto *control = static_cast<AllocationControl *>(state->opaque);
  if (rejectRequest(control))
    return nullptr;
  control->record(size);
  if (size == 0 ||
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

void countingFree(JSMallocState *state, void *pointer) {
  if (pointer == nullptr)
    return;
  auto *control = static_cast<AllocationControl *>(state->opaque);
  auto *header = static_cast<AllocationHeader *>(pointer) - 1;
  --state->malloc_count;
  state->malloc_size -= header->size;
  --control->liveBlocks;
  std::free(header);
}

void *countingRealloc(JSMallocState *state, void *pointer, std::size_t size) {
  if (pointer == nullptr)
    return size == 0 ? nullptr : countingMalloc(state, size);
  if (size == 0) {
    countingFree(state, pointer);
    return nullptr;
  }
  auto *control = static_cast<AllocationControl *>(state->opaque);
  if (rejectRequest(control))
    return nullptr;
  control->record(size);
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

[[nodiscard]] std::size_t countingUsableSize(void const *pointer) {
  if (pointer == nullptr)
    return 0;
  return (static_cast<AllocationHeader const *>(pointer) - 1)->size;
}

constexpr JSMallocFunctions countingAllocator = {
    .js_malloc = countingMalloc,
    .js_free = countingFree,
    .js_realloc = countingRealloc,
    .js_malloc_usable_size = countingUsableSize,
};

[[nodiscard]] JSValue makeIssueForAmount(JSContext *ctx, JSValueConst,
                                         types::AmountIssueKind kind,
                                         std::uint8_t const *identity,
                                         std::uint32_t length) {
  if (kind == types::AmountIssueKind::native) {
    std::uint8_t native[20] = {};
    return types::makeIssueBytes(ctx, native, sizeof(native));
  }
  if (kind == types::AmountIssueKind::iou)
    return types::makeIssueBytes(ctx, identity, length);
  if (identity == nullptr || length != 24)
    return JS_ThrowInternalError(ctx, "invalid MPT issue identity");
  std::uint8_t issue[44] = {};
  std::memcpy(issue, identity + 4, 20);
  issue[39] = 1;
  issue[40] = identity[3];
  issue[41] = identity[2];
  issue[42] = identity[1];
  issue[43] = identity[0];
  return types::makeIssueBytes(ctx, issue, sizeof(issue));
}

[[nodiscard]] std::vector<std::uint8_t> pattern(std::size_t size,
                                                std::uint8_t seed) {
  std::vector<std::uint8_t> bytes(size);
  for (std::size_t i = 0; i < size; ++i)
    bytes[i] = static_cast<std::uint8_t>(seed + i);
  return bytes;
}

[[nodiscard]] std::string stringProperty(JSContext *context,
                                         JSValueConst object,
                                         char const *name) {
  qjs::OwnedValue value(context, JS_GetPropertyStr(context, object, name));
  if (value.isException())
    return {};
  char const *text = JS_ToCString(context, value.get());
  if (text == nullptr)
    return {};
  std::string result{text};
  JS_FreeCString(context, text);
  return result;
}

struct NominalCase {
  char const *name;
  xdata::MaterializerKind kind;
  std::vector<std::uint8_t> expected;
  qjs::OwnedValue value;

  NominalCase(JSContext *ctx, char const *caseName,
              xdata::MaterializerKind caseKind, std::vector<std::uint8_t> bytes,
              JSValue caseValue)
      : name(caseName), kind(caseKind), expected(std::move(bytes)),
        value(ctx, caseValue) {}

  NominalCase(NominalCase &&) noexcept = default;
  NominalCase &operator=(NominalCase &&) noexcept = default;
};

class NominalPayloadTest : public ::testing::Test {
protected:
  AllocationControl allocations;
  JSRuntime *runtime = nullptr;
  JSContext *context = nullptr;

  void SetUp() override {
    runtime = JS_NewRuntime2(&countingAllocator, &allocations);
    ASSERT_NE(runtime, nullptr);
    context = JS_NewContext(runtime);
    ASSERT_NE(context, nullptr);
    ASSERT_TRUE(jshookz::provider::bindings::registerResult(context));
    jshookz::provider::qjs::resetByteClassRegistry();

    qjs::OwnedValue global(context, JS_GetGlobalObject(context));
    ASSERT_FALSE(global.isException());
    ASSERT_TRUE(types::registerSTBlob(context, global.get()));
    ASSERT_TRUE(types::registerHash256(context, global.get()));
    ASSERT_TRUE(types::registerAccountID(context, global.get()));
    ASSERT_TRUE(types::registerXFL(context));
    ASSERT_TRUE(types::registerRichLeafTypes(context));
    types::AmountLeafMaterializers const amountLeaves{
        types::makeAccountIDView, types::makeCurrencyView,
        types::makeHash192View,   types::makeXFLDecimalParts,
        makeIssueForAmount,
    };
    ASSERT_TRUE(types::registerAmount(context, amountLeaves));
    ASSERT_TRUE(types::publishAmountFactory(context, global.get()));
    ASSERT_TRUE(types::registerObjectTypes(context));
    types::PathSetLeafMaterializers const pathLeaves{
        types::makeAccountIDView,
        types::makeCurrencyView,
        types::isCertifiedObjectRange,
    };
    ASSERT_TRUE(types::registerPathSet(context, pathLeaves));
    ASSERT_TRUE(register_uint_types(context));
    ASSERT_FALSE(JS_HasException(context));
  }

  void TearDown() override {
    if (context != nullptr)
      JS_FreeContext(context);
    if (runtime != nullptr) {
      types::unregisterObjectTypes(runtime);
      types::unregisterRichLeafTypes(runtime);
      jshookz::provider::qjs::resetByteClassRegistry();
      JS_FreeRuntime(runtime);
    }
    EXPECT_EQ(allocations.liveBlocks, 0u);
  }

  [[nodiscard]] qjs::OwnedValue eval(char const *source) {
    return qjs::OwnedValue(
        context, JS_Eval(context, source, std::strlen(source),
                         "<nominal-payload-test>", JS_EVAL_TYPE_GLOBAL));
  }

  [[nodiscard]] qjs::OwnedValue makePathSet() {
    std::array<std::uint8_t, 24> wire{};
    wire[0] = 0x01;
    wire[1] = 0x12;
    wire[2] = 0x01;
    for (std::uint32_t i = 0; i < 20; ++i)
      wire[3 + i] = static_cast<std::uint8_t>(0x40 + i);
    wire[23] = 0x00;
    qjs::OwnedValue root(context, types::makeCertifiedObjectCopy(
                                      context, wire.data(),
                                      static_cast<std::uint32_t>(wire.size())));
    if (root.isException())
      return root;
    return qjs::OwnedValue(context,
                           JS_GetPropertyStr(context, root.get(), "Paths"));
  }

  void clearException() {
    if (JS_HasException(context))
      JS_FreeValue(context, JS_GetException(context));
  }
};

TEST_F(NominalPayloadTest,
       AcceptsEveryExactRichKindAndRejectsTheFullWrongKindMatrix) {
  std::vector<NominalCase> cases;
  cases.reserve(16);

  auto add = [&](char const *name, xdata::MaterializerKind kind,
                 std::vector<std::uint8_t> bytes, JSValue value) {
    EXPECT_FALSE(JS_IsException(value)) << name;
    if (!JS_IsException(value))
      cases.emplace_back(context, name, kind, std::move(bytes), value);
  };

  add("UInt8", xdata::MaterializerKind::uint8, {0xab},
      types::makeUIntValue(context, 8, 0xab));
  add("UInt16", xdata::MaterializerKind::uint16, {0xbe, 0xef},
      types::makeUIntValue(context, 16, 0xbeef));
  add("UInt32", xdata::MaterializerKind::uint32, {1, 2, 3, 4},
      types::makeUIntValue(context, 32, 0x01020304));
  add("UInt64", xdata::MaterializerKind::uint64, {1, 2, 3, 4, 5, 6, 7, 8},
      types::makeUIntValue(context, 64, 0x0102030405060708ULL));

  auto hash128 = pattern(16, 0x10);
  add("Hash128", xdata::MaterializerKind::hash128, hash128,
      types::makeHash128Bytes(context, hash128.data(),
                              static_cast<std::uint32_t>(hash128.size())));
  auto hash160 = pattern(20, 0x20);
  add("Hash160", xdata::MaterializerKind::hash160, hash160,
      types::makeHash160Bytes(context, hash160.data(),
                              static_cast<std::uint32_t>(hash160.size())));
  auto hash192 = pattern(24, 0x30);
  add("Hash192", xdata::MaterializerKind::hash192, hash192,
      types::makeHash192Bytes(context, hash192.data(),
                              static_cast<std::uint32_t>(hash192.size())));
  auto hash256 = pattern(32, 0x40);
  add("Hash256", xdata::MaterializerKind::hash256, hash256,
      types::makeHash256Bytes(context, hash256.data(),
                              static_cast<std::uint32_t>(hash256.size())));

  auto blob = pattern(37, 0x50);
  add("STBlob", xdata::MaterializerKind::blob, blob,
      types::makeSTBlobBytes(context, blob.data(),
                             static_cast<std::uint32_t>(blob.size())));
  auto account = pattern(20, 0x60);
  add("AccountID", xdata::MaterializerKind::account_id, account,
      types::makeAccountIDBytes(context, account.data(),
                                static_cast<std::uint32_t>(account.size())));

  std::vector<std::uint8_t> amount{0x40, 0, 0, 0, 0, 0, 0, 42};
  add("Amount", xdata::MaterializerKind::amount, amount,
      types::makeAmountBytes(context, amount.data(),
                             static_cast<std::uint32_t>(amount.size())));
  std::vector<std::uint8_t> currency(20);
  currency[12] = 'U';
  currency[13] = 'S';
  currency[14] = 'D';
  add("Currency", xdata::MaterializerKind::currency, currency,
      types::makeCurrencyBytes(context, currency.data(),
                               static_cast<std::uint32_t>(currency.size())));
  std::vector<std::uint8_t> issue = currency;
  auto issuer = pattern(20, 0x70);
  issue.insert(issue.end(), issuer.begin(), issuer.end());
  add("Issue", xdata::MaterializerKind::issue, issue,
      types::makeIssueBytes(context, issue.data(),
                            static_cast<std::uint32_t>(issue.size())));

  auto pathSet = makePathSet();
  ASSERT_FALSE(pathSet.isException());
  std::vector<std::uint8_t> pathBytes(22);
  pathBytes[0] = 0x01;
  for (std::uint32_t i = 0; i < 20; ++i)
    pathBytes[1 + i] = static_cast<std::uint8_t>(0x40 + i);
  pathBytes[21] = 0x00;
  add("PathSet", xdata::MaterializerKind::path_set, pathBytes,
      pathSet.release());

  auto vector = pattern(64, 0x80);
  add("Vector256", xdata::MaterializerKind::vector256, vector,
      types::makeVector256Bytes(context, vector.data(),
                                static_cast<std::uint32_t>(vector.size())));
  std::vector<std::uint8_t> bridge(82);
  bridge[0] = 20;
  std::copy(account.begin(), account.end(), bridge.begin() + 1);
  bridge[41] = 20;
  std::copy(issuer.begin(), issuer.end(), bridge.begin() + 42);
  add("XChainBridge", xdata::MaterializerKind::xchain_bridge, bridge,
      types::makeXChainBridgeBytes(context, bridge.data(),
                                   static_cast<std::uint32_t>(bridge.size())));

  ASSERT_EQ(cases.size(), 16u);
  for (std::size_t i = 0; i < cases.size(); ++i) {
    SCOPED_TRACE(cases[i].name);
    std::uint8_t scratch[8];
    std::memset(scratch, 0xa5, sizeof(scratch));
    types::NominalPayloadView payload{
        scratch, std::numeric_limits<std::uint32_t>::max()};
    auto const requestsBefore = allocations.requests;
    bool const accepted = types::readNominalPayload(
        context, cases[i].value.get(), cases[i].kind, scratch, payload);
    auto const requestsAfter = allocations.requests;
    EXPECT_TRUE(accepted);
    EXPECT_EQ(requestsAfter, requestsBefore);
    ASSERT_EQ(payload.size, cases[i].expected.size());
    EXPECT_EQ(std::memcmp(payload.data, cases[i].expected.data(), payload.size),
              0);

    for (std::size_t j = 0; j < cases.size(); ++j) {
      if (i == j)
        continue;
      std::memset(scratch, 0xa5, sizeof(scratch));
      payload = {scratch, std::numeric_limits<std::uint32_t>::max()};
      auto const mismatchRequestsBefore = allocations.requests;
      bool const mismatch = types::readNominalPayload(
          context, cases[i].value.get(), cases[j].kind, scratch, payload);
      auto const mismatchRequestsAfter = allocations.requests;
      EXPECT_FALSE(mismatch) << cases[j].name;
      EXPECT_EQ(mismatchRequestsAfter, mismatchRequestsBefore);
      EXPECT_EQ(payload.data, nullptr);
      EXPECT_EQ(payload.size, 0u);
      EXPECT_EQ(std::count(std::begin(scratch), std::end(scratch), 0), 8);
    }
  }
  EXPECT_FALSE(JS_HasException(context));
}

TEST_F(NominalPayloadTest,
       AmountDropsPropagatesEveryAllocationFailureAndRetriesNominally) {
  qjs::OwnedValue global(context, JS_GetGlobalObject(context));
  qjs::OwnedValue factory(
      context, JS_GetPropertyStr(context, global.get(), "Amount"));
  qjs::OwnedValue function(
      context, JS_GetPropertyStr(context, factory.get(), "drops"));
  qjs::OwnedValue argument(context, JS_NewBigInt64(context, 1));
  ASSERT_FALSE(global.isException());
  ASSERT_FALSE(factory.isException());
  ASSERT_FALSE(function.isException());
  ASSERT_FALSE(argument.isException());
  ASSERT_TRUE(JS_IsFunction(context, function.get()));

  auto invoke = [&]() {
    JSValueConst arguments[] = {argument.get()};
    return qjs::OwnedValue(
        context,
        JS_Call(context, function.get(), factory.get(), 1, arguments));
  };

  {
    auto warm = invoke();
    ASSERT_FALSE(warm.isException());
  }

  allocations.startRecording();
  std::size_t const measuredBefore = allocations.requests;
  {
    auto measured = invoke();
    ASSERT_FALSE(measured.isException());
  }
  std::size_t const requestCount = allocations.requests - measuredBefore;
  allocations.stopRecording();
  ASSERT_EQ(allocations.recordedRequests, requestCount);
  ASSERT_GT(requestCount, 0u);

  for (std::size_t ordinal = 1; ordinal <= requestCount; ++ordinal) {
    SCOPED_TRACE(ordinal);
    std::size_t const liveBefore = allocations.liveBlocks;
    std::size_t const rejectionsBefore = allocations.rejections;
    allocations.rejectAt = allocations.requests + ordinal;
    {
      auto failed = invoke();
      allocations.rejectAt = 0;
      ASSERT_TRUE(failed.isException());
      ASSERT_EQ(allocations.rejections, rejectionsBefore + 1);
      ASSERT_TRUE(JS_HasException(context));
      qjs::OwnedValue exception(context, JS_GetException(context));
      ASSERT_TRUE(JS_IsError(context, exception.get()));
      EXPECT_EQ(stringProperty(context, exception.get(), "name"),
                "InternalError");
      EXPECT_EQ(stringProperty(context, exception.get(), "message"),
                "out of memory");
      ASSERT_FALSE(JS_HasException(context));
    }
    EXPECT_EQ(allocations.liveBlocks, liveBefore);

    auto retry = invoke();
    ASSERT_FALSE(retry.isException());
    EXPECT_TRUE(types::isAmountKind(retry.get(), types::AmountIssueKind::native));
    std::uint8_t scratch[8]{};
    types::NominalPayloadView payload;
    ASSERT_TRUE(types::readNominalPayload(
        context, retry.get(), xdata::MaterializerKind::amount, scratch,
        payload));
    std::array<std::uint8_t, 8> const expected{
        0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    ASSERT_EQ(payload.size, expected.size());
    EXPECT_EQ(std::memcmp(payload.data, expected.data(), expected.size()), 0);
  }
  EXPECT_FALSE(JS_HasException(context));
}

TEST_F(NominalPayloadTest,
    RuntimeTypeClassificationIsAllocationFreeAfterRegistration)
{
    qjs::OwnedValue global(context, JS_GetGlobalObject(context));
    ASSERT_FALSE(global.isException());
    qjs::OwnedValue stBlobType(
        context, JS_GetPropertyStr(context, global.get(), "STBlob"));
    qjs::OwnedValue uintRoot(
        context, JS_GetPropertyStr(context, global.get(), "UInt"));
    qjs::OwnedValue uint8Type(
        context, JS_GetPropertyStr(context, global.get(), "UInt8"));
    ASSERT_FALSE(stBlobType.isException());
    ASSERT_FALSE(uintRoot.isException());
    ASSERT_FALSE(uint8Type.isException());

    qjs::OwnedValue blob(context, types::makeSTBlobBytes(context, nullptr, 0));
    qjs::OwnedValue uint8(context, types::makeUIntValue(context, 8, 7));
    qjs::OwnedValue uint16(context, types::makeUIntValue(context, 16, 7));
    ASSERT_FALSE(blob.isException());
    ASSERT_FALSE(uint8.isException());
    ASSERT_FALSE(uint16.isException());

    // Warm the ordinary-object Symbol.hasInstance lookup before measuring.
    ASSERT_EQ(JS_IsInstanceOf(context, blob.get(), stBlobType.get()), 1);
    ASSERT_EQ(JS_IsInstanceOf(context, uint8.get(), uintRoot.get()), 1);
    ASSERT_EQ(JS_IsInstanceOf(context, uint8.get(), uint8Type.get()), 1);
    ASSERT_EQ(JS_IsInstanceOf(context, uint16.get(), uint8Type.get()), 0);
    ASSERT_FALSE(JS_HasException(context));

    auto const requestsBefore = allocations.requests;
    for (std::uint32_t iteration = 0; iteration < 4096; ++iteration)
    {
        ASSERT_EQ(JS_IsInstanceOf(context, blob.get(), stBlobType.get()), 1);
        ASSERT_EQ(JS_IsInstanceOf(context, uint8.get(), uintRoot.get()), 1);
        ASSERT_EQ(JS_IsInstanceOf(context, uint8.get(), uint8Type.get()), 1);
        ASSERT_EQ(JS_IsInstanceOf(context, uint16.get(), uint8Type.get()), 0);
        ASSERT_TRUE(types::runtimeTypeClassifies(
            types::RuntimeTypeId::stBlob, blob.get()));
        ASSERT_TRUE(types::runtimeTypeClassifies(
            types::RuntimeTypeId::uInt, uint16.get()));
        ASSERT_FALSE(types::runtimeTypeClassifies(
            types::RuntimeTypeId::uInt8, uint16.get()));
    }
    EXPECT_EQ(allocations.requests, requestsBefore);
    EXPECT_FALSE(JS_HasException(context));
}

TEST_F(NominalPayloadTest, PreservesEmptyAndVariableCanonicalPayloads) {
  struct Vector {
    xdata::MaterializerKind kind;
    std::vector<std::uint8_t> expected;
    qjs::OwnedValue value;
  };
  std::vector<Vector> vectors;

  vectors.push_back(
      {xdata::MaterializerKind::blob,
       {},
       qjs::OwnedValue(context, types::makeSTBlobBytes(context, nullptr, 0))});
  vectors.push_back({xdata::MaterializerKind::vector256,
                     {},
                     qjs::OwnedValue(context, types::makeVector256Bytes(
                                                  context, nullptr, 0))});

  std::vector<std::uint8_t> iouAmount(48);
  std::array<std::uint8_t, 8> iouPrefix{0xc0, 0x43, 0x8d, 0x7e,
                                        0xa4, 0xc6, 0x80, 0x00};
  std::copy(iouPrefix.begin(), iouPrefix.end(), iouAmount.begin());
  iouAmount[20] = 'U';
  iouAmount[21] = 'S';
  iouAmount[22] = 'D';
  auto amountIssuer = pattern(20, 0x31);
  std::copy(amountIssuer.begin(), amountIssuer.end(), iouAmount.begin() + 28);
  vectors.push_back({xdata::MaterializerKind::amount, iouAmount,
                     qjs::OwnedValue(context, types::makeAmountBytes(
                                                  context, iouAmount.data(),
                                                  static_cast<std::uint32_t>(
                                                      iouAmount.size())))});

  std::vector<std::uint8_t> mptAmount(33);
  mptAmount[0] = 0x20;
  mptAmount[8] = 1;
  vectors.push_back({xdata::MaterializerKind::amount, mptAmount,
                     qjs::OwnedValue(context, types::makeAmountBytes(
                                                  context, mptAmount.data(),
                                                  static_cast<std::uint32_t>(
                                                      mptAmount.size())))});

  std::vector<std::uint8_t> nativeIssue(20);
  vectors.push_back({xdata::MaterializerKind::issue, nativeIssue,
                     qjs::OwnedValue(context, types::makeIssueBytes(
                                                  context, nativeIssue.data(),
                                                  static_cast<std::uint32_t>(
                                                      nativeIssue.size())))});
  std::vector<std::uint8_t> mptIssue(44);
  mptIssue[0] = 1;
  mptIssue[39] = 1;
  vectors.push_back({xdata::MaterializerKind::issue, mptIssue,
                     qjs::OwnedValue(context, types::makeIssueBytes(
                                                  context, mptIssue.data(),
                                                  static_cast<std::uint32_t>(
                                                      mptIssue.size())))});

  std::vector<std::uint8_t> compactBridge(42);
  vectors.push_back({xdata::MaterializerKind::xchain_bridge, compactBridge,
                     qjs::OwnedValue(context, types::makeXChainBridgeBytes(
                                                  context, compactBridge.data(),
                                                  static_cast<std::uint32_t>(
                                                      compactBridge.size())))});

  for (auto const &vector : vectors) {
    ASSERT_FALSE(vector.value.isException());
    std::uint8_t scratch[8];
    types::NominalPayloadView payload;
    auto const requestsBefore = allocations.requests;
    ASSERT_TRUE(types::readNominalPayload(context, vector.value.get(),
                                          vector.kind, scratch, payload));
    auto const requestsAfter = allocations.requests;
    EXPECT_EQ(requestsAfter, requestsBefore);
    ASSERT_EQ(payload.size, vector.expected.size());
    if (payload.size != 0)
      EXPECT_EQ(std::memcmp(payload.data, vector.expected.data(), payload.size),
                0);
  }
  EXPECT_FALSE(JS_HasException(context));
}

TEST_F(NominalPayloadTest,
       MaterializedVectorRetainsSoleOwnerWithoutPayloadCopy) {
  constexpr std::uint32_t payloadSize = 32 * 1024;
  std::vector<std::uint8_t> wire;
  wire.reserve(payloadSize + 5);
  wire.insert(wire.end(), {0x01, 0x13, 0xf1, 0x4f, 0x3f});
  for (std::uint32_t i = 0; i < payloadSize; ++i)
    wire.push_back(static_cast<std::uint8_t>(i));

  allocations.startRecording();
  qjs::OwnedValue root(context, types::makeCertifiedObjectCopy(
                                    context, wire.data(),
                                    static_cast<std::uint32_t>(wire.size())));
  allocations.stopRecording();
  ASSERT_FALSE(root.isException());
  ASSERT_GT(allocations.recordedRequests, 0u);
  EXPECT_EQ(allocations.maximumRecordedRequest, wire.size());
  EXPECT_LT(allocations.secondLargestRecordedRequest, payloadSize);

  allocations.startRecording();
  qjs::OwnedValue vector(context,
                         JS_GetPropertyStr(context, root.get(), "Indexes"));
  allocations.stopRecording();
  ASSERT_FALSE(vector.isException());
  ASSERT_GT(allocations.recordedRequests, 0u);
  EXPECT_LT(allocations.maximumRecordedRequest, payloadSize);

  std::uint8_t scratch[8]{};
  types::NominalPayloadView payload;
  ASSERT_TRUE(types::readNominalPayload(context, vector.get(),
                                        xdata::MaterializerKind::vector256,
                                        scratch, payload));
  ASSERT_EQ(payload.size, payloadSize);
  EXPECT_NE(payload.data, wire.data() + 5);
  EXPECT_EQ(std::memcmp(payload.data, wire.data() + 5, payload.size), 0);
  EXPECT_FALSE(JS_HasException(context));
}

TEST_F(NominalPayloadTest,
       MismatchInitializesOutputsPreservesExceptionsAndRunsNoJS) {
  auto bytes = pattern(16, 0x20);
  qjs::OwnedValue hash(context, types::makeHash128Bytes(
                                    context, bytes.data(),
                                    static_cast<std::uint32_t>(bytes.size())));
  ASSERT_FALSE(hash.isException());

  EXPECT_TRUE(JS_IsException(JS_ThrowTypeError(context, "sentinel")));
  std::uint8_t scratch[8];
  std::memset(scratch, 0xa5, sizeof(scratch));
  types::NominalPayloadView payload{scratch,
                                    std::numeric_limits<std::uint32_t>::max()};
  auto const pendingRequestsBefore = allocations.requests;
  EXPECT_FALSE(types::readNominalPayload(
      context, hash.get(), xdata::MaterializerKind::hash160, scratch, payload));
  auto const pendingRequestsAfter = allocations.requests;
  EXPECT_EQ(pendingRequestsAfter, pendingRequestsBefore);
  EXPECT_TRUE(JS_HasException(context));
  EXPECT_EQ(payload.data, nullptr);
  EXPECT_EQ(payload.size, 0u);
  EXPECT_EQ(std::count(std::begin(scratch), std::end(scratch), 0), 8);
  clearException();

  auto proxy = eval(R"JS(
        globalThis.nominalPayloadTrapCalls = 0;
        new Proxy({}, {
          get() { ++globalThis.nominalPayloadTrapCalls; },
          getPrototypeOf() { ++globalThis.nominalPayloadTrapCalls; },
        })
    )JS");
  ASSERT_FALSE(proxy.isException());
  constexpr xdata::MaterializerKind kinds[] = {
      xdata::MaterializerKind::uint8,
      xdata::MaterializerKind::hash128,
      xdata::MaterializerKind::hash256,
      xdata::MaterializerKind::blob,
      xdata::MaterializerKind::account_id,
      xdata::MaterializerKind::amount,
      xdata::MaterializerKind::currency,
      xdata::MaterializerKind::issue,
      xdata::MaterializerKind::path_set,
      xdata::MaterializerKind::vector256,
      xdata::MaterializerKind::xchain_bridge,
  };
  auto const proxyRequestsBefore = allocations.requests;
  for (auto kind : kinds) {
    std::memset(scratch, 0xa5, sizeof(scratch));
    payload = {scratch, std::numeric_limits<std::uint32_t>::max()};
    EXPECT_FALSE(types::readNominalPayload(context, proxy.get(), kind, scratch,
                                           payload));
    EXPECT_EQ(payload.data, nullptr);
    EXPECT_EQ(payload.size, 0u);
  }
  auto const proxyRequestsAfter = allocations.requests;
  EXPECT_EQ(proxyRequestsAfter, proxyRequestsBefore);
  auto trapCalls = eval("nominalPayloadTrapCalls");
  ASSERT_FALSE(trapCalls.isException());
  std::int32_t count = -1;
  ASSERT_EQ(JS_ToInt32(context, &count, trapCalls.get()), 0);
  EXPECT_EQ(count, 0);
  EXPECT_FALSE(JS_HasException(context));
}

} // namespace
