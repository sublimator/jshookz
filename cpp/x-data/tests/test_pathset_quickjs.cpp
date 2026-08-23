#include "pathset_quickjs_fixture.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

using catl::xdata::test::PathSetMarkEdge;
using catl::xdata::test::PathSetQuickJSFixture;

namespace {

class LocalValue {
public:
  LocalValue(JSContext *context, JSValue value = JS_UNDEFINED) noexcept
      : context_(context), value_(value) {}

  ~LocalValue() { JS_FreeValue(context_, value_); }

  LocalValue(LocalValue const &) = delete;
  LocalValue &operator=(LocalValue const &) = delete;

  LocalValue(LocalValue &&other) noexcept
      : context_(other.context_), value_(other.release()) {}

  LocalValue &operator=(LocalValue &&other) noexcept {
    if (this != &other) {
      reset();
      context_ = other.context_;
      value_ = other.release();
    }
    return *this;
  }

  JSValueConst get() const noexcept { return value_; }

  bool is_exception() const noexcept { return JS_IsException(value_); }

  JSValue release() noexcept {
    JSValue value = value_;
    value_ = JS_UNDEFINED;
    return value;
  }

  void reset(JSValue replacement = JS_UNDEFINED) noexcept {
    JS_FreeValue(context_, value_);
    value_ = replacement;
  }

private:
  JSContext *context_ = nullptr;
  JSValue value_ = JS_UNDEFINED;
};

void append_component(std::vector<std::uint8_t> &bytes, std::uint8_t first) {
  for (std::uint8_t i = 0; i < 20; ++i)
    bytes.push_back(static_cast<std::uint8_t>(first + i));
}

std::vector<std::uint8_t> worked_pathset_object() {
  // Paths field header: extended type 18, field ordinal 1.
  std::vector<std::uint8_t> bytes{0x01, 0x12};

  // Path zero, hop zero: account at payload offset 0.
  bytes.push_back(0x01);
  append_component(bytes, 0xa0);

  // Path zero, hop one: currency at payload offset 21.
  bytes.push_back(0x10);
  append_component(bytes, 0xc0);

  bytes.push_back(0xff);

  // Path one, hop zero: currency+issuer at payload offset 43.
  bytes.push_back(0x30);
  append_component(bytes, 0xd0);
  append_component(bytes, 0xe0);
  bytes.push_back(0x00);
  return bytes;
}

std::uint32_t to_u32(JSContext *context, JSValueConst value) {
  std::uint32_t result = 0;
  EXPECT_EQ(JS_ToUint32(context, &result, value), 0);
  return result;
}

bool typed_array_bytes(JSContext *context, JSValueConst value,
                       std::array<std::uint8_t, 20> &output) {
  if (JS_GetTypedArrayType(value) != JS_TYPED_ARRAY_UINT8)
    return false;
  std::size_t offset = 0;
  std::size_t length = 0;
  std::size_t bytes_per_element = 0;
  LocalValue buffer(context,
                    JS_GetTypedArrayBuffer(context, value, &offset, &length,
                                           &bytes_per_element));
  if (buffer.is_exception() || length != output.size() ||
      bytes_per_element != 1)
    return false;
  std::size_t buffer_size = 0;
  std::uint8_t *data = JS_GetArrayBuffer(context, &buffer_size, buffer.get());
  if (data == nullptr || offset > buffer_size || length > buffer_size - offset)
    return false;
  std::copy_n(data + offset, output.size(), output.begin());
  return true;
}

bool set_first_typed_array_byte(JSContext *context, JSValueConst value,
                                std::uint8_t byte) {
  std::size_t offset = 0;
  std::size_t length = 0;
  std::size_t bytes_per_element = 0;
  LocalValue buffer(context,
                    JS_GetTypedArrayBuffer(context, value, &offset, &length,
                                           &bytes_per_element));
  if (buffer.is_exception() || length == 0 || bytes_per_element != 1)
    return false;
  std::size_t buffer_size = 0;
  std::uint8_t *data = JS_GetArrayBuffer(context, &buffer_size, buffer.get());
  if (data == nullptr || offset >= buffer_size)
    return false;
  data[offset] = byte;
  return true;
}

void expect_ready(PathSetQuickJSFixture const &fixture) {
  ASSERT_TRUE(fixture.ready()) << fixture.init_error();
}

int own_property_status(JSContext *context, JSValueConst object,
                        char const *name) {
  JSAtom const atom = JS_NewAtom(context, name);
  if (atom == JS_ATOM_NULL)
    return -1;
  JSPropertyDescriptor descriptor{};
  int const status = JS_GetOwnProperty(context, &descriptor, object, atom);
  JS_FreeAtom(context, atom);
  if (status > 0) {
    JS_FreeValue(context, descriptor.value);
    JS_FreeValue(context, descriptor.getter);
    JS_FreeValue(context, descriptor.setter);
  }
  return status;
}

} // namespace

