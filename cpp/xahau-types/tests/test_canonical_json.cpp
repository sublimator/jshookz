#include "object/canonical_json.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {

namespace types = jshookz::provider::types;

using Factory = JSValue (*)(JSContext *, std::uint8_t const *,
                            std::uint32_t) noexcept;

class LocalValue {
public:
  LocalValue(JSContext *context, JSValue value) noexcept
      : context_(context), value_(value) {}

  ~LocalValue() { JS_FreeValue(context_, value_); }

  LocalValue(LocalValue const &) = delete;
  LocalValue &operator=(LocalValue const &) = delete;

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

struct AllocatorControl {
  std::size_t requests = 0;
  std::size_t rejectAt = 0;
  std::size_t rejections = 0;
  std::size_t liveBlocks = 0;
};

[[nodiscard]] bool wouldExceed(JSMallocState const *state, std::size_t oldSize,
                               std::size_t newSize) noexcept {
  if (state->malloc_size < oldSize)
    return true;
  std::size_t const retained = state->malloc_size - oldSize;
  return newSize > state->malloc_limit ||
         retained > state->malloc_limit - newSize;
}

[[nodiscard]] bool reject(JSMallocState *state) noexcept {
  auto *control = static_cast<AllocatorControl *>(state->opaque);
  ++control->requests;
  if (control->rejectAt == 0 || control->requests != control->rejectAt)
    return false;
  control->rejectAt = 0;
  ++control->rejections;
  return true;
}

void *testMalloc(JSMallocState *state, std::size_t size) {
  auto *control = static_cast<AllocatorControl *>(state->opaque);
  if (reject(state) || size == 0 ||
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
  if (reject(state))
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

void exerciseOOM(Factory factory, char const *hex) {
  std::uint8_t bytes[1024];
  auto const length = decodeHex(hex, bytes, sizeof(bytes));
  ASSERT_NE(length, std::numeric_limits<std::uint32_t>::max());

  bool reachedSuccess = false;
  for (std::size_t failureOffset = 1; failureOffset <= 96; ++failureOffset) {
    AllocatorControl allocator;
    JSRuntime *runtime = JS_NewRuntime2(&testAllocator, &allocator);
    ASSERT_NE(runtime, nullptr);
    JSContext *context = JS_NewContext(runtime);
    ASSERT_NE(context, nullptr);

    // Warm property atoms and shapes so retained runtime metadata cannot
    // be confused with a leaked partial output tree.
    JSValue warm = factory(context, bytes, length);
    ASSERT_FALSE(JS_IsException(warm));
    JS_FreeValue(context, warm);
    std::size_t const liveBefore = allocator.liveBlocks;

    allocator.rejectAt = allocator.requests + failureOffset;
    JSValue value = factory(context, bytes, length);
    if (allocator.rejections == 0) {
      ASSERT_FALSE(JS_IsException(value));
      JS_FreeValue(context, value);
      reachedSuccess = true;
    } else {
      EXPECT_TRUE(JS_IsException(value));
      EXPECT_TRUE(JS_HasException(context));
      JSValue exception = JS_GetException(context);
      JS_FreeValue(context, exception);
      EXPECT_FALSE(JS_HasException(context));
      EXPECT_EQ(allocator.liveBlocks, liveBefore);
    }

    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    EXPECT_EQ(allocator.liveBlocks, 0u);
    if (reachedSuccess)
      break;
  }
  EXPECT_TRUE(reachedSuccess);
}

TEST(CanonicalJSONOOM, PartialObjectsAndArraysAreReleasedAtomically) {
  exerciseOOM(types::makeAmountCanonicalJSON,
              "D4838D7EA4C68000"
              "0000000000000000000000005553440000000000"
              "B5F762798A53D543A014CAF8B297CFF8F2F937E8");
  exerciseOOM(types::makePathSetCanonicalJSON,
              "31B5F762798A53D543A014CAF8B297CFF8F2F937E8"
              "0000000000000000000000005553440000000000"
              "B5F762798A53D543A014CAF8B297CFF8F2F937E800");
  exerciseOOM(types::makeXChainBridgeCanonicalJSON,
              "14B5F762798A53D543A014CAF8B297CFF8F2F937E8"
              "0000000000000000000000000000000000000000"
              "14B5F762798A53D543A014CAF8B297CFF8F2F937E8"
              "0000000000000000000000000000000000000000");
}

} // namespace
