#include "js.hpp"
#include "object/canonical_json.hpp"
#include "object/field_js.hpp"
#include "object/object.hpp"
#include "quickjs.hpp"

#include "catl/xdata/static_protocol.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

extern "C" bool register_uint_types(JSContext *ctx);

namespace {

namespace qjs = jshookz::provider::qjs;
namespace types = jshookz::provider::types;

using Factory = JSValue (*)(JSContext *, std::uint8_t const *,
                            std::uint32_t) noexcept;

class LocalValue {
public:
  LocalValue(JSContext *context, JSValue value = JS_UNDEFINED) noexcept
      : context_(context), value_(value) {}

  ~LocalValue() { JS_FreeValue(context_, value_); }

  LocalValue(LocalValue const &) = delete;
  LocalValue &operator=(LocalValue const &) = delete;

  LocalValue(LocalValue &&other) noexcept
      : context_(other.context_), value_(other.value_) {
    other.value_ = JS_UNDEFINED;
  }

  LocalValue &operator=(LocalValue &&other) noexcept {
    if (this != &other) {
      JS_FreeValue(context_, value_);
      context_ = other.context_;
      value_ = other.value_;
      other.value_ = JS_UNDEFINED;
    }
    return *this;
  }

  [[nodiscard]] JSValueConst get() const noexcept { return value_; }

private:
  JSContext *context_;
  JSValue value_;
};

class CanonicalJSON : public ::testing::Test {
protected:
  JSRuntime *runtime = nullptr;
  JSContext *context = nullptr;

  void SetUp() override {
    runtime = JS_NewRuntime();
    ASSERT_NE(runtime, nullptr);
    context = JS_NewContext(runtime);
    ASSERT_NE(context, nullptr);
  }