TEST(PathSetQuickJS, UsesExactV1RawContextAndPublishesNoClasses) {
  PathSetQuickJSFixture fixture;
  expect_ready(fixture);
  EXPECT_TRUE(fixture.v1_sandbox_builtins_absent());

  LocalValue global(fixture.context(), JS_GetGlobalObject(fixture.context()));
  ASSERT_FALSE(global.is_exception());
  for (char const *name : {"CertifiedObjectValue", "PrivateSTObjectFacade",
                           "PrivatePathSet", "PrivatePath", "PrivatePathHop"}) {
    LocalValue value(fixture.context(),
                     JS_GetPropertyStr(fixture.context(), global.get(), name));
    ASSERT_FALSE(value.is_exception()) << name;
    EXPECT_TRUE(JS_IsUndefined(value.get())) << name;
  }
}

TEST(PathSetQuickJS, UntouchedFacadeIsImmutableAndBuildsNothing) {
  PathSetQuickJSFixture fixture;
  expect_ready(fixture);
  auto object = worked_pathset_object();
  LocalValue facade(fixture.context(), fixture.make_facade(object));
  ASSERT_FALSE(facade.is_exception()) << fixture.take_exception_message();

  auto const &counters = fixture.counters();
  EXPECT_EQ(counters.owners_created, 1u);
  EXPECT_EQ(counters.facades_created, 1u);
  EXPECT_EQ(counters.pathsets_created, 0u);
  EXPECT_EQ(counters.paths_created, 0u);
  EXPECT_EQ(counters.pathhops_created, 0u);
  EXPECT_EQ(counters.directory_measure_calls, 0u);
  EXPECT_EQ(counters.directory_allocate_attempts, 0u);
  EXPECT_EQ(counters.directory_fill_calls, 0u);
  EXPECT_FALSE(fixture.facade_has_cached_pathset(facade.get()));
  EXPECT_EQ(own_property_status(fixture.context(), facade.get(), "Paths"), 0);
  EXPECT_EQ(JS_IsExtensible(fixture.context(), facade.get()), 0);

  EXPECT_LT(JS_SetPropertyStr(fixture.context(), facade.get(), "extra",
                              JS_NewInt32(fixture.context(), 1)),
            0);
  EXPECT_FALSE(fixture.take_exception_message().empty());
}

