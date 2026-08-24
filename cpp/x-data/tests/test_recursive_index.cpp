#include "catl/xdata/number-rules.h"
#include "catl/xdata/recursive_index.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <vector>

using namespace catl::xdata;

namespace {

struct TrackingAllocator {
  struct LiveAllocation {
    void *pointer;
    std::size_t size;
  };

  std::uint32_t calls = 0;
  std::uint32_t frees = 0;
  std::uint32_t fail_call = 0;
  std::vector<std::size_t> requests;
  std::vector<LiveAllocation> live;
  std::size_t live_requested = 0;
  std::size_t peak_requested = 0;

  static void *reallocate(void *opaque, void *pointer,
                          std::size_t size) noexcept {
    auto &self = *static_cast<TrackingAllocator *>(opaque);
    ++self.calls;
    self.requests.push_back(size);
    if (self.fail_call != 0 && self.calls == self.fail_call)
      return nullptr;
    auto existing = std::find_if(self.live.begin(), self.live.end(),
                                 [pointer](LiveAllocation const &allocation) {
                                   return allocation.pointer == pointer;
                                 });
    std::size_t const previous =
        existing == self.live.end() ? 0 : existing->size;
    void *replacement = std::realloc(pointer, size);
    if (replacement == nullptr)
      return nullptr;
    self.live_requested -= previous;
    self.live_requested += size;
    self.peak_requested = std::max(self.peak_requested, self.live_requested);
    if (existing == self.live.end())
      self.live.push_back({replacement, size});
    else
      *existing = {replacement, size};
    return replacement;
  }

  static void release(void *opaque, void *pointer) noexcept {
    auto &self = *static_cast<TrackingAllocator *>(opaque);
    if (pointer != nullptr) {
      ++self.frees;
      auto existing = std::find_if(self.live.begin(), self.live.end(),
                                   [pointer](LiveAllocation const &allocation) {
                                     return allocation.pointer == pointer;
                                   });
      if (existing != self.live.end()) {
        self.live_requested -= existing->size;
        self.live.erase(existing);
      }
      std::free(pointer);
    }
  }