  void TearDown() override {
    if (context != nullptr)
      JS_FreeContext(context);
    if (runtime != nullptr)
      JS_FreeRuntime(runtime);
  }
};

[[nodiscard]] std::uint8_t nibble(char value) noexcept {
  if (value >= '0' && value <= '9')
    return static_cast<std::uint8_t>(value - '0');
  if (value >= 'A' && value <= 'F')
    return static_cast<std::uint8_t>(value - 'A' + 10);
  if (value >= 'a' && value <= 'f')
    return static_cast<std::uint8_t>(value - 'a' + 10);
  return 0xFF;
}

[[nodiscard]] std::uint32_t decodeHex(char const *text, std::uint8_t *output,
                                      std::uint32_t capacity) noexcept {
  std::uint32_t digits = 0;
  while (text[digits] != '\0')
    ++digits;
  if ((digits & 1U) != 0 || digits / 2 > capacity)
    return std::numeric_limits<std::uint32_t>::max();
  for (std::uint32_t index = 0; index < digits / 2; ++index) {
    auto const high = nibble(text[index * 2]);
    auto const low = nibble(text[index * 2 + 1]);
    if (high == 0xFF || low == 0xFF)
      return std::numeric_limits<std::uint32_t>::max();
    output[index] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return digits / 2;
}

void expectJSON(JSContext *context, Factory factory, char const *hex,
                char const *expected) {
  SCOPED_TRACE(hex);
  std::uint8_t bytes[1024];
  auto const length = decodeHex(hex, bytes, sizeof(bytes));
  ASSERT_NE(length, std::numeric_limits<std::uint32_t>::max());
  LocalValue value(context, factory(context, bytes, length));
  ASSERT_FALSE(JS_IsException(value.get()));

  // Test observation only. Production construction never stringifies or
  // parses JSON; this comparison also pins insertion/key order.
  LocalValue json(context, JS_JSONStringify(context, value.get(), JS_UNDEFINED,
                                            JS_UNDEFINED));
  ASSERT_FALSE(JS_IsException(json.get()));
  char const *text = JS_ToCString(context, json.get());
  ASSERT_NE(text, nullptr);
  EXPECT_STREQ(text, expected);
  JS_FreeCString(context, text);
}

void expectInvalid(JSContext *context, Factory factory, char const *hex) {
  std::uint8_t bytes[1024];
  auto const length = decodeHex(hex, bytes, sizeof(bytes));
  ASSERT_NE(length, std::numeric_limits<std::uint32_t>::max());
  LocalValue value(context, factory(context, bytes, length));
  EXPECT_TRUE(JS_IsException(value.get()));
  EXPECT_TRUE(JS_HasException(context));
  LocalValue exception(context, JS_GetException(context));
  EXPECT_TRUE(JS_IsError(context, exception.get()));
  EXPECT_FALSE(JS_HasException(context));
}

TEST_F(CanonicalJSON, AccountCurrencyAndIssueMatchPinnedOracle) {
  expectJSON(context, types::makeAccountIDCanonicalJSON, "", "\"\"");
  expectJSON(context, types::makeAccountIDCanonicalJSON,
             "B5F762798A53D543A014CAF8B297CFF8F2F937E8",
             "\"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh\"");

  expectJSON(context, types::makeCurrencyCanonicalJSON,
             "0000000000000000000000000000000000000000", "\"XAH\"");
  expectJSON(context, types::makeCurrencyCanonicalJSON,
             "0000000000000000000000005553440000000000", "\"USD\"");
  expectJSON(context, types::makeCurrencyCanonicalJSON,
             "0000000000000000000000000000000000000001", "\"1\"");
  expectJSON(context, types::makeCurrencyCanonicalJSON,
             "0000000000000000000000005841480000000000",
             "\"0000000000000000000000005841480000000000\"");

  expectJSON(context, types::makeIssueCanonicalJSON,
             "0000000000000000000000000000000000000000",
             "{\"currency\":\"XAH\"}");
  expectJSON(context, types::makeIssueCanonicalJSON,
             "0000000000000000000000005553440000000000"
             "B5F762798A53D543A014CAF8B297CFF8F2F937E8",
             "{\"currency\":\"USD\","
             "\"issuer\":\"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh\"}");
  expectJSON(context, types::makeIssueCanonicalJSON,
             "000102030405060708090A0B0C0D0E0F10111213"
             "0000000000000000000000000000000000000001"
             "01020304",
             "{\"mpt_issuance_id\":"
             "\"04030201000102030405060708090A0B0C0D0E0F10111213\"}");
}

TEST_F(CanonicalJSON, AmountFormsAndMPTByteOrderMatchPinnedOracle) {
  expectJSON(context, types::makeAmountCanonicalJSON, "40000000000F4240",
             "\"1000000\"");
  expectJSON(context, types::makeAmountCanonicalJSON,
             "D4838D7EA4C68000"
             "0000000000000000000000005553440000000000"
             "B5F762798A53D543A014CAF8B297CFF8F2F937E8",
             "{\"currency\":\"USD\","
             "\"issuer\":\"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh\","
             "\"value\":\"1\"}");
  expectJSON(context, types::makeAmountCanonicalJSON,
             "C0438D7EA4C68000"
             "0000000000000000000000005553440000000000"
             "B5F762798A53D543A014CAF8B297CFF8F2F937E8",
             "{\"currency\":\"USD\","
             "\"issuer\":\"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh\","
             "\"value\":\"1000000000000000e-96\"}");
  expectJSON(context, types::makeAmountCanonicalJSON,
             "600000000000000001"
             "000102030405060708090A0B0C0D0E0F1011121314151617",
             "{\"mpt_issuance_id\":"
             "\"000102030405060708090A0B0C0D0E0F1011121314151617\","
             "\"value\":\"1\"}");
}

TEST_F(CanonicalJSON, PathSetPinsAllMasksSeparatorsAndTerminator) {
  struct Vector {
    char const *hex;
    char const *json;
  };
  constexpr Vector vectors[] = {
      {
          "01B5F762798A53D543A014CAF8B297CFF8F2F937E800",
          "[[{\"account\":\"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh\","
          "\"type\":1}]]",
      },
      {
          "10000000000000000000000000555344000000000000",
          "[[{\"currency\":\"USD\",\"type\":16}]]",
      },
      {
          "20B5F762798A53D543A014CAF8B297CFF8F2F937E800",
          "[[{\"issuer\":\"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh\","
          "\"type\":32}]]",
      },
      {
          "11B5F762798A53D543A014CAF8B297CFF8F2F937E8"
          "000000000000000000000000555344000000000000",
          "[[{\"account\":\"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh\","
          "\"currency\":\"USD\",\"type\":17}]]",
      },
      {
          "21B5F762798A53D543A014CAF8B297CFF8F2F937E8"
          "B5F762798A53D543A014CAF8B297CFF8F2F937E800",
          "[[{\"account\":\"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh\","
          "\"issuer\":\"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh\","
          "\"type\":33}]]",
      },
      {
          "300000000000000000000000005553440000000000"
          "B5F762798A53D543A014CAF8B297CFF8F2F937E800",
          "[[{\"currency\":\"USD\","
          "\"issuer\":\"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh\","
          "\"type\":48}]]",
      },
      {
          "31B5F762798A53D543A014CAF8B297CFF8F2F937E8"
          "0000000000000000000000005553440000000000"
          "B5F762798A53D543A014CAF8B297CFF8F2F937E800",
          "[[{\"account\":\"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh\","
          "\"currency\":\"USD\","
          "\"issuer\":\"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh\","
          "\"type\":49}]]",
      },
      {
          "01B5F762798A53D543A014CAF8B297CFF8F2F937E8FF"
          "300000000000000000000000005553440000000000"
          "B5F762798A53D543A014CAF8B297CFF8F2F937E800",
          "[[{\"account\":\"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh\","
          "\"type\":1}],"
          "[{\"currency\":\"USD\","
          "\"issuer\":\"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh\","
          "\"type\":48}]]",
      },
  };
  for (auto const &vector : vectors)
    expectJSON(context, types::makePathSetCanonicalJSON, vector.hex,
               vector.json);

  expectInvalid(context, types::makePathSetCanonicalJSON, "00");
  expectInvalid(context, types::makePathSetCanonicalJSON,
                "01B5F762798A53D543A014CAF8B297CFF8F2F937E8");
  expectInvalid(context, types::makePathSetCanonicalJSON,
                "FF01B5F762798A53D543A014CAF8B297CFF8F2F937E800");
  expectInvalid(context, types::makePathSetCanonicalJSON, "0200");
}

TEST_F(CanonicalJSON, IssueRejectsTruncatedMPTSentinelForm) {
  expectInvalid(context, types::makeIssueCanonicalJSON,
                "0000000000000000000000005553440000000000"
                "0000000000000000000000000000000000000001");
}

TEST_F(CanonicalJSON, VectorAndBridgePartsMatchPinnedOracle) {
  expectJSON(
      context, types::makeVector256CanonicalJSON,
      "000102030405060708090A0B0C0D0E0F"
      "101112131415161718191A1B1C1D1E1F"
      "202122232425262728292A2B2C2D2E2F"
      "303132333435363738393A3B3C3D3E3F",
      "[\"000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F\","
      "\"202122232425262728292A2B2C2D2E2F303132333435363738393A3B3C3D3E3F\"]");

  expectJSON(context, types::makeXChainBridgeCanonicalJSON,
             "14B5F762798A53D543A014CAF8B297CFF8F2F937E8"
             "0000000000000000000000000000000000000000"
             "14B5F762798A53D543A014CAF8B297CFF8F2F937E8"
             "0000000000000000000000000000000000000000",
             "{\"IssuingChainDoor\":\"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh\","
             "\"IssuingChainIssue\":{\"currency\":\"XAH\"},"
             "\"LockingChainDoor\":\"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh\","
             "\"LockingChainIssue\":{\"currency\":\"XAH\"}}");
  expectJSON(context, types::makeXChainBridgeCanonicalJSON,
             "00"
             "0000000000000000000000000000000000000000"
             "00"
             "0000000000000000000000000000000000000000",
             "{\"IssuingChainDoor\":\"\","
             "\"IssuingChainIssue\":{\"currency\":\"XAH\"},"
             "\"LockingChainDoor\":\"\","
             "\"LockingChainIssue\":{\"currency\":\"XAH\"}}");
}

TEST_F(CanonicalJSON, MiscallsReturnErrorsWithoutOutOfBoundsReads) {
  {
    LocalValue value(context,
                     types::makeAmountCanonicalJSON(context, nullptr, 0));
    EXPECT_TRUE(JS_IsException(value.get()));
    LocalValue exception(context, JS_GetException(context));
    EXPECT_TRUE(JS_IsError(context, exception.get()));
  }
  expectInvalid(context, types::makeIssueCanonicalJSON,
                "0000000000000000000000000000000000000000"
                "0000000000000000000000000000000000000000");
  expectInvalid(context, types::makeVector256CanonicalJSON,
                "000102030405060708090A0B0C0D0E0F");
  expectInvalid(context, types::makeXChainBridgeCanonicalJSON, "14");
}

struct alignas(std::max_align_t) AllocationHeader {
  std::size_t size;
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
  static constexpr std::size_t maxRecordedRequests = 1024;

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

[[nodiscard]] std::string
stringProperty(JSContext *context, JSValueConst object, char const *name) {
  LocalValue value(context, JS_GetPropertyStr(context, object, name));
  if (JS_IsException(value.get()))
    return {};
  char const *text = JS_ToCString(context, value.get());
  if (text == nullptr)
    return {};
  std::string result{text};
  JS_FreeCString(context, text);
  return result;
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

void expectJSONValue(JSContext *context, JSValueConst value,
                     char const *expected, std::size_t ordinal = 0) {
  SCOPED_TRACE(ordinal);
  LocalValue json(context,
                  JS_JSONStringify(context, value, JS_UNDEFINED, JS_UNDEFINED));
  ASSERT_FALSE(JS_IsException(json.get()));
  char const *text = JS_ToCString(context, json.get());
  ASSERT_NE(text, nullptr);
  EXPECT_STREQ(text, expected);
  JS_FreeCString(context, text);
}

TEST(FieldDescriptorOOM,
     FirstObservationIsAtomicIdentityCachedAndAllocationFreeAfterRetry) {
  AllocatorControl allocator;
  JSRuntime *runtime = JS_NewRuntime2(&testAllocator, &allocator);
  ASSERT_NE(runtime, nullptr);
  JSContext *context = JS_NewContext(runtime);
  ASSERT_NE(context, nullptr);
  ASSERT_TRUE(types::registerObjectTypes(context));

  {
    LocalValue global(context, JS_GetGlobalObject(context));
    ASSERT_FALSE(JS_IsException(global.get()));
    ASSERT_TRUE(types::registerFieldDescriptors(context, global.get()));
    LocalValue fields(context,
                      JS_GetPropertyStr(context, global.get(), "Field"));
    ASSERT_FALSE(JS_IsException(fields.get()));

    LocalValue warm(context,
                    JS_GetPropertyStr(context, fields.get(), "Flags"));
    ASSERT_FALSE(JS_IsException(warm.get()));
    std::size_t const measuredBefore = allocator.requests;
    LocalValue measured(context,
                        JS_GetPropertyStr(context, fields.get(), "Sequence"));
    ASSERT_FALSE(JS_IsException(measured.get()));
    std::size_t const requestCount = allocator.requests - measuredBefore;
    ASSERT_GT(requestCount, 0u);

    static constexpr std::array<char const *, 8> unwarmedNames = {
        "SourceTag", "Account", "Amount", "Fee",
        "SigningPubKey", "TxnSignature", "Memos", "EmitDetails",
    };
    ASSERT_LE(requestCount, unwarmedNames.size());
    for (std::size_t ordinal = 1; ordinal <= requestCount; ++ordinal) {
      SCOPED_TRACE(ordinal);
      char const *name = unwarmedNames[ordinal - 1];
      std::size_t const liveBefore = allocator.liveBlocks;
      std::size_t const rejectionsBefore = allocator.rejections;
      allocator.rejectAt = allocator.requests + ordinal;
      JSValue failed = JS_GetPropertyStr(context, fields.get(), name);
      allocator.rejectAt = 0;

      EXPECT_TRUE(JS_IsException(failed));
      ASSERT_EQ(allocator.rejections, rejectionsBefore + 1);
      expectOnePendingOOM(context, ordinal);
      EXPECT_EQ(allocator.liveBlocks, liveBefore);

      LocalValue retry(context,
                       JS_GetPropertyStr(context, fields.get(), name));
      ASSERT_FALSE(JS_IsException(retry.get()));
      ASSERT_TRUE(JS_IsObject(retry.get()));
      void const *identity = JS_VALUE_GET_PTR(retry.get());
      std::size_t const cachedBefore = allocator.requests;
      LocalValue cached(context,
                        JS_GetPropertyStr(context, fields.get(), name));
      ASSERT_FALSE(JS_IsException(cached.get()));
      EXPECT_EQ(JS_VALUE_GET_PTR(cached.get()), identity);
      EXPECT_EQ(allocator.requests, cachedBefore);
      EXPECT_FALSE(JS_HasException(context));
    }
  }

  JS_FreeContext(context);
  types::unregisterObjectTypes(runtime);
  JS_FreeRuntime(runtime);
  EXPECT_EQ(allocator.liveBlocks, 0u);
}

TEST(FieldDescriptorOOM,
     CallbackFailureLeavesEveryOutputDescriptorMemberUnpublished) {
  AllocatorControl allocator;
  JSRuntime *runtime = JS_NewRuntime2(&testAllocator, &allocator);
  ASSERT_NE(runtime, nullptr);
  JSContext *context = JS_NewContext(runtime);
  ASSERT_NE(context, nullptr);
  ASSERT_TRUE(types::registerObjectTypes(context));

  {
    LocalValue global(context, JS_GetGlobalObject(context));
    ASSERT_FALSE(JS_IsException(global.get()));
    ASSERT_TRUE(types::registerFieldDescriptors(context, global.get()));
    LocalValue fields(context,
                      JS_GetPropertyStr(context, global.get(), "Field"));
    ASSERT_FALSE(JS_IsException(fields.get()));

    JSAtom measuredAtom = JS_NewAtom(context, "Sequence");
    ASSERT_NE(measuredAtom, JS_ATOM_NULL);
    JSPropertyDescriptor measured{
        .flags = 0,
        .value = JS_UNDEFINED,
        .getter = JS_UNDEFINED,
        .setter = JS_UNDEFINED,
    };
    std::size_t const before = allocator.requests;
    ASSERT_EQ(JS_GetOwnProperty(
                  context, &measured, fields.get(), measuredAtom),
              1);
    std::size_t const requestCount = allocator.requests - before;
    JS_FreeValue(context, measured.value);
    JS_FreeValue(context, measured.getter);
    JS_FreeValue(context, measured.setter);
    JS_FreeAtom(context, measuredAtom);
    ASSERT_GT(requestCount, 0u);

    static constexpr std::array<char const *, 8> names = {
        "SourceTag", "Account", "Amount", "Fee",
        "SigningPubKey", "TxnSignature", "Memos", "EmitDetails",
    };
    ASSERT_LE(requestCount, names.size());
    for (std::size_t ordinal = 1; ordinal <= requestCount; ++ordinal) {
      SCOPED_TRACE(ordinal);
      JSAtom atom = JS_NewAtom(context, names[ordinal - 1]);
      ASSERT_NE(atom, JS_ATOM_NULL);
      JSPropertyDescriptor failed{
          .flags = 0,
          .value = JS_UNDEFINED,
          .getter = JS_UNDEFINED,
          .setter = JS_UNDEFINED,
      };
      allocator.rejectAt = allocator.requests + ordinal;
      int const result =
          JS_GetOwnProperty(context, &failed, fields.get(), atom);
      allocator.rejectAt = 0;
      if (result == 1) {
        EXPECT_EQ(failed.flags, JS_PROP_ENUMERABLE);
        EXPECT_TRUE(JS_IsObject(failed.value));
        EXPECT_FALSE(JS_HasException(context));
        JS_FreeValue(context, failed.value);
        JS_FreeValue(context, failed.getter);
        JS_FreeValue(context, failed.setter);
        JS_FreeAtom(context, atom);
        continue;
      }
      EXPECT_EQ(result, -1);
      EXPECT_EQ(failed.flags, 0);
      EXPECT_TRUE(JS_IsUndefined(failed.value));
      EXPECT_TRUE(JS_IsUndefined(failed.getter));
      EXPECT_TRUE(JS_IsUndefined(failed.setter));
      expectOnePendingOOM(context, ordinal);

      JSPropertyDescriptor retry{
          .flags = 0,
          .value = JS_UNDEFINED,
          .getter = JS_UNDEFINED,
          .setter = JS_UNDEFINED,
      };
      ASSERT_EQ(JS_GetOwnProperty(context, &retry, fields.get(), atom), 1);
      EXPECT_EQ(retry.flags, JS_PROP_ENUMERABLE);
      EXPECT_TRUE(JS_IsObject(retry.value));
      JS_FreeValue(context, retry.value);
      JS_FreeValue(context, retry.getter);
      JS_FreeValue(context, retry.setter);
      JS_FreeAtom(context, atom);
    }
  }

  JS_FreeContext(context);
  types::unregisterObjectTypes(runtime);
  JS_FreeRuntime(runtime);
  EXPECT_EQ(allocator.liveBlocks, 0u);
}

TEST(ObjectRegistrarOOM,
     EveryFieldTableFieldAtomAndDiagnosticAtomAllocationRetries) {
  AllocatorControl allocator;
  JSRuntime *runtime = JS_NewRuntime2(&testAllocator, &allocator);
  ASSERT_NE(runtime, nullptr);
  JSContext *context = JS_NewContext(runtime);
  ASSERT_NE(context, nullptr);

  // Class/prototype installation is one staged transaction. Warm that once;
  // the exact registration law below is the generated atom table plus all
  // field and diagnostic atom acquisitions.
  ASSERT_TRUE(types::registerObjectTypes(context));
  types::unregisterObjectTypes(runtime);
  allocator.startRecording();
  std::size_t const before = allocator.requests;
  ASSERT_TRUE(types::registerObjectTypes(context));
  std::size_t const requestCount = allocator.requests - before;
  allocator.stopRecording();
  ASSERT_EQ(allocator.recordedCount, requestCount);
  ASSERT_GT(allocator.recordedCount, 0u);
  EXPECT_EQ(allocator.recorded[0],
            (AllocationRequest{337u * 8u, RequestKind::allocate}));
  types::unregisterObjectTypes(runtime);
  ASSERT_GT(requestCount, 337u);

  for (std::size_t ordinal = 1; ordinal <= requestCount; ++ordinal) {
    SCOPED_TRACE(ordinal);
    std::size_t const rejectionsBefore = allocator.rejections;
    allocator.rejectAt = allocator.requests + ordinal;
    bool const registered = types::registerObjectTypes(context);
    allocator.rejectAt = 0;
    ASSERT_EQ(allocator.rejections, rejectionsBefore + 1);
    if (!registered) {
      expectOnePendingOOM(context, ordinal);
      ASSERT_TRUE(types::registerObjectTypes(context))
          << "same-runtime atom registration retry failed";
    }
    EXPECT_FALSE(JS_HasException(context));
    types::unregisterObjectTypes(runtime);
  }

  JS_FreeContext(context);
  types::unregisterObjectTypes(runtime);
  JS_FreeRuntime(runtime);
  EXPECT_EQ(allocator.liveBlocks, 0u);
}

TEST(FieldDescriptorOOM,
     EveryRegistrationAllocationHasNoGlobalPublicationAndRetries) {
  std::size_t requestCount = 0;
  {
    AllocatorControl measured;
    JSRuntime *runtime = JS_NewRuntime2(&testAllocator, &measured);
    ASSERT_NE(runtime, nullptr);
    JSContext *context = JS_NewContext(runtime);
    ASSERT_NE(context, nullptr);
    ASSERT_TRUE(types::registerObjectTypes(context));
    {
      LocalValue global(context, JS_GetGlobalObject(context));
      ASSERT_FALSE(JS_IsException(global.get()));
      measured.startRecording();
      std::size_t const before = measured.requests;
      ASSERT_TRUE(types::registerFieldDescriptors(context, global.get()));
      requestCount = measured.requests - before;
      measured.stopRecording();
      ASSERT_EQ(measured.recordedCount, requestCount);
      EXPECT_NE(
          std::find(measured.recorded.begin(),
                    measured.recorded.begin() + measured.recordedCount,
                    AllocationRequest{325u * sizeof(void *),
                                      RequestKind::allocate}),
          measured.recorded.begin() + measured.recordedCount);
      EXPECT_FALSE(JS_HasException(context));
    }
    JS_FreeContext(context);
    types::unregisterObjectTypes(runtime);
    JS_FreeRuntime(runtime);
    EXPECT_EQ(measured.liveBlocks, 0u);
  }
  ASSERT_GT(requestCount, 0u);

  for (std::size_t ordinal = 1; ordinal <= requestCount; ++ordinal) {
    SCOPED_TRACE(ordinal);
    AllocatorControl allocator;
    JSRuntime *runtime = JS_NewRuntime2(&testAllocator, &allocator);
    ASSERT_NE(runtime, nullptr);
    JSContext *context = JS_NewContext(runtime);
    ASSERT_NE(context, nullptr);
    ASSERT_TRUE(types::registerObjectTypes(context));
    {
      LocalValue global(context, JS_GetGlobalObject(context));
      ASSERT_FALSE(JS_IsException(global.get()));
      std::size_t const rejectionsBefore = allocator.rejections;
      allocator.rejectAt = allocator.requests + ordinal;
      bool const registered =
          types::registerFieldDescriptors(context, global.get());
      allocator.rejectAt = 0;
      ASSERT_EQ(allocator.rejections, rejectionsBefore + 1);
      if (!registered) {
        expectOnePendingOOM(context, ordinal);
        LocalValue unpublished(
            context, JS_GetPropertyStr(context, global.get(), "Field"));
        ASSERT_FALSE(JS_IsException(unpublished.get()));
        EXPECT_TRUE(JS_IsUndefined(unpublished.get()));
        ASSERT_TRUE(types::registerFieldDescriptors(context, global.get()))
            << "same-runtime Field registration retry failed";
      }
      LocalValue published(context,
                           JS_GetPropertyStr(context, global.get(), "Field"));
      ASSERT_FALSE(JS_IsException(published.get()));
      EXPECT_TRUE(JS_IsObject(published.get()));
      EXPECT_FALSE(JS_HasException(context));
    }
    JS_FreeContext(context);
    types::unregisterObjectTypes(runtime);
    JS_FreeRuntime(runtime);
    EXPECT_EQ(allocator.liveBlocks, 0u);
  }
}

enum class FieldReflectionResult {
  names,
  values,
  entries,
  descriptors,
  copiedObject,
};

struct FieldReflectionRoute {
  char const *name;
  FieldReflectionResult result;
};

[[nodiscard]] std::string materialFieldName(std::uint32_t ordinal) {
  auto const &protocol = catl::xdata::xahau_static_protocol();
  auto const *material = protocol.material_field(ordinal);
  auto const *descriptor = material == nullptr
      ? nullptr : protocol.field_by_code(material->field_code);
  if (descriptor == nullptr)
    return {};
  auto const name = protocol.field_name(descriptor->name_ordinal);
  return {name.data, name.size};
}

void expectFieldReflectionResult(JSContext *context, JSValueConst fields,
                                 JSValueConst result,
                                 FieldReflectionRoute const &route) {
  constexpr std::uint32_t count = 325;
  static constexpr std::array<std::uint32_t, 3> sentinels = {
      0, count / 2, count - 1};
  if (route.result == FieldReflectionResult::names ||
      route.result == FieldReflectionResult::values ||
      route.result == FieldReflectionResult::entries) {
    LocalValue length(context, JS_GetPropertyStr(context, result, "length"));
    ASSERT_FALSE(JS_IsException(length.get()));
    std::uint32_t actualLength = 0;
    ASSERT_EQ(JS_ToUint32(context, &actualLength, length.get()), 0);
    ASSERT_EQ(actualLength, count);
  } else {
    JSPropertyEnum *names = nullptr;
    std::uint32_t actualLength = 0;
    ASSERT_EQ(JS_GetOwnPropertyNames(
                  context, &names, &actualLength, result,
                  JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK),
              0);
    JS_FreePropertyEnum(context, names, actualLength);
    ASSERT_EQ(actualLength, count);
  }

  for (std::uint32_t ordinal : sentinels) {
    SCOPED_TRACE(ordinal);
    std::string const name = materialFieldName(ordinal);
    ASSERT_FALSE(name.empty());
    LocalValue expected(
        context, JS_GetPropertyStr(context, fields, name.c_str()));
    ASSERT_FALSE(JS_IsException(expected.get()));
    ASSERT_TRUE(JS_IsObject(expected.get()));

    LocalValue observed(context);
    if (route.result == FieldReflectionResult::names) {
      LocalValue key(context, JS_GetPropertyUint32(context, result, ordinal));
      ASSERT_FALSE(JS_IsException(key.get()));
      char const *text = JS_ToCString(context, key.get());
      ASSERT_NE(text, nullptr);
      EXPECT_EQ(std::string_view(text), name);
      JS_FreeCString(context, text);
      continue;
    }
    if (route.result == FieldReflectionResult::values) {
      observed = LocalValue(
          context, JS_GetPropertyUint32(context, result, ordinal));
    } else if (route.result == FieldReflectionResult::entries) {
      LocalValue pair(
          context, JS_GetPropertyUint32(context, result, ordinal));
      ASSERT_FALSE(JS_IsException(pair.get()));
      LocalValue key(context, JS_GetPropertyUint32(context, pair.get(), 0));
      ASSERT_FALSE(JS_IsException(key.get()));
      char const *text = JS_ToCString(context, key.get());
      ASSERT_NE(text, nullptr);
      EXPECT_EQ(std::string_view(text), name);
      JS_FreeCString(context, text);
      observed = LocalValue(
          context, JS_GetPropertyUint32(context, pair.get(), 1));
    } else if (route.result == FieldReflectionResult::descriptors) {
      LocalValue descriptor(
          context, JS_GetPropertyStr(context, result, name.c_str()));
      ASSERT_FALSE(JS_IsException(descriptor.get()));
      observed = LocalValue(
          context, JS_GetPropertyStr(context, descriptor.get(), "value"));
    } else {
      observed = LocalValue(
          context, JS_GetPropertyStr(context, result, name.c_str()));
    }
    ASSERT_FALSE(JS_IsException(observed.get()));
    EXPECT_EQ(JS_StrictEq(context, observed.get(), expected.get()), 1);
  }
}

TEST(FieldDescriptorOOM,
     ReflectionAndCopyEnvelopesFailAtomicallyAndRetryWithStableIdentity) {
  static constexpr FieldReflectionRoute routes[] = {
      {"ownKeys", FieldReflectionResult::names},
      {"ownNames", FieldReflectionResult::names},
      {"keys", FieldReflectionResult::names},
      {"values", FieldReflectionResult::values},
      {"entries", FieldReflectionResult::entries},
      {"descriptors", FieldReflectionResult::descriptors},
      {"spread", FieldReflectionResult::copiedObject},
      {"assign", FieldReflectionResult::copiedObject},
  };
  static constexpr char routeSource[] = R"JS(
    globalThis.__fieldReflectionRoutes = Object.freeze({
      ownKeys: () => Reflect.ownKeys(Field),
      ownNames: () => Object.getOwnPropertyNames(Field),
      keys: () => Object.keys(Field),
      values: () => Object.values(Field),
      entries: () => Object.entries(Field),
      descriptors: () => Object.getOwnPropertyDescriptors(Field),
      spread: () => ({...Field}),
      assign: () => Object.assign({}, Field),
    });
  )JS";

  AllocatorControl allocator;
  JSRuntime *runtime = JS_NewRuntime2(&testAllocator, &allocator);
  ASSERT_NE(runtime, nullptr);
  JSContext *context = JS_NewContext(runtime);
  ASSERT_NE(context, nullptr);
  ASSERT_TRUE(types::registerObjectTypes(context));
  {
    LocalValue installed(
        context, JS_Eval(context, routeSource, sizeof(routeSource) - 1,
                         "<field-reflection-routes>", JS_EVAL_TYPE_GLOBAL));
    ASSERT_FALSE(JS_IsException(installed.get()));
  }

  for (auto const &route : routes) {
    SCOPED_TRACE(route.name);
    LocalValue global(context, JS_GetGlobalObject(context));
    ASSERT_FALSE(JS_IsException(global.get()));
    LocalValue routeTable(
        context,
        JS_GetPropertyStr(context, global.get(), "__fieldReflectionRoutes"));
    ASSERT_FALSE(JS_IsException(routeTable.get()));
    LocalValue function(
        context, JS_GetPropertyStr(context, routeTable.get(), route.name));
    ASSERT_TRUE(JS_IsFunction(context, function.get()));

    // Stabilize route-local atoms, shapes, and bytecode before measurement.
    ASSERT_TRUE(types::registerFieldDescriptors(context, global.get()));
    {
      LocalValue fields(
          context, JS_GetPropertyStr(context, global.get(), "Field"));
      LocalValue warm(
          context, JS_Call(context, function.get(), JS_UNDEFINED, 0, nullptr));
      ASSERT_FALSE(JS_IsException(warm.get()));
      expectFieldReflectionResult(
          context, fields.get(), warm.get(), route);
    }

    ASSERT_TRUE(types::registerFieldDescriptors(context, global.get()));
    std::size_t requestCount = 0;
    {
      LocalValue fields(
          context, JS_GetPropertyStr(context, global.get(), "Field"));
      std::size_t const before = allocator.requests;
      LocalValue measured(
          context, JS_Call(context, function.get(), JS_UNDEFINED, 0, nullptr));
      requestCount = allocator.requests - before;
      ASSERT_FALSE(JS_IsException(measured.get()));
      expectFieldReflectionResult(
          context, fields.get(), measured.get(), route);
    }
    ASSERT_GT(requestCount, 0u);

    std::array<std::size_t, 3> const selected = {
        1, (requestCount + 1) / 2, requestCount};
    std::size_t failedSelections = 0;
    for (std::size_t selection = 0; selection < selected.size(); ++selection) {
      std::size_t const ordinal = selected[selection];
      if (selection != 0 && ordinal == selected[selection - 1])
        continue;
      SCOPED_TRACE(ordinal);
      ASSERT_TRUE(types::registerFieldDescriptors(context, global.get()));
      LocalValue fields(
          context, JS_GetPropertyStr(context, global.get(), "Field"));
      void const *fieldTableIdentity = JS_VALUE_GET_PTR(fields.get());

      std::size_t const rejectionsBefore = allocator.rejections;
      allocator.rejectAt = allocator.requests + ordinal;
      LocalValue failed(
          context, JS_Call(context, function.get(), JS_UNDEFINED, 0, nullptr));
      allocator.rejectAt = 0;
      ASSERT_EQ(allocator.rejections, rejectionsBefore + 1);
      if (!JS_IsException(failed.get())) {
        EXPECT_FALSE(JS_HasException(context));
        expectFieldReflectionResult(
            context, fields.get(), failed.get(), route);
        continue;
      }
      ++failedSelections;
      expectOnePendingOOM(context, ordinal);

      LocalValue stillPublished(
          context, JS_GetPropertyStr(context, global.get(), "Field"));
      ASSERT_FALSE(JS_IsException(stillPublished.get()));
      EXPECT_EQ(JS_VALUE_GET_PTR(stillPublished.get()), fieldTableIdentity);

      std::string const firstName = materialFieldName(0);
      std::string const middleName = materialFieldName(325 / 2);
      std::string const lastName = materialFieldName(324);
      LocalValue first(
          context, JS_GetPropertyStr(context, fields.get(), firstName.c_str()));
      LocalValue middle(
          context, JS_GetPropertyStr(context, fields.get(), middleName.c_str()));
      LocalValue last(
          context, JS_GetPropertyStr(context, fields.get(), lastName.c_str()));
      ASSERT_FALSE(JS_IsException(first.get()));
      ASSERT_FALSE(JS_IsException(middle.get()));
      ASSERT_FALSE(JS_IsException(last.get()));

      LocalValue retry(
          context, JS_Call(context, function.get(), JS_UNDEFINED, 0, nullptr));
      ASSERT_FALSE(JS_IsException(retry.get()))
          << "same-runtime reflection retry failed";
      expectFieldReflectionResult(
          context, fields.get(), retry.get(), route);
      LocalValue firstAfter(
          context, JS_GetPropertyStr(context, fields.get(), firstName.c_str()));
      LocalValue middleAfter(
          context, JS_GetPropertyStr(context, fields.get(), middleName.c_str()));
      LocalValue lastAfter(
          context, JS_GetPropertyStr(context, fields.get(), lastName.c_str()));
      EXPECT_EQ(JS_StrictEq(context, first.get(), firstAfter.get()), 1);
      EXPECT_EQ(JS_StrictEq(context, middle.get(), middleAfter.get()), 1);
      EXPECT_EQ(JS_StrictEq(context, last.get(), lastAfter.get()), 1);
      EXPECT_FALSE(JS_HasException(context));
    }
    EXPECT_GT(failedSelections, 0u);
  }

  JS_FreeContext(context);
  types::unregisterObjectTypes(runtime);
  JS_FreeRuntime(runtime);
  EXPECT_EQ(allocator.liveBlocks, 0u);
}

void exerciseSameRuntimeFactoryOOM(JSContext *context,
                                   AllocatorControl &allocator, Factory factory,
                                   char const *hex, char const *expected,
                                   bool allocationFree) {
  std::uint8_t bytes[1024];
  auto const length = decodeHex(hex, bytes, sizeof(bytes));
  ASSERT_NE(length, std::numeric_limits<std::uint32_t>::max());
  std::uint8_t original[sizeof(bytes)];
  std::memcpy(original, bytes, length);

  // Stabilize all route-local atoms and shapes before using retained-block
  // equality as the no-partial-publication oracle.
  for (int pass = 0; pass < 2; ++pass) {
    LocalValue warm(context, factory(context, bytes, length));
    ASSERT_FALSE(JS_IsException(warm.get()));
    expectJSONValue(context, warm.get(), expected);
    ASSERT_EQ(std::memcmp(bytes, original, length), 0);
  }
  std::size_t const liveBefore = allocator.liveBlocks;

  std::size_t const requestBefore = allocator.requests;
  JSValue measuredValue = factory(context, bytes, length);
  std::size_t const requestCount = allocator.requests - requestBefore;
  {
    LocalValue measured(context, measuredValue);
    ASSERT_FALSE(JS_IsException(measured.get()));
    expectJSONValue(context, measured.get(), expected);
  }
  if (allocationFree)
    ASSERT_EQ(requestCount, 0u);
  else
    ASSERT_GT(requestCount, 0u);
  ASSERT_EQ(allocator.liveBlocks, liveBefore);

  for (std::size_t ordinal = 1; ordinal <= requestCount; ++ordinal) {
    SCOPED_TRACE(ordinal);
    std::size_t const rejectionsBefore = allocator.rejections;
    allocator.rejectAt = allocator.requests + ordinal;
    JSValue failed = factory(context, bytes, length);
    allocator.rejectAt = 0;

    EXPECT_TRUE(JS_IsException(failed));
    ASSERT_EQ(allocator.rejections, rejectionsBefore + 1);
    expectOnePendingOOM(context, ordinal);
    EXPECT_EQ(std::memcmp(bytes, original, length), 0);
    EXPECT_EQ(allocator.liveBlocks, liveBefore);

    {
      LocalValue retry(context, factory(context, bytes, length));
      ASSERT_FALSE(JS_IsException(retry.get()));
      expectJSONValue(context, retry.get(), expected, ordinal);
    }
    EXPECT_FALSE(JS_HasException(context));
    EXPECT_EQ(std::memcmp(bytes, original, length), 0);
    EXPECT_EQ(allocator.liveBlocks, liveBefore);
  }
}

struct FactoryOOMCase {
  char const *name;
  Factory factory;
  char const *hex;
  char const *json;
  bool allocationFree = false;
};

TEST(CanonicalJSONOOM, EveryFactoryRouteRetriesInOneRuntime) {
  static constexpr char genesis[] = "B5F762798A53D543A014CAF8B297CFF8F2F937E8";
  static constexpr char nativeIssue[] =
      "0000000000000000000000000000000000000000";
  static constexpr char iouIssue[] = "0000000000000000000000005553440000000000"
                                     "B5F762798A53D543A014CAF8B297CFF8F2F937E8";
  static constexpr char mptIssue[] = "000102030405060708090A0B0C0D0E0F10111213"
                                     "0000000000000000000000000000000000000001"
                                     "01020304";
  static constexpr FactoryOOMCase cases[] = {
      {
          "AccountIDEmpty",
          types::makeAccountIDCanonicalJSON,
          "",
          "\"\"",
          true,
      },
      {
          "AccountID",
          types::makeAccountIDCanonicalJSON,
          genesis,
          "\"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh\"",
      },
      {
          "CurrencyNative",
          types::makeCurrencyCanonicalJSON,
          "0000000000000000000000000000000000000000",
          "\"XAH\"",
      },
      {
          "CurrencyCodeOne",
          types::makeCurrencyCanonicalJSON,
          "0000000000000000000000000000000000000001",
          "\"1\"",
      },
      {
          "CurrencyStandard",
          types::makeCurrencyCanonicalJSON,
          "0000000000000000000000005553440000000000",
          "\"USD\"",
      },
      {
          "CurrencyHex",
          types::makeCurrencyCanonicalJSON,
          "0000000000000000000000005841480000000000",
          "\"0000000000000000000000005841480000000000\"",
      },
      {
          "AmountNative",
          types::makeAmountCanonicalJSON,
          "40000000000F4240",
          "\"1000000\"",
      },
      {
          "AmountIOU",
          types::makeAmountCanonicalJSON,
          "D4838D7EA4C68000"
          "0000000000000000000000005553440000000000"
          "B5F762798A53D543A014CAF8B297CFF8F2F937E8",
          "{\"currency\":\"USD\","
          "\"issuer\":\"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh\","
          "\"value\":\"1\"}",
      },
      {
          "AmountMPT",
          types::makeAmountCanonicalJSON,
          "600000000000000001"
          "000102030405060708090A0B0C0D0E0F1011121314151617",
          "{\"mpt_issuance_id\":"
          "\"000102030405060708090A0B0C0D0E0F1011121314151617\","
          "\"value\":\"1\"}",
      },
      {
          "IssueNative",
          types::makeIssueCanonicalJSON,
          nativeIssue,
          "{\"currency\":\"XAH\"}",
      },
      {
          "IssueIOU",
          types::makeIssueCanonicalJSON,
          iouIssue,
          "{\"currency\":\"USD\","
          "\"issuer\":\"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh\"}",
      },
      {
          "IssueMPT",
          types::makeIssueCanonicalJSON,
          mptIssue,
          "{\"mpt_issuance_id\":"
          "\"04030201000102030405060708090A0B0C0D0E0F10111213\"}",
      },
      {
          "Vector256",
          types::makeVector256CanonicalJSON,
          "000102030405060708090A0B0C0D0E0F"
          "101112131415161718191A1B1C1D1E1F"
          "202122232425262728292A2B2C2D2E2F"
          "303132333435363738393A3B3C3D3E3F",
          "[\"000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F"
          "\","
          "\"202122232425262728292A2B2C2D2E2F303132333435363738393A3B3C3D3E3F\""
          "]",
      },
      {
          "PathSet",
          types::makePathSetCanonicalJSON,
          "31B5F762798A53D543A014CAF8B297CFF8F2F937E8"
          "0000000000000000000000005553440000000000"
          "B5F762798A53D543A014CAF8B297CFF8F2F937E8FF"
          "10"
          "000000000000000000000000555344000000000000",
          "[[{\"account\":\"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh\","
          "\"currency\":\"USD\","
          "\"issuer\":\"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh\","
          "\"type\":49}],[{\"currency\":\"USD\",\"type\":16}]]",
      },
      {
          "XChainBridgeNative",
          types::makeXChainBridgeCanonicalJSON,
          "14B5F762798A53D543A014CAF8B297CFF8F2F937E8"
          "0000000000000000000000000000000000000000"
          "14B5F762798A53D543A014CAF8B297CFF8F2F937E8"
          "0000000000000000000000000000000000000000",
          "{\"IssuingChainDoor\":\"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh\","
          "\"IssuingChainIssue\":{\"currency\":\"XAH\"},"
          "\"LockingChainDoor\":\"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh\","
          "\"LockingChainIssue\":{\"currency\":\"XAH\"}}",
      },
      {
          "XChainBridgeMPT",
          types::makeXChainBridgeCanonicalJSON,
          "00"
          "000102030405060708090A0B0C0D0E0F10111213"
          "0000000000000000000000000000000000000001"
          "01020304"
          "00"
          "000102030405060708090A0B0C0D0E0F10111213"
          "0000000000000000000000000000000000000001"
          "01020304",
          "{\"IssuingChainDoor\":\"\","
          "\"IssuingChainIssue\":{\"mpt_issuance_id\":"
          "\"04030201000102030405060708090A0B0C0D0E0F10111213\"},"
          "\"LockingChainDoor\":\"\","
          "\"LockingChainIssue\":{\"mpt_issuance_id\":"
          "\"04030201000102030405060708090A0B0C0D0E0F10111213\"}}",
      },
  };

  AllocatorControl allocator;
  JSRuntime *runtime = JS_NewRuntime2(&testAllocator, &allocator);
  ASSERT_NE(runtime, nullptr);
  JSContext *context = JS_NewContext(runtime);
  ASSERT_NE(context, nullptr);

  EXPECT_TRUE(JS_IsException(JS_ThrowOutOfMemory(context)));
  expectOnePendingOOM(context, 0);
  for (auto const &testCase : cases) {
    SCOPED_TRACE(testCase.name);
    exerciseSameRuntimeFactoryOOM(context, allocator, testCase.factory,
                                  testCase.hex, testCase.json,
                                  testCase.allocationFree);
  }

  JS_FreeContext(context);
  JS_FreeRuntime(runtime);
  EXPECT_EQ(allocator.liveBlocks, 0u);
}

void expectCertifiedSourceUnchanged(
    JSContext *context, JSValueConst root, void const *rootIdentity,
    JSValueConst toBytes, JSValueConst expectedMemos,
    JSValueConst expectedElement, JSValueConst expectedMemo,
    std::uint8_t const *expectedBytes, std::uint32_t expectedSize,
    std::size_t ordinal) {
  SCOPED_TRACE(ordinal);
  ASSERT_TRUE(types::isSTObject(root));
  EXPECT_EQ(JS_VALUE_GET_PTR(root), rootIdentity);

  LocalValue memos(context, JS_GetPropertyStr(context, root, "Memos"));
  ASSERT_FALSE(JS_IsException(memos.get()));
  EXPECT_EQ(JS_StrictEq(context, memos.get(), expectedMemos), 1);
  LocalValue element(context, JS_GetPropertyUint32(context, memos.get(), 0));
  ASSERT_FALSE(JS_IsException(element.get()));
  EXPECT_EQ(JS_StrictEq(context, element.get(), expectedElement), 1);
  LocalValue memo(context, JS_GetPropertyStr(context, element.get(), "Memo"));
  ASSERT_FALSE(JS_IsException(memo.get()));
  EXPECT_EQ(JS_StrictEq(context, memo.get(), expectedMemo), 1);

  LocalValue bytes(context, JS_Call(context, toBytes, root, 0, nullptr));
  ASSERT_FALSE(JS_IsException(bytes.get()));
  JSValue backing = JS_UNDEFINED;
  std::uint8_t const *data = nullptr;
  std::size_t size = 0;
  ASSERT_EQ(
      JS_GetObjectByteSpanNoThrow(context, bytes.get(), &backing, &data, &size),
      JS_OBJECT_BYTES_OK);
  LocalValue ownedBacking(context, backing);
  ASSERT_EQ(size, expectedSize);
  EXPECT_EQ(std::memcmp(data, expectedBytes, expectedSize), 0);
  EXPECT_FALSE(JS_HasException(context));
}

struct MaterializerOOMCase {
  char const *name;
  char const *fieldName;
  char const *wireHex;
  char const *expectedJSON;
  char const *renderedString;
  std::size_t temporaryProviderBytes = 0;
};

void expectMaterializerSourceUnchanged(
    JSContext *context, JSValueConst root, void const *rootIdentity,
    JSValueConst toBytes, char const *fieldName, JSValueConst expectedField,
    void const *fieldIdentity, std::uint8_t const *expectedBytes,
    std::uint32_t expectedSize, std::size_t ordinal) {
  SCOPED_TRACE(ordinal);
  ASSERT_TRUE(types::isSTObject(root));
  EXPECT_EQ(JS_VALUE_GET_PTR(root), rootIdentity);

  LocalValue field(context, JS_GetPropertyStr(context, root, fieldName));
  ASSERT_FALSE(JS_IsException(field.get()));
  EXPECT_EQ(JS_StrictEq(context, field.get(), expectedField), 1);
  ASSERT_TRUE(JS_IsObject(field.get()) || JS_IsString(field.get()));
  EXPECT_EQ(JS_VALUE_GET_PTR(field.get()), fieldIdentity);

  LocalValue bytes(context, JS_Call(context, toBytes, root, 0, nullptr));
  ASSERT_FALSE(JS_IsException(bytes.get()));
  JSValue backing = JS_UNDEFINED;
  std::uint8_t const *data = nullptr;
  std::size_t size = 0;
  ASSERT_EQ(
      JS_GetObjectByteSpanNoThrow(context, bytes.get(), &backing, &data, &size),
      JS_OBJECT_BYTES_OK);
  LocalValue ownedBacking(context, backing);
  ASSERT_EQ(size, expectedSize);
  EXPECT_EQ(std::memcmp(data, expectedBytes, expectedSize), 0);
  EXPECT_FALSE(JS_HasException(context));
}

void measureQuickJSStringRequest(JSContext *context,
                                 AllocatorControl &allocator, char const *text,
                                 std::size_t &requestSize) {
  std::size_t const liveBefore = allocator.liveBlocks;
  allocator.startRecording();
  JSValue raw = JS_NewStringLen(context, text, std::strlen(text));
  allocator.stopRecording();
  {
    LocalValue value(context, raw);
    ASSERT_FALSE(JS_IsException(value.get()));
    ASSERT_EQ(allocator.recordedCount, 1u);
    ASSERT_LE(allocator.recordedCount, allocator.recorded.size());
    EXPECT_EQ(allocator.recorded[0].kind, RequestKind::allocate);
    requestSize = allocator.recorded[0].size;
  }
  EXPECT_EQ(allocator.liveBlocks, liveBefore);
}

void expectMaterializerAllocationProfile(AllocatorControl const &allocator,
                                         MaterializerOOMCase const &testCase,
                                         std::size_t quickJSStringRequestSize) {
  ASSERT_GT(allocator.recordedCount, 0u);
  ASSERT_LE(allocator.recordedCount, allocator.recorded.size());

  std::size_t stringRequests = 0;
  for (std::size_t i = 0; i < allocator.recordedCount; ++i) {
    auto const &request = allocator.recorded[i];
    if (request.kind == RequestKind::allocate &&
        request.size == quickJSStringRequestSize)
      ++stringRequests;
  }
  EXPECT_GT(stringRequests, 0u)
      << testCase.name << ": calibrated QuickJS string request is absent";

  if (testCase.temporaryProviderBytes == 0)
    return;

  std::size_t adjacentPairs = 0;
  for (std::size_t i = 0; i + 1 < allocator.recordedCount; ++i) {
    auto const &temporary = allocator.recorded[i];
    auto const &string = allocator.recorded[i + 1];
    if (temporary.kind == RequestKind::allocate &&
        temporary.size == testCase.temporaryProviderBytes &&
        string.kind == RequestKind::allocate &&
        string.size == quickJSStringRequestSize)
      ++adjacentPairs;
  }
  EXPECT_EQ(adjacentPairs, 1u)
      << testCase.name
      << ": long Blob must allocate its provider hex buffer immediately before "
         "the QuickJS string";
}

void exerciseSameRuntimeMaterializerOOM(JSContext *context,
                                        AllocatorControl &allocator,
                                        MaterializerOOMCase const &testCase) {
  SCOPED_TRACE(testCase.name);
  std::uint8_t wire[1024];
  auto const wireSize = decodeHex(testCase.wireHex, wire, sizeof(wire));
  ASSERT_NE(wireSize, std::numeric_limits<std::uint32_t>::max());
  std::array<std::uint8_t, sizeof(wire)> original{};
  std::memcpy(original.data(), wire, wireSize);

  // Stabilize field atoms, output shapes, and nominal field prototypes on a
  // sacrificial owner. The measured owner and its selected cache entry are
  // then immutable evidence throughout every injected attempt.
  {
    LocalValue warmRoot(
        context, types::makeCertifiedObjectCopy(context, wire, wireSize));
    ASSERT_FALSE(JS_IsException(warmRoot.get()));
    void const *warmIdentity = JS_VALUE_GET_PTR(warmRoot.get());
    LocalValue warmToJSON(context,
                          JS_GetPropertyStr(context, warmRoot.get(), "toJSON"));
    LocalValue warmToBytes(
        context, JS_GetPropertyStr(context, warmRoot.get(), "toBytes"));
    LocalValue warmField(context, JS_GetPropertyStr(context, warmRoot.get(),
                                                    testCase.fieldName));
    ASSERT_FALSE(JS_IsException(warmToJSON.get()));
    ASSERT_FALSE(JS_IsException(warmToBytes.get()));
    ASSERT_FALSE(JS_IsException(warmField.get()));
    ASSERT_TRUE(JS_IsObject(warmField.get()) || JS_IsString(warmField.get()));
    void const *warmFieldIdentity = JS_VALUE_GET_PTR(warmField.get());
    for (int pass = 0; pass < 2; ++pass) {
      LocalValue warm(context, JS_Call(context, warmToJSON.get(),
                                       warmRoot.get(), 0, nullptr));
      ASSERT_FALSE(JS_IsException(warm.get()));
      expectJSONValue(context, warm.get(), testCase.expectedJSON);
      expectMaterializerSourceUnchanged(context, warmRoot.get(), warmIdentity,
                                        warmToBytes.get(), testCase.fieldName,
                                        warmField.get(), warmFieldIdentity,
                                        original.data(), wireSize, 0);
    }
  }
  std::size_t const liveAfterWarmOwner = allocator.liveBlocks;

  {
    LocalValue root(context,
                    types::makeCertifiedObjectCopy(context, wire, wireSize));
    ASSERT_FALSE(JS_IsException(root.get()));
    void const *rootIdentity = JS_VALUE_GET_PTR(root.get());
    LocalValue toJSON(context,
                      JS_GetPropertyStr(context, root.get(), "toJSON"));
    LocalValue toBytes(context,
                       JS_GetPropertyStr(context, root.get(), "toBytes"));
    LocalValue cachedField(
        context, JS_GetPropertyStr(context, root.get(), testCase.fieldName));
    ASSERT_FALSE(JS_IsException(toJSON.get()));
    ASSERT_FALSE(JS_IsException(toBytes.get()));
    ASSERT_FALSE(JS_IsException(cachedField.get()));
    ASSERT_TRUE(JS_IsObject(cachedField.get()) ||
                JS_IsString(cachedField.get()));
    void const *fieldIdentity = JS_VALUE_GET_PTR(cachedField.get());

    std::size_t quickJSStringRequestSize = 0;
    measureQuickJSStringRequest(context, allocator, testCase.renderedString,
                                quickJSStringRequestSize);
    ASSERT_GT(quickJSStringRequestSize, 0u);
    std::size_t const liveBefore = allocator.liveBlocks;

    allocator.startRecording();
    JSValue measuredValue =
        JS_Call(context, toJSON.get(), root.get(), 0, nullptr);
    allocator.stopRecording();
    ASSERT_LE(allocator.recordedCount, allocator.recorded.size());
    std::size_t const requestCount = allocator.recordedCount;
    auto const successfulRequests = allocator.recorded;
    expectMaterializerAllocationProfile(allocator, testCase,
                                        quickJSStringRequestSize);
    {
      LocalValue measured(context, measuredValue);
      ASSERT_FALSE(JS_IsException(measured.get()));
      expectJSONValue(context, measured.get(), testCase.expectedJSON);
    }
    expectMaterializerSourceUnchanged(
        context, root.get(), rootIdentity, toBytes.get(), testCase.fieldName,
        cachedField.get(), fieldIdentity, original.data(), wireSize, 0);
    EXPECT_EQ(std::memcmp(wire, original.data(), wireSize), 0);
    ASSERT_GT(requestCount, 0u);
    ASSERT_EQ(allocator.liveBlocks, liveBefore);

    for (std::size_t ordinal = 1; ordinal <= requestCount; ++ordinal) {
      SCOPED_TRACE(ordinal);
      std::size_t const rejectionsBefore = allocator.rejections;
      allocator.startRecording();
      allocator.rejectAt = allocator.requests + ordinal;
      JSValue failed = JS_Call(context, toJSON.get(), root.get(), 0, nullptr);
      allocator.stopRecording();
      allocator.rejectAt = 0;

      EXPECT_TRUE(JS_IsException(failed));
      ASSERT_EQ(allocator.rejections, rejectionsBefore + 1);
      ASSERT_GE(allocator.recordedCount, ordinal);
      ASSERT_LE(ordinal, successfulRequests.size());
      for (std::size_t i = 0; i < ordinal; ++i)
        EXPECT_EQ(allocator.recorded[i], successfulRequests[i])
            << "request prefix at ordinal " << ordinal;
      expectOnePendingOOM(context, ordinal);
      EXPECT_EQ(std::memcmp(wire, original.data(), wireSize), 0);
      expectMaterializerSourceUnchanged(
          context, root.get(), rootIdentity, toBytes.get(), testCase.fieldName,
          cachedField.get(), fieldIdentity, original.data(), wireSize, ordinal);
      EXPECT_EQ(allocator.liveBlocks, liveBefore)
          << "partial JSON value escaped at ordinal " << ordinal;

      std::size_t const retryBefore = allocator.requests;
      JSValue retryValue =
          JS_Call(context, toJSON.get(), root.get(), 0, nullptr);
      std::size_t const retryRequestCount = allocator.requests - retryBefore;
      {
        LocalValue retry(context, retryValue);
        ASSERT_FALSE(JS_IsException(retry.get()));
        expectJSONValue(context, retry.get(), testCase.expectedJSON, ordinal);
      }
      EXPECT_EQ(retryRequestCount, requestCount);
      EXPECT_FALSE(JS_HasException(context));
      EXPECT_EQ(std::memcmp(wire, original.data(), wireSize), 0);
      expectMaterializerSourceUnchanged(
          context, root.get(), rootIdentity, toBytes.get(), testCase.fieldName,
          cachedField.get(), fieldIdentity, original.data(), wireSize, ordinal);
      EXPECT_EQ(allocator.liveBlocks, liveBefore);
    }
  }
  EXPECT_EQ(allocator.liveBlocks, liveAfterWarmOwner);
}

TEST(CanonicalJSONOOM,
     RemainingAllocationDistinctMaterializersRetryInOneRuntime) {
  static constexpr char longBlob[] = "000102030405060708090A0B0C0D0E0F"
                                     "101112131415161718191A1B1C1D1E1F20";
  static constexpr MaterializerOOMCase cases[] = {
      {
          "UInt64DecimalString",
          "IndexNext",
          "31FFFFFFFFFFFFFFFF",
          "{\"IndexNext\":\"18446744073709551615\"}",
          "18446744073709551615",
      },
      {
          "NumberCanonicalDecimalString",
          "Number",
          "91000470DE4DF82000FFFFFFF1",
          "{\"Number\":\"1.25\"}",
          "1.25",
      },
      {
          "LongBlobProviderAndQuickJSString",
          "MemoData",
          "7D21"
          "000102030405060708090A0B0C0D0E0F"
          "101112131415161718191A1B1C1D1E1F20",
          "{\"MemoData\":\""
          "000102030405060708090A0B0C0D0E0F"
          "101112131415161718191A1B1C1D1E1F20\"}",
          longBlob,
          sizeof(longBlob) - 1,
      },
  };

  AllocatorControl allocator;
  JSRuntime *runtime = JS_NewRuntime2(&testAllocator, &allocator);
  ASSERT_NE(runtime, nullptr);
  JSContext *context = JS_NewContext(runtime);
  ASSERT_NE(context, nullptr);
  qjs::resetByteClassRegistry();
  {
    LocalValue global(context, JS_GetGlobalObject(context));
    ASSERT_FALSE(JS_IsException(global.get()));
    ASSERT_TRUE(types::registerSTBlob(context, global.get()));
  }
  ASSERT_TRUE(register_uint_types(context));
  ASSERT_TRUE(types::registerObjectTypes(context));
  ASSERT_FALSE(JS_HasException(context));

  EXPECT_TRUE(JS_IsException(JS_ThrowOutOfMemory(context)));
  expectOnePendingOOM(context, 0);
  for (auto const &testCase : cases)
    exerciseSameRuntimeMaterializerOOM(context, allocator, testCase);

  JS_FreeContext(context);
  types::unregisterObjectTypes(runtime);
  qjs::resetByteClassRegistry();
  JS_FreeRuntime(runtime);
  EXPECT_EQ(allocator.liveBlocks, 0u);
}

TEST(CanonicalJSONOOM, RecursiveObjectArrayRichTreeRetriesInOneRuntime) {
  static constexpr char wireHex[] =
      // Root Memos -> element Memo -> all seven rich JSON routes.
      "F9EA"
      "61D4838D7EA4C68000"
      "0000000000000000000000005553440000000000"
      "B5F762798A53D543A014CAF8B297CFF8F2F937E8"
      "8114B5F762798A53D543A014CAF8B297CFF8F2F937E8"
      "011201B5F762798A53D543A014CAF8B297CFF8F2F937E800"
      "011320"
      "000102030405060708090A0B0C0D0E0F"
      "101112131415161718191A1B1C1D1E1F"
      "01180000000000000000000000000000000000000000"
      "0119"
      "14B5F762798A53D543A014CAF8B297CFF8F2F937E8"
      "0000000000000000000000000000000000000000"
      "14B5F762798A53D543A014CAF8B297CFF8F2F937E8"
      "0000000000000000000000000000000000000000"
      "011A0000000000000000000000000000000000000000"
      "E1F1";
  static constexpr char expectedJSON[] =
      "{\"Memos\":[{\"Memo\":{"
      "\"Amount\":{\"currency\":\"USD\","
      "\"issuer\":\"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh\","
      "\"value\":\"1\"},"
      "\"Account\":\"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh\","
      "\"Paths\":[[{\"account\":"
      "\"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh\",\"type\":1}]],"
      "\"Indexes\":["
      "\"000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F\"],"
      "\"LockingChainIssue\":{\"currency\":\"XAH\"},"
      "\"XChainBridge\":{"
      "\"IssuingChainDoor\":\"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh\","
      "\"IssuingChainIssue\":{\"currency\":\"XAH\"},"
      "\"LockingChainDoor\":\"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh\","
      "\"LockingChainIssue\":{\"currency\":\"XAH\"}},"
      "\"BaseAsset\":\"XAH\"}}]}";

  std::uint8_t wire[1024];
  auto const wireSize = decodeHex(wireHex, wire, sizeof(wire));
  ASSERT_NE(wireSize, std::numeric_limits<std::uint32_t>::max());
  std::array<std::uint8_t, sizeof(wire)> original{};
  std::memcpy(original.data(), wire, wireSize);

  AllocatorControl allocator;
  JSRuntime *runtime = JS_NewRuntime2(&testAllocator, &allocator);
  ASSERT_NE(runtime, nullptr);
  JSContext *context = JS_NewContext(runtime);
  ASSERT_NE(context, nullptr);
  ASSERT_TRUE(types::registerObjectTypes(context));

  EXPECT_TRUE(JS_IsException(JS_ThrowOutOfMemory(context)));
  expectOnePendingOOM(context, 0);

  // Warm runtime-owned atoms and shapes on a sacrificial owner. The measured
  // owner below has never traversed JSON when its source-state baseline is
  // captured, so retained-block equality also detects illicit cache writes.
  {
    LocalValue warmRoot(
        context, types::makeCertifiedObjectCopy(context, wire, wireSize));
    ASSERT_FALSE(JS_IsException(warmRoot.get()));
    void const *warmIdentity = JS_VALUE_GET_PTR(warmRoot.get());
    LocalValue warmToJSON(context,
                          JS_GetPropertyStr(context, warmRoot.get(), "toJSON"));
    LocalValue warmToBytes(
        context, JS_GetPropertyStr(context, warmRoot.get(), "toBytes"));
    LocalValue warmMemos(context,
                         JS_GetPropertyStr(context, warmRoot.get(), "Memos"));
    LocalValue warmElement(context,
                           JS_GetPropertyUint32(context, warmMemos.get(), 0));
    LocalValue warmMemo(context,
                        JS_GetPropertyStr(context, warmElement.get(), "Memo"));
    ASSERT_FALSE(JS_IsException(warmToJSON.get()));
    ASSERT_FALSE(JS_IsException(warmToBytes.get()));
    ASSERT_FALSE(JS_IsException(warmMemos.get()));
    ASSERT_FALSE(JS_IsException(warmElement.get()));
    ASSERT_FALSE(JS_IsException(warmMemo.get()));
    for (int pass = 0; pass < 2; ++pass) {
      LocalValue warm(context, JS_Call(context, warmToJSON.get(),
                                       warmRoot.get(), 0, nullptr));
      ASSERT_FALSE(JS_IsException(warm.get()));
      expectJSONValue(context, warm.get(), expectedJSON);
      expectCertifiedSourceUnchanged(context, warmRoot.get(), warmIdentity,
                                     warmToBytes.get(), warmMemos.get(),
                                     warmElement.get(), warmMemo.get(),
                                     original.data(), wireSize, 0);
    }
  }
  std::size_t const liveAfterWarmOwner = allocator.liveBlocks;

  {
    LocalValue root(context,
                    types::makeCertifiedObjectCopy(context, wire, wireSize));
    ASSERT_FALSE(JS_IsException(root.get()));
    void const *rootIdentity = JS_VALUE_GET_PTR(root.get());
    LocalValue toJSON(context,
                      JS_GetPropertyStr(context, root.get(), "toJSON"));
    LocalValue toBytes(context,
                       JS_GetPropertyStr(context, root.get(), "toBytes"));
    LocalValue memos(context, JS_GetPropertyStr(context, root.get(), "Memos"));
    LocalValue element(context, JS_GetPropertyUint32(context, memos.get(), 0));
    LocalValue memo(context, JS_GetPropertyStr(context, element.get(), "Memo"));
    ASSERT_FALSE(JS_IsException(toJSON.get()));
    ASSERT_FALSE(JS_IsException(toBytes.get()));
    ASSERT_FALSE(JS_IsException(memos.get()));
    ASSERT_FALSE(JS_IsException(element.get()));
    ASSERT_FALSE(JS_IsException(memo.get()));
    std::size_t const liveBefore = allocator.liveBlocks;

    std::size_t const requestBefore = allocator.requests;
    JSValue measuredValue =
        JS_Call(context, toJSON.get(), root.get(), 0, nullptr);
    std::size_t const requestCount = allocator.requests - requestBefore;
    {
      LocalValue measured(context, measuredValue);
      ASSERT_FALSE(JS_IsException(measured.get()));
      expectJSONValue(context, measured.get(), expectedJSON);
    }
    expectCertifiedSourceUnchanged(context, root.get(), rootIdentity,
                                   toBytes.get(), memos.get(), element.get(),
                                   memo.get(), original.data(), wireSize, 0);
    ASSERT_GT(requestCount, 0u);
    ASSERT_EQ(allocator.liveBlocks, liveBefore);

    for (std::size_t ordinal = 1; ordinal <= requestCount; ++ordinal) {
      SCOPED_TRACE(ordinal);
      std::size_t const rejectionsBefore = allocator.rejections;
      allocator.rejectAt = allocator.requests + ordinal;
      JSValue failed = JS_Call(context, toJSON.get(), root.get(), 0, nullptr);
      allocator.rejectAt = 0;

      EXPECT_TRUE(JS_IsException(failed));
      ASSERT_EQ(allocator.rejections, rejectionsBefore + 1);
      expectOnePendingOOM(context, ordinal);
      EXPECT_EQ(std::memcmp(wire, original.data(), wireSize), 0);
      expectCertifiedSourceUnchanged(
          context, root.get(), rootIdentity, toBytes.get(), memos.get(),
          element.get(), memo.get(), original.data(), wireSize, ordinal);
      EXPECT_EQ(allocator.liveBlocks, liveBefore);

      {
        LocalValue retry(
            context, JS_Call(context, toJSON.get(), root.get(), 0, nullptr));
        ASSERT_FALSE(JS_IsException(retry.get()));
        expectJSONValue(context, retry.get(), expectedJSON, ordinal);
      }
      expectCertifiedSourceUnchanged(
          context, root.get(), rootIdentity, toBytes.get(), memos.get(),
          element.get(), memo.get(), original.data(), wireSize, ordinal);
      EXPECT_EQ(allocator.liveBlocks, liveBefore);
    }
  }
  EXPECT_EQ(allocator.liveBlocks, liveAfterWarmOwner);

  types::unregisterObjectTypes(runtime);
  JS_FreeContext(context);
  JS_FreeRuntime(runtime);
  EXPECT_EQ(allocator.liveBlocks, 0u);
}

} // namespace