TEST(PathSetQuickJS, FirstPathsAccessBuildsOneExactDirectoryAndCachesIdentity) {
  PathSetQuickJSFixture fixture;
  expect_ready(fixture);
  auto object = worked_pathset_object();
  LocalValue facade(fixture.context(), fixture.make_facade(object));
  ASSERT_FALSE(facade.is_exception()) << fixture.take_exception_message();
  EXPECT_EQ(own_property_status(fixture.context(), facade.get(), "Paths"), 0);

  LocalValue first(fixture.context(), fixture.paths(facade.get()));
  ASSERT_FALSE(first.is_exception()) << fixture.take_exception_message();
  LocalValue second(fixture.context(), fixture.paths(facade.get()));
  ASSERT_FALSE(second.is_exception()) << fixture.take_exception_message();
  EXPECT_EQ(JS_StrictEq(fixture.context(), first.get(), second.get()), 1);
  EXPECT_TRUE(fixture.facade_has_cached_pathset(facade.get()));
  EXPECT_EQ(own_property_status(fixture.context(), facade.get(), "Paths"), 0);

  auto const &counters = fixture.counters();
  EXPECT_EQ(counters.directory_measure_calls, 1u);
  EXPECT_EQ(counters.directory_allocate_attempts, 1u);
  EXPECT_EQ(counters.directory_allocations, 1u);
  EXPECT_EQ(counters.directory_bytes_allocated, 24u);
  EXPECT_EQ(counters.directory_fill_calls, 1u);
  EXPECT_EQ(counters.directory_fill_successes, 1u);
  EXPECT_EQ(counters.pathsets_created, 1u);
  EXPECT_EQ(counters.pathset_cache_stores, 1u);
  EXPECT_EQ(counters.pathset_cache_hits, 1u);

  std::array<std::uint32_t, 6> directory{};
  std::size_t paths = 0;
  std::size_t hops = 0;
  ASSERT_TRUE(fixture.directory_snapshot(first.get(), directory, paths, hops));
  EXPECT_EQ(paths, 2u);
  EXPECT_EQ(hops, 3u);
  EXPECT_EQ(directory, (std::array<std::uint32_t, 6>{0, 2, 3, 0, 21, 43}));

  LocalValue length(fixture.context(), fixture.property(first.get(), "length"));
  ASSERT_FALSE(length.is_exception());
  EXPECT_EQ(to_u32(fixture.context(), length.get()), 2u);
  EXPECT_EQ(counters.directory_measure_calls, 1u);
  EXPECT_EQ(counters.directory_fill_calls, 1u);
}

TEST(PathSetQuickJS, RandomAccessMintsFreshLightweightChildren) {
  PathSetQuickJSFixture fixture;
  expect_ready(fixture);
  auto object = worked_pathset_object();
  LocalValue facade(fixture.context(), fixture.make_facade(object));
  LocalValue pathset(fixture.context(), fixture.paths(facade.get()));
  ASSERT_FALSE(pathset.is_exception()) << fixture.take_exception_message();

  LocalValue path_a(fixture.context(), fixture.at(pathset.get(), 0));
  LocalValue path_b(fixture.context(), fixture.at(pathset.get(), 0));
  ASSERT_FALSE(path_a.is_exception());
  ASSERT_FALSE(path_b.is_exception());
  EXPECT_EQ(JS_StrictEq(fixture.context(), path_a.get(), path_b.get()), 0);

  LocalValue length(fixture.context(),
                    fixture.property(path_a.get(), "length"));
  ASSERT_FALSE(length.is_exception());
  EXPECT_EQ(to_u32(fixture.context(), length.get()), 2u);

  LocalValue hop_a(fixture.context(), fixture.at(path_a.get(), 0));
  LocalValue hop_b(fixture.context(), fixture.at(path_a.get(), 0));
  ASSERT_FALSE(hop_a.is_exception());
  ASSERT_FALSE(hop_b.is_exception());
  EXPECT_EQ(JS_StrictEq(fixture.context(), hop_a.get(), hop_b.get()), 0);

  auto const &counters = fixture.counters();
  EXPECT_EQ(counters.paths_created, 2u);
  EXPECT_EQ(counters.pathhops_created, 2u);
  EXPECT_EQ(counters.directory_measure_calls, 1u);
  EXPECT_EQ(counters.directory_allocations, 1u);
  EXPECT_EQ(counters.directory_fill_calls, 1u);
}

