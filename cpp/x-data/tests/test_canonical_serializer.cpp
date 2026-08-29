#include "catl/xdata/canonical_serializer.h"

#include "catl/xdata/canonical_writer.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <vector>

namespace catl::xdata {
namespace {

static_assert(sizeof(CanonicalWriter) ==
              sizeof(void *) + 2 * sizeof(std::uint32_t));

void *resize(void *, void *pointer, std::size_t size) noexcept {
  if (size == 0) {
    std::free(pointer);
    return nullptr;
  }
  return std::realloc(pointer, size);
}

void release(void *, void *pointer) noexcept { std::free(pointer); }

struct Indexed {
  std::vector<std::uint8_t> wire;
  void *index = nullptr;
  std::uint32_t index_size = 0;

  explicit Indexed(std::initializer_list<std::uint8_t> bytes) : wire(bytes) {}

  ~Indexed() { std::free(index); }

  Indexed(Indexed const &) = delete;
  Indexed &operator=(Indexed const &) = delete;
  Indexed(Indexed &&other) noexcept
      : wire(std::move(other.wire)), index(other.index),
        index_size(other.index_size) {
    other.index = nullptr;
  }

  [[nodiscard]] RecursiveIndexView view() const noexcept {
    return {index, index_size, static_cast<std::uint32_t>(wire.size())};
  }
};

Indexed indexed(std::initializer_list<std::uint8_t> bytes) {
  Indexed out(bytes);
  RecursiveScanOptions options{.protocol = &xahau_static_protocol()};
  auto result =
      guest_exact_object_index(Slice{out.wire.data(), out.wire.size()}, options,
                               ScanAllocator{nullptr, resize, release});
  EXPECT_TRUE(result.ok()) << scan_message_literal(result.status.message_id);
  out.index = result.index;
  out.index_size = result.index_size;
  return out;
}

std::vector<std::uint8_t> canonical(Indexed const &value) {
  auto const size = canonical_object_size(
      Slice{value.wire.data(), value.wire.size()}, value.view(), 0, true);
  EXPECT_TRUE(size.ok()) << scan_message_literal(size.status.message_id);
  std::vector<std::uint8_t> output(size.size);
  auto const written = canonical_object_write(
      Slice{value.wire.data(), value.wire.size()}, value.view(), 0, true,
      output.data(), static_cast<std::uint32_t>(output.size()));
  EXPECT_TRUE(written.ok()) << scan_message_literal(written.status.message_id);
  EXPECT_EQ(written.written, output.size());
  return output;
}

bool emit_direct_fixture(CanonicalWriter &writer) {
  std::uint8_t const payload[] = {0xaa, 0xbb, 0xcc};
  return writer.field_header(0x000f0009) && writer.field_header(0x000e000a) &&
         writer.field_header(0x00020001) && writer.be32(1) &&
         writer.field_header(0x00070001) && writer.vl_prefix(sizeof(payload)) &&
         writer.bytes(payload, sizeof(payload)) && writer.byte(0xe1) &&
         writer.field_header(0x000e000a) && writer.byte(0xe1) &&
         writer.byte(0xf1);
}

TEST(CanonicalSerializer, RestoresNestedClosersAndOmitsRootClose) {
  EXPECT_EQ(canonical(indexed({0xe1})), (std::vector<std::uint8_t>{}));
  EXPECT_EQ(canonical(indexed({0xea})),
            (std::vector<std::uint8_t>{0xea, 0xe1}));
  EXPECT_EQ(canonical(indexed({0xf9, 0xea})),
            (std::vector<std::uint8_t>{0xf9, 0xea, 0xe1, 0xf1}));
}

TEST(CanonicalSerializer, SortsObjectFieldsAndPreservesPayloads) {
  auto value = indexed({
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
  });
  EXPECT_EQ(canonical(value), (std::vector<std::uint8_t>{
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
                              }));
}

TEST(CanonicalSerializer, FieldValueExcludesHeaderAndVlPrefix) {
  auto value = indexed({0x71, 3, 0xaa, 0xbb, 0xcc});
  auto const *field = value.view().field(0);
  ASSERT_NE(field, nullptr);
  auto const size = canonical_field_value_size(
      Slice{value.wire.data(), value.wire.size()}, value.view(), *field);
  ASSERT_TRUE(size.ok());
  ASSERT_EQ(size.size, 3u);
  std::vector<std::uint8_t> output(size.size);
  auto const written = canonical_field_value_write(
      Slice{value.wire.data(), value.wire.size()}, value.view(), *field,
      output.data(), static_cast<std::uint32_t>(output.size()));
  ASSERT_TRUE(written.ok());
  EXPECT_EQ(output, (std::vector<std::uint8_t>{0xaa, 0xbb, 0xcc}));
}

TEST(CanonicalSerializer, NormalizesNumberWithoutRescanningFraming) {
  auto value = indexed({
      0x91,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      1,
      0,
      0,
      0,
      0,
  });
  EXPECT_EQ(canonical(value), (std::vector<std::uint8_t>{
                                  0x91,
                                  0x00,
                                  0x03,
                                  0x8d,
                                  0x7e,
                                  0xa4,
                                  0xc6,
                                  0x80,
                                  0x00,
                                  0xff,
                                  0xff,
                                  0xff,
                                  0xf1,
                              }));
}

TEST(CanonicalSerializer, RefusesShortOutputWithoutPartialSuccess) {
  auto value = indexed({0x21, 0, 0, 0, 1});
  std::uint8_t output[4] = {0xaa, 0xaa, 0xaa, 0xaa};
  auto const written =
      canonical_object_write(Slice{value.wire.data(), value.wire.size()},
                             value.view(), 0, true, output, sizeof(output));
  EXPECT_FALSE(written.ok());
  EXPECT_EQ(written.written, 0u);
  EXPECT_EQ(output[0], 0xaa);
}

TEST(CanonicalWriter, CountAndWriteUseTheSameDirectEmission) {
  auto counter = CanonicalWriter::measuring();
  ASSERT_TRUE(emit_direct_fixture(counter));

  std::vector<std::uint8_t> output(counter.position());
  CanonicalWriter writer(output.data(),
                         static_cast<std::uint32_t>(output.size()));
  ASSERT_TRUE(emit_direct_fixture(writer));
  EXPECT_EQ(writer.position(), counter.position());
  EXPECT_EQ(output, (std::vector<std::uint8_t>{
                        0xf9,
                        0xea,
                        0x21,
                        0,
                        0,
                        0,
                        1,
                        0x71,
                        3,
                        0xaa,
                        0xbb,
                        0xcc,
                        0xe1,
                        0xea,
                        0xe1,
                        0xf1,
                    }));
}

TEST(CanonicalWriter, RejectsInvalidFramingAndShortCapacity) {
  auto counter = CanonicalWriter::measuring();
  EXPECT_FALSE(counter.field_header(0));
  EXPECT_FALSE(counter.vl_prefix(918'745));
  EXPECT_EQ(counter.position(), 0u);

  std::uint8_t output[15]{};
  CanonicalWriter writer(output, sizeof(output));
  EXPECT_FALSE(emit_direct_fixture(writer));
  EXPECT_LE(writer.position(), sizeof(output));
}

} // namespace
} // namespace catl::xdata
