#include "catl/xdata/canonical_replacement.h"

#include "catl/xdata/static_protocol.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <utility>
#include <vector>

namespace catl::xdata {
namespace {

void *resize(void *, void *pointer, std::size_t size) noexcept {
  if (size == 0) {
    std::free(pointer);
    return nullptr;
  }
  return std::realloc(pointer, size);
}

void release(void *, void *pointer) noexcept { std::free(pointer); }

[[nodiscard]] Slice slice(std::vector<std::uint8_t> const &value) noexcept {
  return {value.data(), value.size()};
}

struct Indexed {
  std::vector<std::uint8_t> wire;
  void *index = nullptr;
  std::uint32_t index_size = 0;

  explicit Indexed(std::vector<std::uint8_t> bytes) : wire(std::move(bytes)) {
    RecursiveScanOptions options{.protocol = &xahau_static_protocol()};
    auto result = guest_exact_object_index(
        slice(wire), options, ScanAllocator{nullptr, resize, release});
    EXPECT_TRUE(result.ok()) << scan_message_literal(result.status.message_id);
    index = result.index;
    index_size = result.index_size;
  }

  ~Indexed() { std::free(index); }

  Indexed(Indexed const &) = delete;
  Indexed &operator=(Indexed const &) = delete;

  [[nodiscard]] RecursiveIndexView view() const noexcept {
    return {index, index_size, static_cast<std::uint32_t>(wire.size())};
  }
};

[[nodiscard]] std::vector<std::uint8_t>
with_field(Indexed const &source, std::uint32_t scope_id,
           std::uint32_t field_code, std::vector<std::uint8_t> const &payload) {
  auto const measured = canonical_object_with_field_size(
      slice(source.wire), source.view(), scope_id, field_code, slice(payload));
  EXPECT_TRUE(measured.ok())
      << scan_message_literal(measured.status.message_id);
  EXPECT_FALSE(measured.no_op());
  std::vector<std::uint8_t> output(measured.size);
  auto const written = canonical_object_with_field_write(
      slice(source.wire), source.view(), scope_id, field_code, slice(payload),
      output.data(), static_cast<std::uint32_t>(output.size()));
  EXPECT_TRUE(written.ok()) << scan_message_literal(written.status.message_id);
  EXPECT_FALSE(written.no_op());
  EXPECT_EQ(written.written, measured.size);
  return output;
}

[[nodiscard]] std::vector<std::uint8_t>
with_indexed_field(Indexed const &source, std::uint32_t scope_id,
                   std::uint32_t field_code, Indexed const &value,
                   std::uint32_t value_scope_id) {
  auto const measured = canonical_object_with_indexed_field_size(
      slice(source.wire), source.view(), scope_id, field_code,
      slice(value.wire), value.view(), value_scope_id);
  EXPECT_TRUE(measured.ok())
      << scan_message_literal(measured.status.message_id);
  EXPECT_FALSE(measured.no_op());
  std::vector<std::uint8_t> output(measured.size);
  auto const written = canonical_object_with_indexed_field_write(
      slice(source.wire), source.view(), scope_id, field_code,
      slice(value.wire), value.view(), value_scope_id, output.data(),
      static_cast<std::uint32_t>(output.size()));
  EXPECT_TRUE(written.ok()) << scan_message_literal(written.status.message_id);
  EXPECT_FALSE(written.no_op());
  EXPECT_EQ(written.written, measured.size);
  return output;
}

[[nodiscard]] std::vector<std::uint8_t>
without_field(Indexed const &source, std::uint32_t scope_id,
              std::uint32_t field_code) {
  auto const measured = canonical_object_without_field_size(
      slice(source.wire), source.view(), scope_id, field_code);
  EXPECT_TRUE(measured.ok())
      << scan_message_literal(measured.status.message_id);
  EXPECT_FALSE(measured.no_op());
  std::vector<std::uint8_t> output(measured.size);
  auto const written = canonical_object_without_field_write(
      slice(source.wire), source.view(), scope_id, field_code, output.data(),
      static_cast<std::uint32_t>(output.size()));
  EXPECT_TRUE(written.ok()) << scan_message_literal(written.status.message_id);
  EXPECT_FALSE(written.no_op());
  EXPECT_EQ(written.written, measured.size);
  return output;
}

[[nodiscard]] std::vector<std::uint8_t> uint32_payload(std::uint32_t value) {
  return {static_cast<std::uint8_t>(value >> 24),
          static_cast<std::uint8_t>(value >> 16),
          static_cast<std::uint8_t>(value >> 8),
          static_cast<std::uint8_t>(value)};
}

TEST(CanonicalReplacement, InsertsBeforeMiddleAndAfterInCanonicalOrder) {
  Indexed source({
      0x22,
      0,
      0,
      0,
      2,
      0x24,
      0,
      0,
      0,
      4,
  });

  EXPECT_EQ(with_field(source, 0, 0x00020001, uint32_payload(1)),
            (std::vector<std::uint8_t>{
                0x21,
                0,
                0,
                0,
                1,
                0x22,
                0,
                0,
                0,
                2,
                0x24,
                0,
                0,
                0,
                4,
            }));
  EXPECT_EQ(with_field(source, 0, 0x00020003, uint32_payload(3)),
            (std::vector<std::uint8_t>{
                0x22,
                0,
                0,
                0,
                2,
                0x23,
                0,
                0,
                0,
                3,
                0x24,
                0,
                0,
                0,
                4,
            }));
  EXPECT_EQ(with_field(source, 0, 0x00020005, uint32_payload(5)),
            (std::vector<std::uint8_t>{
                0x22,
                0,
                0,
                0,
                2,
                0x24,
                0,
                0,
                0,
                4,
                0x25,
                0,
                0,
                0,
                5,
            }));
}

TEST(CanonicalReplacement, ReplacesEvenWhenBytesAreIdentical) {
  Indexed source({0x21, 0, 0, 0, 1, 0x22, 0, 0, 0, 2});
  auto const original = source.wire;
  auto const output = with_field(source, 0, 0x00020002, uint32_payload(2));
  EXPECT_EQ(output, original);
  EXPECT_EQ(source.wire, original);

  auto const changed = with_field(source, 0, 0x00020002, uint32_payload(9));
  EXPECT_EQ(changed, (std::vector<std::uint8_t>{
                         0x21,
                         0,
                         0,
                         0,
                         1,
                         0x22,
                         0,
                         0,
                         0,
                         9,
                     }));
  EXPECT_EQ(source.wire, original);
}

TEST(CanonicalReplacement, RemovesPresentAndReportsAbsentNoOpWithoutOutput) {
  Indexed source({0x21, 0, 0, 0, 1, 0x22, 0, 0, 0, 2});
  auto const original = source.wire;
  EXPECT_EQ(without_field(source, 0, 0x00020001),
            (std::vector<std::uint8_t>{0x22, 0, 0, 0, 2}));
  EXPECT_EQ(source.wire, original);

  auto const measured = canonical_object_without_field_size(
      slice(source.wire), source.view(), 0, 0x00020003);
  ASSERT_TRUE(measured.ok());
  EXPECT_TRUE(measured.no_op());
  EXPECT_EQ(measured.size, 0u);
  std::array<std::uint8_t, 8> poison{};
  poison.fill(0xa5);
  auto const written = canonical_object_without_field_write(
      slice(source.wire), source.view(), 0, 0x00020003, poison.data(), 0);
  ASSERT_TRUE(written.ok());
  EXPECT_TRUE(written.no_op());
  EXPECT_EQ(written.written, 0u);
  EXPECT_TRUE(std::all_of(poison.begin(), poison.end(),
                          [](auto byte) { return byte == 0xa5; }));
}

TEST(CanonicalReplacement, EncodesEveryVlPrefixBoundaryExactly) {
  Indexed empty({});
  struct Case {
    std::uint32_t size;
    std::vector<std::uint8_t> prefix;
  };
  std::array<Case, 5> const cases{{
      {192, {192}},
      {193, {193, 0}},
      {12'480, {240, 255}},
      {12'481, {241, 0, 0}},
      {918'744, {254, 212, 23}},
  }};
  for (auto const &test : cases) {
    SCOPED_TRACE(test.size);
    std::vector<std::uint8_t> payload(test.size, 0x5a);
    auto const output = with_field(empty, 0, 0x00070001, payload);
    ASSERT_EQ(output[0], 0x71);
    ASSERT_GE(output.size(), 1u + test.prefix.size());
    EXPECT_TRUE(
        std::equal(test.prefix.begin(), test.prefix.end(), output.begin() + 1));
    EXPECT_TRUE(std::equal(payload.begin(), payload.end(),
                           output.begin() + 1 + test.prefix.size()));
  }
}

TEST(CanonicalReplacement, EncodesExtendedNthAndTypeHeaders) {
  Indexed empty({});
  EXPECT_EQ(with_field(empty, 0, 0x00020010, uint32_payload(7)),
            (std::vector<std::uint8_t>{0x20, 0x10, 0, 0, 0, 7}));
  EXPECT_EQ(with_field(empty, 0, 0x00100001, {0x7f}),
            (std::vector<std::uint8_t>{0x01, 0x10, 0x7f}));
}

TEST(CanonicalReplacement, InsertsAndPreservesNestedCanonicalValues) {
  Indexed source({
      0xea,
      0x22,
      0,
      0,
      0,
      2,
      0x21,
      0,
      0,
      0,
      1,
      0xe1,
  });
  auto const output = with_field(source, 0, 0x00020001, uint32_payload(9));
  EXPECT_EQ(output, (std::vector<std::uint8_t>{
                        0x21,
                        0,
                        0,
                        0,
                        9,
                        0xea,
                        0x21,
                        0,
                        0,
                        0,
                        1,
                        0x22,
                        0,
                        0,
                        0,
                        2,
                        0xe1,
                    }));

  Indexed empty({});
  std::vector<std::uint8_t> const nested_object{0x21, 0, 0, 0, 3, 0xe1};
  EXPECT_EQ(with_field(empty, 0, 0x000e0002, nested_object),
            (std::vector<std::uint8_t>{
                0xe2,
                0x21,
                0,
                0,
                0,
                3,
                0xe1,
            }));
  std::vector<std::uint8_t> const nested_array{0xea, 0xe1, 0xf1};
  EXPECT_EQ(with_field(empty, 0, 0x000f0003, nested_array),
            (std::vector<std::uint8_t>{0xf3, 0xea, 0xe1, 0xf1}));
}

TEST(CanonicalReplacement, StreamsCertifiedContainersWithoutTemporaryPayload) {
  Indexed empty({});
  Indexed object_value({
      0x24,
      0,
      0,
      0,
      4,
      0x99,
      0x22,
      0,
      0,
      0,
      2,
  });
  EXPECT_EQ(with_indexed_field(empty, 0, 0x000e0002, object_value, 0),
            (std::vector<std::uint8_t>{
                0xe2,
                0x22,
                0,
                0,
                0,
                2,
                0x24,
                0,
                0,
                0,
                4,
                0xe1,
            }));

  Indexed array_value({
      0xf3,
      0xea,
      0x24,
      0,
      0,
      0,
      4,
      0x22,
      0,
      0,
      0,
      2,
      0xe1,
      0xf1,
  });
  EXPECT_EQ(with_indexed_field(empty, 0, 0x000f0003, array_value, 1),
            (std::vector<std::uint8_t>{
                0xf3,
                0xea,
                0x22,
                0,
                0,
                0,
                2,
                0x24,
                0,
                0,
                0,
                4,
                0xe1,
                0xf1,
            }));

  auto const wrong_kind = canonical_object_with_indexed_field_size(
      slice(empty.wire), empty.view(), 0, 0x000f0003, slice(object_value.wire),
      object_value.view(), 0);
  EXPECT_FALSE(wrong_kind.ok());
  EXPECT_EQ(wrong_kind.status.issue,
            static_cast<std::uint16_t>(ScanIssue::malformed_data));
  EXPECT_EQ(wrong_kind.status.message_id,
            static_cast<std::uint16_t>(ScanMessage::noncanonical_payload));
}

TEST(CanonicalReplacement, CanonicalizesUnsortedInputAndUntouchedNumber) {
  Indexed source({
      0x91, 0,    0, 0, 0, 0, 0,    0, 1, 0, 0, 0,
      0,    0x22, 0, 0, 0, 2, 0x21, 0, 0, 0, 1,
  });
  auto const output = with_field(source, 0, 0x00020003, uint32_payload(3));
  EXPECT_EQ(output,
            (std::vector<std::uint8_t>{
                0x21, 0,    0,    0,    1,    0x22, 0,    0,    0,    2,
                0x23, 0,    0,    0,    3,    0x91, 0x00, 0x03, 0x8d, 0x7e,
                0xa4, 0xc6, 0x80, 0x00, 0xff, 0xff, 0xff, 0xf1,
            }));
}

TEST(CanonicalReplacement, RejectsShortOutputBeforeMutation) {
  Indexed source({0x21, 0, 0, 0, 1});
  auto const payload = uint32_payload(2);
  auto const measured = canonical_object_with_field_size(
      slice(source.wire), source.view(), 0, 0x00020002, slice(payload));
  ASSERT_TRUE(measured.ok());
  std::array<std::uint8_t, 16> output{};
  output.fill(0xa5);
  auto const written = canonical_object_with_field_write(
      slice(source.wire), source.view(), 0, 0x00020002, slice(payload),
      output.data(), measured.size - 1);
  EXPECT_FALSE(written.ok());
  EXPECT_EQ(written.status.issue,
            static_cast<std::uint16_t>(ScanIssue::internal_error));
  EXPECT_EQ(written.status.message_id,
            static_cast<std::uint16_t>(ScanMessage::index_size_overflow));
  EXPECT_EQ(written.status.aux, measured.size);
  EXPECT_EQ(written.written, 0u);
  EXPECT_TRUE(std::all_of(output.begin(), output.end(),
                          [](auto byte) { return byte == 0xa5; }));
}

TEST(CanonicalReplacement, RejectsUnknownNonmaterialAndInvalidPayloads) {
  Indexed empty({});
  std::vector<std::uint8_t> const four(4);
  auto unknown = canonical_object_with_field_size(
      slice(empty.wire), empty.view(), 0, 0x00ff00ff, slice(four));
  EXPECT_FALSE(unknown.ok());
  EXPECT_EQ(unknown.status.issue,
            static_cast<std::uint16_t>(ScanIssue::malformed_data));
  EXPECT_EQ(unknown.status.message_id,
            static_cast<std::uint16_t>(ScanMessage::unknown_field));

  auto terminator = canonical_object_without_field_size(
      slice(empty.wire), empty.view(), 0, 0x000e0001);
  EXPECT_FALSE(terminator.ok());
  EXPECT_EQ(terminator.status.message_id,
            static_cast<std::uint16_t>(ScanMessage::unknown_field));

  std::vector<std::uint8_t> const short_fixed(3);
  auto fixed = canonical_object_with_field_size(
      slice(empty.wire), empty.view(), 0, 0x00020001, slice(short_fixed));
  EXPECT_FALSE(fixed.ok());
  EXPECT_EQ(fixed.status.message_id,
            static_cast<std::uint16_t>(ScanMessage::truncated_field));
  EXPECT_EQ(fixed.status.aux, 4u);

  std::uint8_t byte = 0;
  auto vl = canonical_object_with_field_size(slice(empty.wire), empty.view(), 0,
                                             0x00070001, Slice{&byte, 918'745});
  EXPECT_FALSE(vl.ok());
  EXPECT_EQ(vl.status.message_id,
            static_cast<std::uint16_t>(ScanMessage::invalid_vl));
  EXPECT_EQ(vl.status.aux, 918'745u);

  auto too_large = canonical_object_with_field_size(
      slice(empty.wire), empty.view(), 0, 0x00060001,
      Slice{&byte, std::numeric_limits<std::uint32_t>::max()});
  EXPECT_FALSE(too_large.ok());
  EXPECT_EQ(too_large.status.message_id,
            static_cast<std::uint16_t>(ScanMessage::index_size_overflow));
}

TEST(CanonicalReplacement, RejectsNonObjectScopeAndInvalidIndex) {
  Indexed source({0xf9, 0xea, 0xe1, 0xf1});
  auto const payload = uint32_payload(1);
  auto array = canonical_object_with_field_size(
      slice(source.wire), source.view(), 1, 0x00020001, slice(payload));
  EXPECT_FALSE(array.ok());
  EXPECT_EQ(array.status.issue,
            static_cast<std::uint16_t>(ScanIssue::internal_error));
  EXPECT_EQ(array.status.message_id,
            static_cast<std::uint16_t>(ScanMessage::invalid_index));

  auto *header = static_cast<IndexHeader *>(source.index);
  auto const original_count = header->field_count;
  ++header->field_count;
  auto invalid = canonical_object_with_field_size(
      slice(source.wire), source.view(), 0, 0x00020001, slice(payload));
  header->field_count = original_count;
  EXPECT_FALSE(invalid.ok());
  EXPECT_EQ(invalid.status.message_id,
            static_cast<std::uint16_t>(ScanMessage::invalid_index));
}

TEST(CanonicalReplacement, PresentRemovalOfOnlyFieldEmitsEmptyRoot) {
  Indexed source({0x21, 0, 0, 0, 1});
  auto const measured = canonical_object_without_field_size(
      slice(source.wire), source.view(), 0, 0x00020001);
  ASSERT_TRUE(measured.ok());
  EXPECT_FALSE(measured.no_op());
  EXPECT_EQ(measured.size, 0u);
  auto const written = canonical_object_without_field_write(
      slice(source.wire), source.view(), 0, 0x00020001, nullptr, 0);
  ASSERT_TRUE(written.ok());
  EXPECT_FALSE(written.no_op());
  EXPECT_EQ(written.written, 0u);
}

} // namespace
} // namespace catl::xdata