TEST(PathSetQuickJS, HopCachesOwnedBytesOnceAndExportsFreshCopies) {
  PathSetQuickJSFixture fixture;
  expect_ready(fixture);
  auto object = worked_pathset_object();
  LocalValue facade(fixture.context(), fixture.make_facade(object));
  LocalValue pathset(fixture.context(), fixture.paths(facade.get()));
  LocalValue first_path(fixture.context(), fixture.at(pathset.get(), 0));
  LocalValue account_hop(fixture.context(), fixture.at(first_path.get(), 0));
  ASSERT_FALSE(account_hop.is_exception()) << fixture.take_exception_message();

  LocalValue first(fixture.context(),
                   fixture.property(account_hop.get(), "account"));
  LocalValue second(fixture.context(),
                    fixture.property(account_hop.get(), "account"));
  ASSERT_FALSE(first.is_exception());
  ASSERT_FALSE(second.is_exception());
  EXPECT_EQ(JS_StrictEq(fixture.context(), first.get(), second.get()), 0);
  EXPECT_EQ(fixture.counters().account_component_copies, 1u);

  std::array<std::uint8_t, 20> first_bytes{};
  std::array<std::uint8_t, 20> second_bytes{};
  ASSERT_TRUE(typed_array_bytes(fixture.context(), first.get(), first_bytes));
  ASSERT_TRUE(typed_array_bytes(fixture.context(), second.get(), second_bytes));
  EXPECT_EQ(first_bytes, second_bytes);
  EXPECT_EQ(first_bytes.front(), 0xa0);

  ASSERT_TRUE(set_first_typed_array_byte(fixture.context(), first.get(), 0x00));
  LocalValue third(fixture.context(),
                   fixture.property(account_hop.get(), "account"));
  std::array<std::uint8_t, 20> third_bytes{};
  ASSERT_TRUE(typed_array_bytes(fixture.context(), third.get(), third_bytes));
  EXPECT_EQ(third_bytes.front(), 0xa0);
  EXPECT_EQ(fixture.counters().account_component_copies, 1u);

  LocalValue missing_currency(fixture.context(),
                              fixture.property(account_hop.get(), "currency"));
  LocalValue missing_issuer(fixture.context(),
                            fixture.property(account_hop.get(), "issuer"));
  EXPECT_TRUE(JS_IsUndefined(missing_currency.get()));
  EXPECT_TRUE(JS_IsUndefined(missing_issuer.get()));

  LocalValue second_path(fixture.context(), fixture.at(pathset.get(), 1));
  LocalValue combined_hop(fixture.context(), fixture.at(second_path.get(), 0));
  LocalValue currency_a(fixture.context(),
                        fixture.property(combined_hop.get(), "currency"));
  LocalValue currency_b(fixture.context(),
                        fixture.property(combined_hop.get(), "currency"));
  LocalValue issuer_a(fixture.context(),
                      fixture.property(combined_hop.get(), "issuer"));
  LocalValue issuer_b(fixture.context(),
                      fixture.property(combined_hop.get(), "issuer"));
  ASSERT_FALSE(currency_a.is_exception());
  ASSERT_FALSE(currency_b.is_exception());
  ASSERT_FALSE(issuer_a.is_exception());
  ASSERT_FALSE(issuer_b.is_exception());
  EXPECT_EQ(JS_StrictEq(fixture.context(), currency_a.get(), currency_b.get()),
            0);
  EXPECT_EQ(JS_StrictEq(fixture.context(), issuer_a.get(), issuer_b.get()), 0);
  EXPECT_EQ(fixture.counters().currency_component_copies, 1u);
  EXPECT_EQ(fixture.counters().issuer_component_copies, 1u);
}

TEST(PathSetQuickJS, TargetedDirectoryOomIsUncachedAndRecoverable) {
  PathSetQuickJSFixture fixture;
  expect_ready(fixture);
  auto object = worked_pathset_object();
  LocalValue facade(fixture.context(), fixture.make_facade(object));
  ASSERT_FALSE(facade.is_exception()) << fixture.take_exception_message();

  fixture.fail_next_directory_allocation();
  LocalValue failed(fixture.context(), fixture.paths(facade.get()));
  ASSERT_TRUE(failed.is_exception());
  EXPECT_NE(fixture.take_exception_message().find("memory"), std::string::npos);
  EXPECT_FALSE(fixture.facade_has_cached_pathset(facade.get()));

  auto const &after_failure = fixture.counters();
  EXPECT_EQ(after_failure.directory_measure_calls, 1u);
  EXPECT_EQ(after_failure.directory_allocate_attempts, 1u);
  EXPECT_EQ(after_failure.forced_directory_ooms, 1u);
  EXPECT_EQ(after_failure.directory_allocations, 0u);
  EXPECT_EQ(after_failure.directory_fill_calls, 0u);
  EXPECT_EQ(after_failure.pathsets_created, 0u);
  EXPECT_EQ(after_failure.pathset_cache_stores, 0u);

  LocalValue recovered(fixture.context(), fixture.paths(facade.get()));
  ASSERT_FALSE(recovered.is_exception()) << fixture.take_exception_message();
  auto const &counters = fixture.counters();
  EXPECT_EQ(counters.directory_measure_calls, 2u);
  EXPECT_EQ(counters.directory_allocate_attempts, 2u);
  EXPECT_EQ(counters.directory_allocations, 1u);
  EXPECT_EQ(counters.directory_bytes_allocated, 24u);
  EXPECT_EQ(counters.directory_fill_calls, 1u);
  EXPECT_EQ(counters.directory_fill_successes, 1u);
  EXPECT_EQ(counters.pathset_cache_stores, 1u);
}

