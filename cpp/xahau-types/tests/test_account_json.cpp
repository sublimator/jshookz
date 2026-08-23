#include "account/account_json.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string_view>

namespace {

using jshookz::provider::types::AccountIDClassicString;
using jshookz::provider::types::encodeAccountIDClassic;
using jshookz::provider::types::makeAccountIDCanonicalJSONString;

constexpr std::uint8_t genesis[20] = {
    0xB5, 0xF7, 0x62, 0x79, 0x8A, 0x53, 0xD5, 0x43, 0xA0, 0x14,
    0xCA, 0xF8, 0xB2, 0x97, 0xCF, 0xF8, 0xF2, 0xF9, 0x37, 0xE8,
};
constexpr std::uint8_t zero[20]{};

TEST(AccountIDCanonicalJSON, MatchesPinnedGenesisAndAllZeroOracle) {
  AccountIDClassicString encoded{};
  ASSERT_TRUE(encodeAccountIDClassic(genesis, sizeof(genesis), &encoded));
  EXPECT_EQ(std::string_view(encoded.chars, encoded.length),
            "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh");

  ASSERT_TRUE(encodeAccountIDClassic(zero, sizeof(zero), &encoded));
  EXPECT_EQ(std::string_view(encoded.chars, encoded.length),
            "rrrrrrrrrrrrrrrrrrrrrhoLvTp");
}

TEST(AccountIDCanonicalJSON, MatchesPinnedBoundaryAndMixedOracleVectors) {
  struct Vector {
    std::uint8_t bytes[20];
    char const *address;
  };
  constexpr Vector vectors[] = {
      {
          {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
          "rrrrrrrrrrrrrrrrrrrrBZbvji",
      },
      {
          {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
           0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
          "rQLbzfJH5BT1FS9apRLKV3G8dWEA5njaQi",
      },
      {
          {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99,
           0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11, 0x22, 0x33},
          "rrMXTieC1jrZSsQne9DaDxGzZKHWwLyo9",
      },
  };
  for (auto const &vector : vectors) {
    AccountIDClassicString encoded{};
    ASSERT_TRUE(
        encodeAccountIDClassic(vector.bytes, sizeof(vector.bytes), &encoded));
    EXPECT_EQ(std::string_view(encoded.chars, encoded.length), vector.address);
  }
}

TEST(AccountIDCanonicalJSON, InvalidNativeInputPublishesNoPartialOutput) {
  AccountIDClassicString output;
  std::memset(&output, 0xA5, sizeof(output));
  EXPECT_FALSE(encodeAccountIDClassic(nullptr, 20, &output));
  EXPECT_EQ(output.length, 0u);
  EXPECT_EQ(output.chars[0], '\0');

  std::memset(&output, 0xA5, sizeof(output));
  EXPECT_FALSE(encodeAccountIDClassic(genesis, 19, &output));
  EXPECT_EQ(output.length, 0u);
  EXPECT_EQ(output.chars[0], '\0');
  EXPECT_FALSE(encodeAccountIDClassic(genesis, 20, nullptr));
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

bool wouldExceed(JSMallocState const *state, std::size_t oldSize,
                 std::size_t newSize) noexcept {
  if (state->malloc_size < oldSize)
    return true;
  std::size_t const retained = state->malloc_size - oldSize;
  return newSize > state->malloc_limit ||
         retained > state->malloc_limit - newSize;
}

bool reject(JSMallocState *state) noexcept {
  auto *control = static_cast<AllocatorControl *>(state->opaque);
  ++control->requests;
  if (control->rejectAt != 0 && control->requests == control->rejectAt) {
    control->rejectAt = 0;
    ++control->rejections;
    return true;
  }
  return false;
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

std::size_t testUsableSize(void const *pointer) {
  if (pointer == nullptr)
    return 0;
  return (static_cast<AllocationHeader const *>(pointer) - 1)->size;
}

JSMallocFunctions const testAllocator = {
    .js_malloc = testMalloc,
    .js_free = testFree,
    .js_realloc = testRealloc,
    .js_malloc_usable_size = testUsableSize,
};

TEST(AccountIDCanonicalJSON, QuickJSStringOOMIsAtomicAndRetryable) {
  AllocatorControl allocator;
  JSRuntime *runtime = JS_NewRuntime2(&testAllocator, &allocator);
  ASSERT_NE(runtime, nullptr);
  JSContext *context = JS_NewContext(runtime);
  ASSERT_NE(context, nullptr);

  std::size_t const liveBefore = allocator.liveBlocks;
  allocator.rejectAt = allocator.requests + 1;
  JSValue failed =
      makeAccountIDCanonicalJSONString(context, genesis, sizeof(genesis));
  EXPECT_TRUE(JS_IsException(failed));
  EXPECT_EQ(allocator.rejections, 1u);
  EXPECT_TRUE(JS_HasException(context));
  JSValue exception = JS_GetException(context);
  EXPECT_TRUE(JS_IsError(context, exception));
  JS_FreeValue(context, exception);
  EXPECT_FALSE(JS_HasException(context));
  EXPECT_EQ(allocator.liveBlocks, liveBefore);

  JSValue retried =
      makeAccountIDCanonicalJSONString(context, genesis, sizeof(genesis));
  ASSERT_FALSE(JS_IsException(retried));
  char const *text = JS_ToCString(context, retried);
  ASSERT_NE(text, nullptr);
  EXPECT_STREQ(text, "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh");
  JS_FreeCString(context, text);
  JS_FreeValue(context, retried);

  JS_FreeContext(context);
  JS_FreeRuntime(runtime);
  EXPECT_EQ(allocator.liveBlocks, 0u);
}

TEST(AccountIDCanonicalJSON, InvalidFactoryInputThrowsWithoutPublishingString) {
  JSRuntime *runtime = JS_NewRuntime();
  ASSERT_NE(runtime, nullptr);
  JSContext *context = JS_NewContext(runtime);
  ASSERT_NE(context, nullptr);
  JSValue failed = makeAccountIDCanonicalJSONString(context, genesis, 19);
  EXPECT_TRUE(JS_IsException(failed));
  EXPECT_TRUE(JS_HasException(context));
  JSValue exception = JS_GetException(context);
  EXPECT_TRUE(JS_IsError(context, exception));
  JS_FreeValue(context, exception);
  JS_FreeContext(context);
  JS_FreeRuntime(runtime);
}

} // namespace