  [[nodiscard]] ScanAllocator api() noexcept {
    return {this, reallocate, release};
  }
};

static_assert(sizeof(std::uint64_t[11][6]) == 528);

std::vector<std::uint8_t> bytes(std::initializer_list<std::uint8_t> values) {
  return {values};
}

Slice slice(std::vector<std::uint8_t> const &value) {
  return {value.data(), value.size()};
}

void append_header(std::vector<std::uint8_t> &output,
                   std::uint32_t field_code) {
  std::uint16_t const type = field_code >> 16;
  std::uint16_t const nth = field_code;
  std::uint8_t first = 0;
  if (type < 16)
    first |= static_cast<std::uint8_t>(type << 4);
  if (nth < 16)
    first |= static_cast<std::uint8_t>(nth);
  output.push_back(first);
  if (type >= 16)
    output.push_back(static_cast<std::uint8_t>(type));
  if (nth >= 16)
    output.push_back(static_cast<std::uint8_t>(nth));
}

void expect_exact_index(std::vector<std::uint8_t> const &input,
                        std::initializer_list<std::uint32_t> words) {
  TrackingAllocator memory;
  auto result = guest_exact_object_index(slice(input), {}, memory.api());
  ASSERT_TRUE(result.ok()) << scan_message_literal(result.status.message_id);
  ASSERT_EQ(result.index_size, words.size() * sizeof(std::uint32_t));
  std::vector<std::uint32_t> expected(words);
  EXPECT_EQ(std::memcmp(result.index, expected.data(), result.index_size), 0);
  memory.api().free(memory.api().opaque, result.index);
  EXPECT_TRUE(memory.live.empty());
  EXPECT_EQ(memory.live_requested, 0);
}

std::vector<std::uint8_t>
maximum_topology_input(std::uint32_t elements = 32'767) {
  std::vector<std::uint8_t> input;
  append_header(input, (15u << 16) | 92u); // Amounts array.
  for (std::uint32_t i = 0; i < elements; ++i) {
    append_header(input, (14u << 16) | 10u); // Memo object element.
    input.push_back(0xE1);
  }
  input.push_back(0xF1);
  return input;
}

TEST(RecursiveIndex, EmptyRootHasExactHeaderAndScope) {
  TrackingAllocator memory;
  auto const input = bytes({});
  auto result = guest_exact_object_index(slice(input), {}, memory.api());
  ASSERT_TRUE(result.ok()) << scan_message_literal(result.status.message_id);
  EXPECT_EQ(result.consumed, 0);
  EXPECT_EQ(result.index_size, 32);
  ASSERT_NE(result.index, nullptr);
  RecursiveIndexView const index{result.index, result.index_size,
                                 result.consumed};
  ASSERT_TRUE(index.structurally_valid());
  ASSERT_NE(index.header(), nullptr);
  EXPECT_EQ(index.header()->format_version, 1);
  EXPECT_EQ(index.header()->scope_count, 1);
  EXPECT_EQ(index.header()->field_count, 0);
  ASSERT_NE(index.scope(0), nullptr);
  EXPECT_EQ(index.scope(0)->kind(), ScopeKind::object);
  EXPECT_EQ(index.scope(0)->close_kind(), ScopeCloseKind::eof);
  EXPECT_EQ(memory.requests, std::vector<std::size_t>({32}));
  memory.api().free(memory.api().opaque, result.index);
}

TEST(RecursiveIndex, ObjectSlicesSortByCodeWithoutMovingWireOffsets) {
  // Sequence (2/4) precedes Flags (2/2) on wire, but the exact object slice
  // is sorted by field code for binary lookup.
  auto const input = bytes({0x24, 0, 0, 0, 7, 0x22, 0, 0, 0, 9});
  TrackingAllocator memory;
  RecursiveScanCounters counters;
  RecursiveScanOptions options;
  options.counters = &counters;
  auto result = guest_exact_object_index(slice(input), options, memory.api());
  ASSERT_TRUE(result.ok()) << scan_message_literal(result.status.message_id);
  EXPECT_EQ(result.index_size, 16 + 16 + 2 * 20);
  EXPECT_EQ(counters.wire_passes, 1);
  EXPECT_EQ(counters.scope_entries, 1);
  EXPECT_EQ(counters.field_headers, 2);
  EXPECT_EQ(counters.material_fields, 2);
  EXPECT_EQ(counters.leaf_routes, 2);

  RecursiveIndexView const index{result.index, result.index_size,
                                 result.consumed};
  auto const *flags = index.find_object_field(0, (2u << 16) | 2u);
  auto const *sequence = index.find_object_field(0, (2u << 16) | 4u);
  ASSERT_NE(flags, nullptr);
  ASSERT_NE(sequence, nullptr);
  EXPECT_EQ(flags->header_begin, 5);
  EXPECT_EQ(sequence->header_begin, 0);
  EXPECT_EQ(index.field(0), flags);
  EXPECT_EQ(index.field(1), sequence);
  for (std::uint32_t i = 0; i < 100; ++i) {
    EXPECT_EQ(index.find_object_field(0, (2u << 16) | 2u), flags);
    EXPECT_EQ(index.find_object_field(0, (2u << 16) | 4u), sequence);
  }
  EXPECT_EQ(counters.wire_passes, 1); // Indexed lookup cannot rescan bytes.
  memory.api().free(memory.api().opaque, result.index);
}

TEST(RecursiveIndex, ContiguousNopRunPreservesCountersAndExactFailure) {
  std::vector<std::uint8_t> accepted(63, 0x99);
  accepted.insert(accepted.end(), {0x22, 0, 0, 0, 1});

  TrackingAllocator accepted_memory;
  RecursiveScanCounters accepted_counters;
  RecursiveScanOptions accepted_options;
  accepted_options.counters = &accepted_counters;
  auto indexed = guest_exact_object_index(slice(accepted), accepted_options,
                                          accepted_memory.api());
  ASSERT_TRUE(indexed.ok())
      << scan_message_literal(indexed.status.message_id);
  EXPECT_EQ(indexed.consumed, accepted.size());
  EXPECT_EQ(accepted_counters.wire_passes, 1);
  EXPECT_EQ(accepted_counters.scope_entries, 1);
  EXPECT_EQ(accepted_counters.field_headers, 64);
  EXPECT_EQ(accepted_counters.material_fields, 1);
  EXPECT_EQ(accepted_counters.leaf_routes, 1);

  RecursiveIndexView const view{indexed.index, indexed.index_size,
                                indexed.consumed};
  ASSERT_TRUE(view.structurally_valid());
  ASSERT_EQ(view.header()->scope_count, 1);
  ASSERT_EQ(view.header()->field_count, 1);
  auto const *flags = view.find_object_field(0, (2u << 16) | 2u);
  ASSERT_NE(flags, nullptr);
  EXPECT_EQ(flags->header_begin, 63);
  EXPECT_EQ(flags->payload_begin, 64);
  EXPECT_EQ(flags->wire_end, accepted.size());
  accepted_memory.api().free(accepted_memory.api().opaque, indexed.index);
  EXPECT_TRUE(accepted_memory.live.empty());

  std::vector<std::uint8_t> rejected(64, 0x99);
  TrackingAllocator rejected_memory;
  RecursiveScanCounters rejected_counters;
  RecursiveScanOptions rejected_options;
  rejected_options.counters = &rejected_counters;
  indexed = guest_exact_object_index(slice(rejected), rejected_options,
                                     rejected_memory.api());
  EXPECT_FALSE(indexed.ok());
  EXPECT_EQ(indexed.status.issue,
            static_cast<std::uint16_t>(ScanIssue::malformed_data));
  EXPECT_EQ(indexed.status.message_id,
            static_cast<std::uint16_t>(ScanMessage::too_many_nops));
  EXPECT_EQ(indexed.status.offset, 63);
  EXPECT_EQ(indexed.status.field_code, (9u << 16) | 9u);
  EXPECT_EQ(indexed.status.aux, 64);
  EXPECT_EQ(indexed.index, nullptr);
  EXPECT_EQ(rejected_counters.wire_passes, 1);
  EXPECT_EQ(rejected_counters.scope_entries, 1);
  EXPECT_EQ(rejected_counters.field_headers, 64);
  EXPECT_EQ(rejected_counters.material_fields, 0);
  EXPECT_EQ(rejected_counters.leaf_routes, 0);
  EXPECT_EQ(rejected_memory.calls, 0);
  EXPECT_TRUE(rejected_memory.live.empty());
}

TEST(RecursiveIndex, RecursiveObjectArrayTopologyIsDirect) {
  // Memos array -> Memo object element -> Flags leaf.
  auto const input = bytes({0xF9, 0xEA, 0x22, 0, 0, 0, 1, 0xE1, 0xF1});
  TrackingAllocator memory;
  auto result = guest_exact_object_index(slice(input), {}, memory.api());
  ASSERT_TRUE(result.ok()) << scan_message_literal(result.status.message_id);
  EXPECT_EQ(result.index_size, 16 + 3 * 16 + 3 * 20);
  RecursiveIndexView const index{result.index, result.index_size,
                                 result.consumed};
  ASSERT_TRUE(index.structurally_valid());
  ASSERT_EQ(index.header()->scope_count, 3);
  ASSERT_EQ(index.header()->field_count, 3);

  auto const *memos = index.find_object_field(0, (15u << 16) | 9u);
  ASSERT_NE(memos, nullptr);
  ASSERT_EQ(memos->child_scope, 1);
  auto const *array_scope = index.scope(1);
  ASSERT_NE(array_scope, nullptr);
  EXPECT_EQ(array_scope->kind(), ScopeKind::array);
  EXPECT_EQ(array_scope->close_kind(), ScopeCloseKind::array_end);
  EXPECT_EQ(array_scope->field_count(), 1);

  auto const *memo = index.array_element(1, 0);
  ASSERT_NE(memo, nullptr);
  EXPECT_EQ(memo->field_code, (14u << 16) | 10u);
  ASSERT_EQ(memo->child_scope, 2);
  auto const *object_scope = index.scope(2);
  ASSERT_NE(object_scope, nullptr);
  EXPECT_EQ(object_scope->kind(), ScopeKind::object);
  EXPECT_EQ(object_scope->close_kind(), ScopeCloseKind::object_end);
  ASSERT_NE(index.find_object_field(2, (2u << 16) | 2u), nullptr);
  memory.api().free(memory.api().opaque, result.index);
}

TEST(RecursiveIndex, SameCodeAcrossScopesAndRepeatedArrayCodesStayDistinct) {
  // TemplateEntry{Flags}, Memo{Flags}: duplicate membership is per scope.
  auto const siblings =
      bytes({0xE9, 0x22, 0, 0, 0, 1, 0xE1, 0xEA, 0x22, 0, 0, 0, 2, 0xE1});
  TrackingAllocator memory;
  auto result = guest_exact_object_index(slice(siblings), {}, memory.api());
  ASSERT_TRUE(result.ok()) << scan_message_literal(result.status.message_id);
  RecursiveIndexView index{result.index, result.index_size, result.consumed};
  ASSERT_TRUE(index.structurally_valid());
  auto const *left = index.find_object_field(1, (2u << 16) | 2u);
  auto const *right = index.find_object_field(2, (2u << 16) | 2u);
  ASSERT_NE(left, nullptr);
  ASSERT_NE(right, nullptr);
  EXPECT_NE(left->header_begin, right->header_begin);
  memory.api().free(memory.api().opaque, result.index);

  // One array may repeat its element descriptor; identity is by ordinal.
  auto const repeated = bytes({0xF9, 0xEA, 0xE1, 0xEA, 0xE1, 0xF1});
  TrackingAllocator repeated_memory;
  result = guest_exact_object_index(slice(repeated), {}, repeated_memory.api());
  ASSERT_TRUE(result.ok()) << scan_message_literal(result.status.message_id);
  index = {result.index, result.index_size, result.consumed};
  auto const *first = index.array_element(1, 0);
  auto const *second = index.array_element(1, 1);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(first->field_code, second->field_code);
  EXPECT_NE(first->child_scope, second->child_scope);
  repeated_memory.api().free(repeated_memory.api().opaque, result.index);
}

TEST(RecursiveIndex, PhysicalRootEofClosesAllLiveScopes) {
  for (auto const &input : {
           bytes({0xEA}),
           bytes({0xF9}),
           bytes({0xF9, 0xEA}),
       }) {
    TrackingAllocator memory;
    auto result = guest_exact_object_index(slice(input), {}, memory.api());
    ASSERT_TRUE(result.ok()) << scan_message_literal(result.status.message_id);
    RecursiveIndexView const index{result.index, result.index_size,
                                   result.consumed};
    ASSERT_TRUE(index.structurally_valid());
    for (std::uint32_t i = 0; i < index.header()->scope_count; ++i)
      EXPECT_EQ(index.scope(i)->close_kind(), ScopeCloseKind::eof);
    memory.api().free(memory.api().opaque, result.index);
  }
}

TEST(RecursiveIndex, WrongTerminatorRetainsExactExpectedContext) {
  RecursiveScanOptions options;
  options.protocol = &xahau_static_protocol();

  auto const root_bytes = bytes({0xF1});
  auto root = guest_exact_validate_object(slice(root_bytes), options);
  EXPECT_EQ(root.message_id,
            static_cast<std::uint16_t>(ScanMessage::illegal_terminator));
  EXPECT_EQ(root.field_code, (15u << 16) | 1u);
  EXPECT_EQ(root.aux, static_cast<std::uint32_t>(ExpectedTerminator::root_eof));

  // TemplateEntry opens a nested object, where ArrayEnd is wrong.
  auto const object_bytes = bytes({0xE9, 0xF1});
  auto object = guest_exact_validate_object(slice(object_bytes), options);
  EXPECT_EQ(object.message_id,
            static_cast<std::uint16_t>(ScanMessage::illegal_terminator));
  EXPECT_EQ(object.aux,
            static_cast<std::uint32_t>(ExpectedTerminator::object_end));

  // Memos opens an array, where ObjectEnd is wrong.
  auto const array_bytes = bytes({0xF9, 0xE1});
  auto array = guest_exact_validate_object(slice(array_bytes), options);
  EXPECT_EQ(array.message_id,
            static_cast<std::uint16_t>(ScanMessage::illegal_terminator));
  EXPECT_EQ(array.aux,
            static_cast<std::uint32_t>(ExpectedTerminator::array_end));
}

TEST(RecursiveIndex, FrozenMinimalIndexesHaveExactBytes) {
  expect_exact_index(bytes({0xE1}), {1, 0, 1, 0, 0, 0, 0, 0x20000000});
  expect_exact_index(bytes({0xEA}), {1, 0, 2, 1, 0, 1, 0, 1, 1, 1, 1, 0,
                                     0x000E000A, 0, 1, 1, 1});
  expect_exact_index(bytes({0xF9}), {1, 0, 2, 1, 0, 1, 0, 1, 1, 1, 1,
                                     0x80000000, 0x000F0009, 0, 1, 1, 1});
  expect_exact_index(bytes({0xF9, 0xEA}),
                     {1, 0, 3,          2,          0, 2, 0, 1,          1,
                      2, 1, 0x80000001, 2,          2, 2, 0, 0x000F0009, 0,
                      1, 2, 1,          0x000E000A, 1, 2, 2, 2});
  expect_exact_index(bytes({0xEA, 0xEA}),
                     {1, 0, 3, 2,          0, 2, 0, 1, 1,          2, 1, 1, 2,
                      2, 2, 0, 0x000E000A, 0, 1, 2, 1, 0x000E000A, 1, 2, 2, 2});
  expect_exact_index(bytes({0xEA, 0xE1}), {1, 0, 2, 1, 0, 2, 0, 1, 1, 1, 1,
                                           0x20000000, 0x000E000A, 0, 1, 2, 1});
  expect_exact_index(bytes({0xF9, 0xF1}), {1, 0, 2, 1, 0, 2, 0, 1, 1, 1, 1,
                                           0xC0000000, 0x000F0009, 0, 1, 2, 1});
  expect_exact_index(bytes({0x24, 0, 0, 0, 7, 0x22, 0, 0, 0, 9}),
                     {1, 0, 1, 2, 0, 10, 0, 2, 0x00020002, 5, 6, 10,
                      FieldRecord::no_child, 0x00020004, 0, 1, 5,
                      FieldRecord::no_child});
  expect_exact_index(
      bytes({0xF9, 0xEA, 0xE1, 0xEA, 0xE1, 0xF1}),
      {1, 0,          4,          3,          0, 6, 0,          1,          1,
       5, 1,          0xC0000002, 2,          2, 3, 0x20000000, 4,          4,
       3, 0x20000000, 0x000F0009, 0,          1, 6, 1,          0x000E000A, 1,
       2, 3,          2,          0x000E000A, 3, 4, 5,          3});
}

TEST(RecursiveIndex, ConstructorParityAndGuestExactHaveDifferentRootLaw) {
  auto const input = bytes({0xE1, 0xFF});
  auto const parity = constructor_parity_scan(slice(input), 0, {});
  ASSERT_TRUE(parity.status.ok());
  EXPECT_EQ(parity.consumed, 1);
  auto const exact = guest_exact_validate_object(slice(input), {});
  EXPECT_FALSE(exact.ok());
  EXPECT_EQ(exact.message_id,
            static_cast<std::uint16_t>(ScanMessage::trailing_bytes));

  TrackingAllocator memory;
  auto indexed = guest_exact_object_index(slice(input), {}, memory.api());
  EXPECT_FALSE(indexed.ok());
  EXPECT_EQ(indexed.index, nullptr);
  EXPECT_EQ(memory.calls, 0);
  EXPECT_EQ(memory.frees, 0);
}

TEST(RecursiveIndex, DuplicateAndVectorShapeAreCertified) {
  auto const duplicate = bytes({0x22, 0, 0, 0, 1, 0x22, 0, 0, 0, 2});
  auto status = guest_exact_validate_object(slice(duplicate), {});
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.message_id,
            static_cast<std::uint16_t>(ScanMessage::duplicate_field));