TEST(PathSetQuickJS, DescendantsMechanicallyRetainTheUniqueOwner) {
  PathSetQuickJSFixture fixture;
  expect_ready(fixture);
  auto object = worked_pathset_object();
  LocalValue facade(fixture.context(), fixture.make_facade(object));
  LocalValue pathset(fixture.context(), fixture.paths(facade.get()));
  LocalValue path(fixture.context(), fixture.at(pathset.get(), 0));
  LocalValue hop(fixture.context(), fixture.at(path.get(), 0));
  ASSERT_FALSE(hop.is_exception()) << fixture.take_exception_message();

  facade.reset();
  EXPECT_EQ(fixture.counters().facades_finalized, 1u);
  EXPECT_EQ(fixture.counters().owners_finalized, 0u);

  pathset.reset();
  path.reset();
  fixture.run_gc();
  EXPECT_EQ(fixture.counters().pathsets_finalized, 0u);
  EXPECT_EQ(fixture.counters().owners_finalized, 0u);

  LocalValue type(fixture.context(), fixture.property(hop.get(), "type"));
  ASSERT_FALSE(type.is_exception()) << fixture.take_exception_message();
  EXPECT_EQ(to_u32(fixture.context(), type.get()), 0x01u);

  hop.reset();
  fixture.run_gc();
  EXPECT_EQ(fixture.counters().owners_created, 1u);
  EXPECT_EQ(fixture.counters().owners_finalized, 1u);
  EXPECT_EQ(fixture.counters().pathsets_created, 1u);
  EXPECT_EQ(fixture.counters().pathsets_finalized, 1u);
  EXPECT_EQ(fixture.counters().paths_created, 1u);
  EXPECT_EQ(fixture.counters().paths_finalized, 1u);
  EXPECT_EQ(fixture.counters().pathhops_created, 1u);
  EXPECT_EQ(fixture.counters().pathhops_finalized, 1u);
  EXPECT_EQ(fixture.counters().directory_frees, 1u);
  EXPECT_EQ(fixture.counters().directory_bytes_freed, 24u);
}

TEST(PathSetQuickJS, EveryHiddenEdgeParticipatesInPlantedCycleCollection) {
  for (PathSetMarkEdge const edge :
       {PathSetMarkEdge::facade, PathSetMarkEdge::pathset,
        PathSetMarkEdge::path, PathSetMarkEdge::pathhop}) {
    PathSetQuickJSFixture fixture;
    expect_ready(fixture);
    auto object = worked_pathset_object();
    LocalValue facade(fixture.context(), fixture.make_facade(object));
    ASSERT_FALSE(facade.is_exception()) << fixture.take_exception_message();
    ASSERT_TRUE(fixture.plant_cycle(edge, facade.get()))
        << catl::xdata::test::pathset_mark_edge_name(edge) << ": "
        << fixture.take_exception_message();
    facade.reset();
    fixture.run_gc();
    EXPECT_TRUE(fixture.cycle_collected(edge))
        << catl::xdata::test::pathset_mark_edge_name(edge);
    EXPECT_EQ(fixture.counters().owners_finalized, 1u);
    EXPECT_EQ(fixture.counters().facades_finalized, 1u);
    EXPECT_EQ(fixture.counters().pathsets_finalized, 1u);
    EXPECT_EQ(fixture.counters().directory_allocations, 1u);
    EXPECT_EQ(fixture.counters().directory_frees, 1u);
  }
}