  // Vector256 type 19, nth 1, VL payload length 1.
  auto const malformed_vector = bytes({0x01, 0x13, 0x01, 0x00});
  status = guest_exact_validate_object(slice(malformed_vector), {});
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.message_id,
            static_cast<std::uint16_t>(ScanMessage::invalid_vector256));
}

TEST(RecursiveIndex, NumberUsesPinnedConstructorNormalizationLaw) {
  NormalizedNumber normalized;
  ASSERT_TRUE(NumberRules::normalize(1, 0, normalized));
  EXPECT_EQ(normalized.mantissa, 1'000'000'000'000'000LL);
  EXPECT_EQ(normalized.exponent, -15);

  ASSERT_TRUE(NumberRules::normalize(0, 1234, normalized));
  EXPECT_EQ(normalized.mantissa, 0);
  EXPECT_EQ(normalized.exponent, std::numeric_limits<std::int32_t>::min());

  ASSERT_TRUE(NumberRules::normalize(10'000'000'000'000'005LL, 0, normalized));
  EXPECT_EQ(normalized.mantissa, 1'000'000'000'000'000LL);
  EXPECT_EQ(normalized.exponent, 1); // Exact half rounds to even.
  ASSERT_TRUE(NumberRules::normalize(10'000'000'000'000'015LL, 0, normalized));
  EXPECT_EQ(normalized.mantissa, 1'000'000'000'000'002LL);
  EXPECT_EQ(normalized.exponent, 1);
  ASSERT_TRUE(NumberRules::normalize(std::numeric_limits<std::int64_t>::min(),
                                     32'765, normalized));
  EXPECT_EQ(normalized.mantissa, -9'223'372'036'854'776LL);
  EXPECT_EQ(normalized.exponent, 32'768);
  EXPECT_FALSE(NumberRules::normalize(std::numeric_limits<std::int64_t>::max(),
                                      32'766, normalized));

  // Number field (type 9/nth 1), then a non-normal pair accepted and
  // normalized by Xahau's STNumber constructor.
  auto const noncanonical = bytes({0x91, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0});
  EXPECT_TRUE(guest_exact_validate_object(slice(noncanonical), {}).ok());

  // INT64_MAX at exponent 32768 must divide before it is representable;
  // pinned Number::normalize throws instead of crossing maxExponent.
  auto const overflow = bytes({0x91, 0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                               0xff, 0x00, 0x00, 0x80, 0x00});
  auto const status = guest_exact_validate_object(slice(overflow), {});
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.message_id,
            static_cast<std::uint16_t>(ScanMessage::invalid_number));
}

TEST(RecursiveIndex, RawFieldPayloadRequiresExactCanonicalValueBytes) {
  auto const flags = bytes({0, 0, 0, 9});
  EXPECT_TRUE(
      guest_exact_validate_field_payload(slice(flags), (2u << 16) | 2u, 0, {})
          .ok());
  auto const short_flags = bytes({0, 0, 9});
  EXPECT_EQ(guest_exact_validate_field_payload(slice(short_flags),
                                               (2u << 16) | 2u, 0, {})
                .message_id,
            static_cast<std::uint16_t>(ScanMessage::truncated_field));

  auto const account = bytes({});
  EXPECT_TRUE(
      guest_exact_validate_field_payload(slice(account), (8u << 16) | 1u, 0, {})
          .ok());
  auto const malformed_account = bytes({1});
  EXPECT_EQ(guest_exact_validate_field_payload(slice(malformed_account),
                                               (8u << 16) | 1u, 0, {})
                .message_id,
            static_cast<std::uint16_t>(ScanMessage::invalid_account_id));

  std::vector<std::uint8_t> vector(64, 0x5a);
  EXPECT_TRUE(
      guest_exact_validate_field_payload(slice(vector), (19u << 16) | 1u, 0, {})
          .ok());
  vector.pop_back();
  EXPECT_EQ(
      guest_exact_validate_field_payload(slice(vector), (19u << 16) | 1u, 0, {})
          .message_id,
      static_cast<std::uint16_t>(ScanMessage::invalid_vector256));
}

TEST(RecursiveIndex, RawNumberPayloadRejectsAdmittedNoncanonicalPair) {
  auto const canonical = bytes(
      {0x00, 0x03, 0x8d, 0x7e, 0xa4, 0xc6, 0x80, 0x00, 0xff, 0xff, 0xff, 0xf1});
  ASSERT_EQ(canonical.size(), 12);
  EXPECT_TRUE(guest_exact_validate_field_payload(slice(canonical),
                                                 (9u << 16) | 1u, 0, {})
                  .ok());

  auto const admitted_noncanonical =
      bytes({0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0});
  EXPECT_TRUE(guest_exact_validate_object(
                  slice(bytes({0x91, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0})), {})
                  .ok());
  EXPECT_EQ(guest_exact_validate_field_payload(slice(admitted_noncanonical),
                                               (9u << 16) | 1u, 0, {})
                .message_id,
            static_cast<std::uint16_t>(ScanMessage::invalid_number));
}

TEST(RecursiveIndex, DecimalNumberParserMatchesCanonicalOraclePairs) {
  auto expect = [](char const *text, std::int64_t mantissa,
                   std::int32_t exponent) {
    NormalizedNumber parsed;
    ASSERT_TRUE(NumberRules::parse_decimal(text, std::strlen(text), parsed));
    EXPECT_EQ(parsed.mantissa, mantissa) << text;
    EXPECT_EQ(parsed.exponent, exponent) << text;
  };
  expect("1", 1'000'000'000'000'000LL, -15);
  expect("1.25", 1'250'000'000'000'000LL, -15);
  expect("1e20", 1'000'000'000'000'000LL, 5);
  expect("-0.00125", -1'250'000'000'000'000LL, -18);
  expect("10000000000000005", 1'000'000'000'000'000LL, 1);
  expect("10000000000000015", 1'000'000'000'000'002LL, 1);

  NormalizedNumber parsed;
  ASSERT_TRUE(NumberRules::parse_decimal("-0", 2, parsed));
  EXPECT_EQ(parsed.mantissa, 0);
  EXPECT_EQ(parsed.exponent, std::numeric_limits<std::int32_t>::min());
  EXPECT_FALSE(NumberRules::parse_decimal("1e-999999", 9, parsed));
  EXPECT_FALSE(NumberRules::parse_decimal("1e999999", 8, parsed));
  EXPECT_FALSE(NumberRules::parse_decimal(".", 1, parsed));
  EXPECT_FALSE(NumberRules::parse_decimal(".5", 2, parsed));
  EXPECT_FALSE(NumberRules::parse_decimal("1.", 2, parsed));
  EXPECT_FALSE(NumberRules::parse_decimal("0001.2500", 9, parsed));
  EXPECT_FALSE(NumberRules::parse_decimal("1.2.3", 5, parsed));
  EXPECT_FALSE(NumberRules::parse_decimal("1e", 2, parsed));
}

TEST(RecursiveIndex, RawContainerPayloadRequiresCanonicalOrderAndClose) {
  auto const canonical = bytes({0x22, 0, 0, 0, 1, 0x24, 0, 0, 0, 2, 0xE1});
  EXPECT_TRUE(guest_exact_validate_field_payload(slice(canonical),
                                                 (14u << 16) | 10u, 0, {})
                  .ok());

  auto const out_of_order = bytes({0x24, 0, 0, 0, 2, 0x22, 0, 0, 0, 1, 0xE1});
  EXPECT_EQ(guest_exact_validate_field_payload(slice(out_of_order),
                                               (14u << 16) | 10u, 0, {})
                .message_id,
            static_cast<std::uint16_t>(ScanMessage::noncanonical_payload));

  auto const nop = bytes({0x99, 0xE1});
  EXPECT_EQ(
      guest_exact_validate_field_payload(slice(nop), (14u << 16) | 10u, 0, {})
          .message_id,
      static_cast<std::uint16_t>(ScanMessage::noncanonical_payload));

  auto const missing_close = bytes({0x22, 0, 0, 0, 1});
  EXPECT_EQ(guest_exact_validate_field_payload(slice(missing_close),
                                               (14u << 16) | 10u, 0, {})
                .message_id,
            static_cast<std::uint16_t>(ScanMessage::noncanonical_payload));

  auto const empty_array = bytes({0xF1});
  EXPECT_TRUE(guest_exact_validate_field_payload(slice(empty_array),
                                                 (15u << 16) | 92u, 0, {})
                  .ok());
}

TEST(RecursiveIndex, RawContainerPayloadHonorsDestinationDepthBeforeEncoding) {
  auto const empty_object = bytes({0xE1});
  EXPECT_TRUE(guest_exact_validate_field_payload(slice(empty_object),
                                                 (14u << 16) | 10u, 9, {})
                  .ok());
  EXPECT_EQ(guest_exact_validate_field_payload(slice(empty_object),
                                               (14u << 16) | 10u, 10, {})
                .message_id,
            static_cast<std::uint16_t>(ScanMessage::nesting_too_deep));

  auto const nested = bytes({0xEA, 0xE1, 0xE1});
  EXPECT_EQ(guest_exact_validate_field_payload(slice(nested), (14u << 16) | 10u,
                                               9, {})
                .message_id,
            static_cast<std::uint16_t>(ScanMessage::nesting_too_deep));
}

TEST(RecursiveIndex, DepthTenIsIndexedAndDepthElevenRejects) {
  std::vector<std::uint8_t> depth_ten(10, 0xEA);
  TrackingAllocator memory;
  auto indexed = guest_exact_object_index(slice(depth_ten), {}, memory.api());
  ASSERT_TRUE(indexed.ok()) << scan_message_literal(indexed.status.message_id);
  RecursiveIndexView const view{indexed.index, indexed.index_size,
                                indexed.consumed};
  ASSERT_EQ(view.header()->scope_count, 11);
  ASSERT_EQ(view.header()->field_count, 10);
  for (std::uint32_t i = 0; i < 10; ++i) {
    auto const *current = view.scope(i);
    ASSERT_NE(current, nullptr);
    EXPECT_EQ(current->content_begin, i);
    EXPECT_EQ(current->content_end, 10);
    EXPECT_EQ(current->first_field, i);
    EXPECT_EQ(current->field_count(), 1);
    EXPECT_EQ(current->close_kind(), ScopeCloseKind::eof);
    auto const *nested = view.find_object_field(i, (14u << 16) | 10u);
    ASSERT_NE(nested, nullptr);
    EXPECT_EQ(nested->header_begin, i);
    EXPECT_EQ(nested->payload_begin, i + 1);
    EXPECT_EQ(nested->wire_end, 10);
    EXPECT_EQ(nested->child_scope, i + 1);
  }
  auto const *deepest = view.scope(10);
  ASSERT_NE(deepest, nullptr);
  EXPECT_EQ(deepest->content_begin, 10);
  EXPECT_EQ(deepest->content_end, 10);
  EXPECT_EQ(deepest->first_field, 10);
  EXPECT_EQ(deepest->field_count(), 0);
  memory.api().free(memory.api().opaque, indexed.index);

  std::vector<std::uint8_t> depth_eleven(11, 0xEA);
  TrackingAllocator rejected;
  indexed = guest_exact_object_index(slice(depth_eleven), {}, rejected.api());
  EXPECT_FALSE(indexed.ok());
  EXPECT_EQ(indexed.status.message_id,
            static_cast<std::uint16_t>(ScanMessage::nesting_too_deep));
  EXPECT_EQ(indexed.index, nullptr);
  EXPECT_TRUE(rejected.live.empty());
}

TEST(RecursiveIndex, MoreThanSixteenExtendedFieldsAreNotFalseDuplicates) {
  std::vector<std::uint8_t> input;
  auto const &protocol = xahau_static_protocol();
  std::uint32_t selected = 0;
  for (std::uint16_t i = 0; i < protocol.field_count && selected < 17; ++i) {
    auto const *descriptor = protocol.field_by_ordinal(i);
    if (descriptor->material_ordinal == ProtocolView::no_ordinal ||
        descriptor->header_size < 2 || descriptor->fixed_size == 0 ||
        descriptor->wire_type == 14 || descriptor->wire_type == 15)
      continue;
    append_header(input, descriptor->code);
    input.insert(input.end(), descriptor->fixed_size, 0);
    ++selected;
  }
  ASSERT_EQ(selected, 17);
  EXPECT_TRUE(guest_exact_validate_object(slice(input), {}).ok());
}

TEST(RecursiveIndex, EveryLimitTurnsRedBeforeProhibitedGrowth) {
  auto const one_field = bytes({0x22, 0, 0, 0, 1});
  auto const two_fields = bytes({0x22, 0, 0, 0, 1, 0x24, 0, 0, 0, 2});
  auto const child = bytes({0xEA, 0xE1});

  RecursiveScanOptions options;
  options.limits.max_bytes = 4;
  EXPECT_EQ(guest_exact_validate_object(slice(one_field), options).message_id,
            static_cast<std::uint16_t>(ScanMessage::input_too_large));

  options = {};
  options.limits.max_fields = 1;
  EXPECT_EQ(guest_exact_validate_object(slice(two_fields), options).message_id,
            static_cast<std::uint16_t>(ScanMessage::too_many_fields));

  options = {};
  options.limits.max_scopes = 1;
  EXPECT_EQ(guest_exact_validate_object(slice(child), options).message_id,
            static_cast<std::uint16_t>(ScanMessage::too_many_scopes));

  options = {};
  options.limits.max_depth = 0;
  EXPECT_EQ(guest_exact_validate_object(slice(child), options).message_id,
            static_cast<std::uint16_t>(ScanMessage::nesting_too_deep));
}

TEST(RecursiveIndex, CapCrossingsDoNotCauseTheCrossingGrowth) {
  std::vector<std::uint8_t> nine_fields;
  auto const &protocol = xahau_static_protocol();
  std::uint32_t selected = 0;
  for (std::uint16_t i = 0; i < protocol.material_field_count && selected < 9;
       ++i) {
    auto const *material = protocol.material_field(i);
    auto const *descriptor =
        protocol.field_by_ordinal(material->admission_ordinal);
    if (descriptor->wire_type != 2)
      continue;
    append_header(nine_fields, descriptor->code);
    nine_fields.insert(nine_fields.end(), 4, 0);
    ++selected;
  }
  ASSERT_EQ(selected, 9);

  RecursiveScanOptions options;
  options.limits.max_fields = 8;
  TrackingAllocator field_memory;
  auto failed =
      guest_exact_object_index(slice(nine_fields), options, field_memory.api());
  EXPECT_EQ(failed.status.message_id,
            static_cast<std::uint16_t>(ScanMessage::too_many_fields));
  EXPECT_EQ(field_memory.calls, 0);

  auto eight_then_child = nine_fields;
  eight_then_child.resize(eight_then_child.size() - 5);
  eight_then_child.push_back(0xEA);
  options = {};
  options.limits.max_scopes = 1;
  TrackingAllocator scope_memory;
  failed = guest_exact_object_index(slice(eight_then_child), options,
                                    scope_memory.api());
  EXPECT_EQ(failed.status.message_id,
            static_cast<std::uint16_t>(ScanMessage::too_many_scopes));
  EXPECT_EQ(scope_memory.calls, 0);

  options = {};
  options.limits.max_depth = 0;
  TrackingAllocator depth_memory;
  auto const child = bytes({0xEA});
  failed = guest_exact_object_index(slice(child), options, depth_memory.api());
  EXPECT_EQ(failed.status.message_id,
            static_cast<std::uint16_t>(ScanMessage::nesting_too_deep));
  EXPECT_EQ(depth_memory.calls, 0);

  options = {};
  options.limits.max_bytes = 4;
  TrackingAllocator byte_memory;
  auto const one_field = bytes({0x22, 0, 0, 0, 1});
  failed =
      guest_exact_object_index(slice(one_field), options, byte_memory.api());
  EXPECT_EQ(failed.status.message_id,
            static_cast<std::uint16_t>(ScanMessage::input_too_large));
  EXPECT_EQ(byte_memory.calls, 0);
}

TEST(RecursiveIndex, FrozenDefaultCrossingsStayWithinScratchCeilings) {
  std::vector<std::uint8_t> oversized(1'048'577, 0);
  TrackingAllocator bytes_memory;
  auto failed =
      guest_exact_object_index(slice(oversized), {}, bytes_memory.api());
  EXPECT_EQ(failed.status.message_id,
            static_cast<std::uint16_t>(ScanMessage::input_too_large));
  EXPECT_EQ(bytes_memory.calls, 0);

  auto const too_many = maximum_topology_input(32'768);
  TrackingAllocator topology_memory;
  failed = guest_exact_object_index(slice(too_many), {}, topology_memory.api());
  EXPECT_EQ(failed.status.message_id,
            static_cast<std::uint16_t>(ScanMessage::too_many_fields));
  EXPECT_EQ(failed.index, nullptr);
  EXPECT_TRUE(topology_memory.live.empty());
  EXPECT_LE(topology_memory.peak_requested, 917'504 + 1'048'576);
  EXPECT_EQ(std::find(topology_memory.requests.begin(),
                      topology_memory.requests.end(), 1'179'680),
            topology_memory.requests.end());
}

TEST(RecursiveIndex, StructuralValidationRejectsUnreachableScope) {
  std::vector<std::uint32_t> impossible{1,
                                        0,
                                        2,
                                        1,
                                        0,
                                        5,
                                        0,
                                        1,
                                        1,
                                        1,
                                        1,
                                        0,
                                        0x00020002,
                                        0,
                                        1,
                                        5,
                                        FieldRecord::no_child};
  RecursiveIndexView const view{
      impossible.data(),
      static_cast<std::uint32_t>(impossible.size() * sizeof(std::uint32_t)), 5};
  EXPECT_FALSE(view.structurally_valid());
}

TEST(RecursiveIndex, FinalIndexOomPublishesNothingAndRetrySucceeds) {
  auto const input = bytes({0x22, 0, 0, 0, 1});
  TrackingAllocator memory;
  memory.fail_call = 1;
  auto failed = guest_exact_object_index(slice(input), {}, memory.api());
  EXPECT_FALSE(failed.ok());
  EXPECT_EQ(failed.index, nullptr);
  EXPECT_EQ(failed.status.issue,
            static_cast<std::uint16_t>(ScanIssue::out_of_memory));
  EXPECT_EQ(memory.frees, 0);

  memory.fail_call = 0;
  auto retry = guest_exact_object_index(slice(input), {}, memory.api());
  ASSERT_TRUE(retry.ok()) << scan_message_literal(retry.status.message_id);
  EXPECT_EQ(retry.index_size, 52);
  EXPECT_EQ(memory.live.size(), 1);
  EXPECT_EQ(memory.live_requested, retry.index_size);
  memory.api().free(memory.api().opaque, retry.index);
  EXPECT_TRUE(memory.live.empty());
}

TEST(RecursiveIndex, NineFieldsUseOneScratchAllocationAndOneExactIndex) {
  std::vector<std::uint8_t> input;
  auto const &protocol = xahau_static_protocol();
  std::uint32_t selected = 0;
  for (std::uint16_t i = 0; i < protocol.material_field_count && selected < 9;
       ++i) {
    auto const *material = protocol.material_field(i);
    auto const *descriptor =
        protocol.field_by_ordinal(material->admission_ordinal);
    if (descriptor->wire_type != 2)
      continue;
    append_header(input, descriptor->code);
    input.insert(input.end(), 4, 0);
    ++selected;
  }
  ASSERT_EQ(selected, 9);

  TrackingAllocator memory;
  auto result = guest_exact_object_index(slice(input), {}, memory.api());
  ASSERT_TRUE(result.ok()) << scan_message_literal(result.status.message_id);
  ASSERT_EQ(memory.requests.size(), 2);
  EXPECT_EQ(memory.requests[0], 16 * 28);
  EXPECT_EQ(memory.requests[1], 16 + 16 + 9 * 20);
  // Scratch is gone before success publication; only the exact index remains.
  EXPECT_EQ(memory.frees, 1);
  EXPECT_EQ(memory.live.size(), 1);
  memory.api().free(memory.api().opaque, result.index);
  EXPECT_TRUE(memory.live.empty());
}

TEST(RecursiveIndex, MaximumTopologyPinsScratchAndExactIndexSizes) {
  auto const input = maximum_topology_input();
  ASSERT_EQ(input.size(), 65'537);

  TrackingAllocator memory;
  auto result = guest_exact_object_index(slice(input), {}, memory.api());
  ASSERT_TRUE(result.ok()) << scan_message_literal(result.status.message_id);
  EXPECT_EQ(result.index_size, 1'179'680);
  RecursiveIndexView const index{result.index, result.index_size,
                                 result.consumed};
  EXPECT_EQ(index.header()->scope_count, 32'769);
  EXPECT_EQ(index.header()->field_count, 32'768);
  EXPECT_NE(std::find(memory.requests.begin(), memory.requests.end(), 917'504),
            memory.requests.end());
  EXPECT_NE(
      std::find(memory.requests.begin(), memory.requests.end(), 1'048'576),
      memory.requests.end());
  ASSERT_FALSE(memory.requests.empty());
  EXPECT_EQ(memory.requests.back(), 1'179'680);
  for (std::uint32_t capacity = 16; capacity <= 32'768; capacity *= 2) {
    EXPECT_EQ(std::count(memory.requests.begin(), memory.requests.end(),
                         std::size_t{capacity} * 28),
              1)
        << "field scratch capacity " << capacity;
  }
  for (std::uint32_t capacity = 8; capacity <= 65'536; capacity *= 2) {
    EXPECT_EQ(std::count(memory.requests.begin(), memory.requests.end(),
                         std::size_t{capacity} * 16),
              1)
        << "scope scratch capacity " << capacity;
  }
  EXPECT_EQ(memory.requests.size(), 27);
  EXPECT_EQ(memory.peak_requested, 917'504 + 1'048'576 + 1'179'680);
  ASSERT_EQ(memory.live.size(), 1);
  EXPECT_EQ(memory.live_requested, result.index_size);
  EXPECT_EQ(memory.frees, 2); // Both scratch allocations are already gone.
  memory.api().free(memory.api().opaque, result.index);
  EXPECT_TRUE(memory.live.empty());
}

TEST(RecursiveIndex, EveryMaximumTopologyAllocationCanFailAndRetry) {
  auto const input = maximum_topology_input();
  TrackingAllocator clean;
  auto clean_result = guest_exact_object_index(slice(input), {}, clean.api());
  ASSERT_TRUE(clean_result.ok());
  std::uint32_t const allocator_ordinals = clean.calls;
  clean.api().free(clean.api().opaque, clean_result.index);
  ASSERT_GT(allocator_ordinals, 3);

  for (std::uint32_t ordinal = 1; ordinal <= allocator_ordinals; ++ordinal) {
    TrackingAllocator injected;
    injected.fail_call = ordinal;
    auto failed = guest_exact_object_index(slice(input), {}, injected.api());
    ASSERT_FALSE(failed.ok()) << "allocator ordinal " << ordinal;
    EXPECT_EQ(failed.index, nullptr) << "allocator ordinal " << ordinal;
    EXPECT_EQ(failed.status.issue,
              static_cast<std::uint16_t>(ScanIssue::out_of_memory))
        << "allocator ordinal " << ordinal;
    EXPECT_TRUE(injected.live.empty()) << "allocator ordinal " << ordinal;

    injected.fail_call = 0;
    auto retry = guest_exact_object_index(slice(input), {}, injected.api());
    ASSERT_TRUE(retry.ok()) << "retry after allocator ordinal " << ordinal;
    EXPECT_EQ(retry.index_size, 1'179'680);
    injected.api().free(injected.api().opaque, retry.index);
    EXPECT_TRUE(injected.live.empty());
  }
}

} // namespace
