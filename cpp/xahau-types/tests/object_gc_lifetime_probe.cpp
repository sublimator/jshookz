#include "object/field_js.hpp"
#include "object/object.hpp"
#include "object_gc_lifetime_probe_hooks.hpp"

#include <quickjs.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace types = jshookz::provider::types;
namespace probe = jshookz::provider::types::test;

namespace {

constexpr std::size_t entityCount =
    static_cast<std::size_t>(probe::TrackedEntity::count);

class LocalValue {
public:
  LocalValue() = default;
  LocalValue(JSContext *ctx, JSValue value) : ctx_(ctx), value_(value) {}
  LocalValue(LocalValue const &) = delete;
  LocalValue &operator=(LocalValue const &) = delete;
  LocalValue(LocalValue &&other) noexcept
      : ctx_(other.ctx_), value_(other.value_) {
    other.ctx_ = nullptr;
    other.value_ = JS_UNDEFINED;
  }
  LocalValue &operator=(LocalValue &&other) noexcept {
    if (this != &other) {
      reset();
      ctx_ = other.ctx_;
      value_ = other.value_;
      other.ctx_ = nullptr;
      other.value_ = JS_UNDEFINED;
    }
    return *this;
  }
  ~LocalValue() { reset(); }

  [[nodiscard]] JSValueConst get() const noexcept { return value_; }
  [[nodiscard]] bool isException() const noexcept {
    return JS_IsException(value_);
  }
  void reset() noexcept {
    if (ctx_ != nullptr)
      JS_FreeValue(ctx_, value_);
    ctx_ = nullptr;
    value_ = JS_UNDEFINED;
  }

private:
  JSContext *ctx_ = nullptr;
  JSValue value_ = JS_UNDEFINED;
};

class PendingTarget {
public:
  explicit PendingTarget(JSValueConst target) {
    probe::gcProbeSetPendingTarget(target);
  }
  PendingTarget(PendingTarget const &) = delete;
  PendingTarget &operator=(PendingTarget const &) = delete;
  ~PendingTarget() { probe::gcProbeClearPendingTarget(); }
};

class RuntimeFixture {
public:
  RuntimeFixture() {
    runtime_ = JS_NewRuntime();
    if (runtime_ == nullptr) {
      error_ = "JS_NewRuntime failed";
      return;
    }
    context_ = JS_NewContext(runtime_);
    if (context_ == nullptr) {
      error_ = "JS_NewContext failed";
      return;
    }
    if (!types::registerObjectTypes(context_))
      error_ = takeException("registerObjectTypes failed");
  }

  RuntimeFixture(RuntimeFixture const &) = delete;
  RuntimeFixture &operator=(RuntimeFixture const &) = delete;

  ~RuntimeFixture() {
    if (context_ != nullptr)
      JS_FreeContext(context_);
    if (runtime_ != nullptr) {
      types::unregisterObjectTypes(runtime_);
      JS_FreeRuntime(runtime_);
    }
  }

  [[nodiscard]] bool ready() const noexcept { return error_.empty(); }
  [[nodiscard]] JSContext *context() const noexcept { return context_; }
  [[nodiscard]] JSRuntime *runtime() const noexcept { return runtime_; }
  [[nodiscard]] std::string const &error() const noexcept { return error_; }

  [[nodiscard]] std::string takeException(char const *fallback) {
    if (context_ == nullptr || !JS_HasException(context_))
      return fallback;
    LocalValue exception(context_, JS_GetException(context_));
    LocalValue string(context_, JS_ToString(context_, exception.get()));
    if (string.isException())
      return fallback;
    std::size_t length = 0;
    char const *characters = JS_ToCStringLen(context_, &length, string.get());
    if (characters == nullptr)
      return fallback;
    std::string message(characters, length);
    JS_FreeCString(context_, characters);
    return message;
  }

private:
  JSRuntime *runtime_ = nullptr;
  JSContext *context_ = nullptr;
  std::string error_;
};

using ExpectedCounts = std::array<std::uint32_t, entityCount>;

[[nodiscard]] std::size_t entityIndex(probe::TrackedEntity entity) noexcept {
  return static_cast<std::size_t>(entity);
}

[[nodiscard]] char const *entityName(probe::TrackedEntity entity) noexcept {
  switch (entity) {
  case probe::TrackedEntity::owner:
    return "owner";
  case probe::TrackedEntity::object:
    return "object";
  case probe::TrackedEntity::array:
    return "array";
  case probe::TrackedEntity::iterator:
    return "iterator";
  case probe::TrackedEntity::fieldTable:
    return "field-table";
  case probe::TrackedEntity::fieldDescriptor:
    return "field-descriptor";
  case probe::TrackedEntity::count:
    return "count";
  }
  return "unknown";
}

[[nodiscard]] bool checkCreated(ExpectedCounts const &expected) {
  for (std::size_t i = 0; i < expected.size(); ++i) {
    auto const entity = static_cast<probe::TrackedEntity>(i);
    auto const observed = probe::gcProbeCounts(entity);
    if (observed.created != expected[i] ||
        observed.finalized > observed.created) {
      std::fprintf(stderr,
                   "%s count mismatch: created=%u finalized=%u expected=%u\n",
                   entityName(entity), observed.created, observed.finalized,
                   expected[i]);
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool checkCounts(probe::TrackedEntity entity,
                               std::uint32_t created, std::uint32_t finalized) {
  auto const observed = probe::gcProbeCounts(entity);
  if (observed.created == created && observed.finalized == finalized)
    return true;
  std::fprintf(stderr,
               "%s count mismatch: created=%u finalized=%u expected=%u/%u\n",
               entityName(entity), observed.created, observed.finalized,
               created, finalized);
  return false;
}

[[nodiscard]] LocalValue mint(JSContext *ctx, std::uint8_t const *bytes,
                              std::uint32_t size) {
  return LocalValue(ctx, types::makeCertifiedObjectCopy(ctx, bytes, size));
}

[[nodiscard]] LocalValue iteratorFor(JSContext *ctx, JSValueConst arrayValue) {
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
  LocalValue method(ctx, JS_GetProperty(ctx, arrayValue, atom));
  JS_FreeAtom(ctx, atom);
  if (method.isException())
    return LocalValue(ctx, JS_EXCEPTION);
  return LocalValue(ctx, JS_Call(ctx, method.get(), arrayValue, 0, nullptr));
}

[[nodiscard]] bool callToBytes(JSContext *ctx, JSValueConst object) {
  LocalValue method(ctx, JS_GetPropertyStr(ctx, object, "toBytes"));
  if (method.isException())
    return false;
  LocalValue bytes(ctx, JS_Call(ctx, method.get(), object, 0, nullptr));
  return !bytes.isException() && JS_IsObject(bytes.get());
}

[[nodiscard]] ExpectedCounts expectedFor(probe::HiddenEdge edge) {
  ExpectedCounts expected{};
  auto set = [&expecteded = expected](probe::TrackedEntity entity,
                                      std::uint32_t count) {
    expecteded[entityIndex(entity)] = count;
  };
  switch (edge) {
  case probe::HiddenEdge::objectOwner:
    set(probe::TrackedEntity::owner, 1);
    set(probe::TrackedEntity::object, 1);
    break;
  case probe::HiddenEdge::objectCacheValue:
    set(probe::TrackedEntity::owner, 1);
    set(probe::TrackedEntity::object, 2);
    break;
  case probe::HiddenEdge::arrayOwner:
    set(probe::TrackedEntity::owner, 1);
    set(probe::TrackedEntity::object, 1);
    set(probe::TrackedEntity::array, 1);
    break;
  case probe::HiddenEdge::arrayCacheValue:
    set(probe::TrackedEntity::owner, 1);
    set(probe::TrackedEntity::object, 2);
    set(probe::TrackedEntity::array, 1);
    break;
  case probe::HiddenEdge::iteratorArray:
    set(probe::TrackedEntity::owner, 1);
    set(probe::TrackedEntity::object, 1);
    set(probe::TrackedEntity::array, 1);
    set(probe::TrackedEntity::iterator, 1);
    break;
  case probe::HiddenEdge::fieldTableDescriptor:
    set(probe::TrackedEntity::fieldTable, 1);
    set(probe::TrackedEntity::fieldDescriptor, 1);
    break;
  case probe::HiddenEdge::none:
    break;
  }
  return expected;
}

[[nodiscard]] bool prepareCycle(RuntimeFixture &fixture,
                                probe::HiddenEdge edge) {
  auto *ctx = fixture.context();
  static constexpr std::uint8_t objectBytes[] = {0x22, 0x00, 0x00, 0x00, 0x01};
  static constexpr std::uint8_t nestedObjectBytes[] = {0xea, 0xe1};
  static constexpr std::uint8_t arrayBytes[] = {0xf9, 0xea, 0xe1, 0xf1};

  if (edge == probe::HiddenEdge::fieldTableDescriptor) {
    LocalValue global(ctx, JS_GetGlobalObject(ctx));
    if (global.isException() ||
        !types::registerFieldDescriptors(ctx, global.get()))
      return false;
    LocalValue table(ctx, JS_GetPropertyStr(ctx, global.get(), "Field"));
    if (table.isException() || !JS_IsObject(table.get()))
      return false;
    LocalValue descriptor;
    {
      PendingTarget pending(table.get());
      descriptor =
          LocalValue(ctx, JS_GetPropertyStr(ctx, table.get(), "Flags"));
    }
    if (descriptor.isException() || !JS_IsObject(descriptor.get()))
      return false;
    if (JS_SetPropertyStr(ctx, global.get(), "Field", JS_UNDEFINED) < 0)
      return false;
    return true;
  }

  auto const *bytes = edge == probe::HiddenEdge::objectOwner ? objectBytes
                      : edge == probe::HiddenEdge::objectCacheValue
                          ? nestedObjectBytes
                          : arrayBytes;
  auto const size = edge == probe::HiddenEdge::objectOwner ? sizeof(objectBytes)
                    : edge == probe::HiddenEdge::objectCacheValue
                        ? sizeof(nestedObjectBytes)
                        : sizeof(arrayBytes);
  LocalValue root = mint(ctx, bytes, static_cast<std::uint32_t>(size));
  if (root.isException())
    return false;
  if (edge == probe::HiddenEdge::objectOwner)
    return true;

  if (edge == probe::HiddenEdge::objectCacheValue) {
    LocalValue child;
    {
      PendingTarget pending(root.get());
      child = LocalValue(ctx, JS_GetPropertyStr(ctx, root.get(), "Memo"));
    }
    return !child.isException() && types::isSTObject(child.get());
  }

  LocalValue array(ctx, JS_GetPropertyStr(ctx, root.get(), "Memos"));
  if (array.isException() || !types::isSTArray(array.get()))
    return false;
  if (edge == probe::HiddenEdge::arrayOwner)
    return true;
  if (edge == probe::HiddenEdge::arrayCacheValue) {
    LocalValue child;
    {
      PendingTarget pending(array.get());
      child = LocalValue(ctx, JS_GetPropertyUint32(ctx, array.get(), 0));
    }
    return !child.isException() && types::isSTObject(child.get());
  }
  LocalValue iterator = iteratorFor(ctx, array.get());
  return !iterator.isException() && JS_IsObject(iterator.get());
}

[[nodiscard]] int runEdge(probe::HiddenEdge edge, bool disabled) {
  probe::configureGCProbe(edge, disabled);
  RuntimeFixture fixture;
  if (!fixture.ready()) {
    std::fprintf(stderr, "fixture init failed: %s\n", fixture.error().c_str());
    return 70;
  }
  if (!prepareCycle(fixture, edge)) {
    std::fprintf(stderr, "cycle preparation failed for %s: %s\n",
                 probe::hiddenEdgeName(edge),
                 fixture.takeException("no pending exception").c_str());
    return 70;
  }

  JS_RunGC(fixture.runtime());
  auto const expected = expectedFor(edge);
  if (!checkCreated(expected))
    return 71;

  if (!disabled) {
    if (!probe::gcProbeSourceFinalized() || !probe::gcProbeAllFinalized()) {
      std::fprintf(stderr, "enabled edge retained: %s\n",
                   probe::hiddenEdgeName(edge));
      return 71;
    }
    std::printf("collected %s\n", probe::hiddenEdgeName(edge));
    return 0;
  }

  if (probe::gcProbeSourceFinalized() || probe::gcProbeAllFinalized()) {
    std::fprintf(stderr, "disabled edge did not retain its source: %s\n",
                 probe::hiddenEdgeName(edge));
    return 72;
  }
  std::printf("retained %s\n", probe::hiddenEdgeName(edge));

  probe::enableAllGCProbeMarks();
  JS_RunGC(fixture.runtime());
  if (!probe::gcProbeSourceFinalized() || !probe::gcProbeAllFinalized()) {
    std::fprintf(stderr, "red-cycle cleanup failed: %s\n",
                 probe::hiddenEdgeName(edge));
    return 73;
  }
  return 23;
}

[[nodiscard]] bool runParentBeforeChild(RuntimeFixture &fixture) {
  auto *ctx = fixture.context();
  static constexpr std::uint8_t bytes[] = {0xea, 0x22, 0x00, 0x00,
                                           0x00, 0x07, 0xe1};
  LocalValue root = mint(ctx, bytes, sizeof(bytes));
  LocalValue child(ctx, JS_GetPropertyStr(ctx, root.get(), "Memo"));
  if (root.isException() || child.isException() ||
      !types::isSTObject(child.get()))
    return false;
  root.reset();
  JS_RunGC(fixture.runtime());
  if (!checkCounts(probe::TrackedEntity::owner, 1, 0) ||
      !checkCounts(probe::TrackedEntity::object, 2, 1) ||
      !callToBytes(ctx, child.get()))
    return false;
  child.reset();
  JS_RunGC(fixture.runtime());
  return probe::gcProbeAllFinalized();
}

[[nodiscard]] bool runIteratorBeforeElement(RuntimeFixture &fixture) {
  auto *ctx = fixture.context();
  static constexpr std::uint8_t bytes[] = {0xf9, 0xea, 0xe1, 0xf1};
  LocalValue root = mint(ctx, bytes, sizeof(bytes));
  LocalValue array(ctx, JS_GetPropertyStr(ctx, root.get(), "Memos"));
  LocalValue iterator = iteratorFor(ctx, array.get());
  if (root.isException() || array.isException() || iterator.isException())
    return false;

  root.reset();
  array.reset();
  JS_RunGC(fixture.runtime());
  if (!checkCounts(probe::TrackedEntity::owner, 1, 0) ||
      !checkCounts(probe::TrackedEntity::object, 1, 1) ||
      !checkCounts(probe::TrackedEntity::array, 1, 0) ||
      !checkCounts(probe::TrackedEntity::iterator, 1, 0))
    return false;

  LocalValue next(ctx, JS_GetPropertyStr(ctx, iterator.get(), "next"));
  LocalValue result(ctx, JS_Call(ctx, next.get(), iterator.get(), 0, nullptr));
  LocalValue element(ctx, JS_GetPropertyStr(ctx, result.get(), "value"));
  if (next.isException() || result.isException() || element.isException() ||
      !types::isSTObject(element.get()))
    return false;
  result.reset();
  iterator.reset();
  JS_RunGC(fixture.runtime());
  if (!checkCounts(probe::TrackedEntity::owner, 1, 0) ||
      !checkCounts(probe::TrackedEntity::object, 2, 1) ||
      !checkCounts(probe::TrackedEntity::array, 1, 1) ||
      !checkCounts(probe::TrackedEntity::iterator, 1, 1) ||
      !callToBytes(ctx, element.get()))
    return false;
  element.reset();
  next.reset();
  JS_RunGC(fixture.runtime());
  return probe::gcProbeAllFinalized();
}

enum class RadixBankKind : std::uint8_t {
  onePage,
  pagesUnderOneBranch,
  pageUnderEveryBranch,
  valueInEveryPage,
  maximumSequential,
};

enum class RadixBankFailure : std::uint8_t {
  none,
  setup,
  access,
  identity,
  hitAllocation,
  metrics,
  iterator,
};

struct RadixBank {
  char const *name = nullptr;
  RadixBankKind kind = RadixBankKind::onePage;
  std::uint32_t length = 0;
  std::uint32_t expectedBranches = 0;
  std::uint32_t expectedPages = 0;
  std::uint32_t expectedValues = 0;
  std::size_t expectedBytes = 0;
  std::size_t expectedAllocations = 0;
};

constexpr std::size_t nativeRootBytes = 272;
constexpr std::size_t nativeBranchBytes = 256;
constexpr std::size_t nativeLeafBytes = 512;
constexpr std::uint32_t maximumArrayLength = 32'767;
constexpr std::uint32_t maximumFixtureNopsPerObject = 30;
static_assert(maximumFixtureNopsPerObject < 64);
static_assert(maximumFixtureNopsPerObject + 1 +
                  maximumArrayLength * (1 + maximumFixtureNopsPerObject + 1) +
                  1 ==
              1'048'576);
static_assert(maximumArrayLength + 1 == 32'768);
static_assert(maximumArrayLength + 2 == 32'769);

constexpr std::array radixBanks = {
    RadixBank{"one-page", RadixBankKind::onePage, 32, 1, 1, 32, 1'040, 3},
    RadixBank{"one-branch", RadixBankKind::pagesUnderOneBranch, 1'024, 1, 32,
              1'024, 16'912, 34},
    RadixBank{"all-branches", RadixBankKind::pageUnderEveryBranch, 31'776, 32,
              32, 1'024, 24'848, 65},
    RadixBank{"all-pages", RadixBankKind::valueInEveryPage, 32'737, 32, 1'024,
              1'024, 532'752, 1'057},
    RadixBank{"maximum", RadixBankKind::maximumSequential, maximumArrayLength,
              32, 1'024, maximumArrayLength, 532'752, 1'057},
};

[[nodiscard]] std::vector<std::uint8_t> makeRadixWire(RadixBank const &bank) {
  bool const maximum = bank.kind == RadixBankKind::maximumSequential;
  std::vector<std::uint8_t> bytes;
  bytes.reserve(maximum ? std::size_t{1'048'576}
                        : std::size_t{bank.length} * 2 + 2);
  if (maximum)
    bytes.insert(bytes.end(), maximumFixtureNopsPerObject, 0x99);
  bytes.push_back(0xF9);
  for (std::uint32_t index = 0; index < bank.length; ++index) {
    bytes.push_back(0xEA);
    if (maximum)
      bytes.insert(bytes.end(), maximumFixtureNopsPerObject, 0x99);
    bytes.push_back(0xE1);
  }
  bytes.push_back(0xF1);
  return bytes;
}

template <class Observe>
[[nodiscard]] bool forEachRadixIndex(RadixBank const &bank, Observe observe) {
  switch (bank.kind) {
  case RadixBankKind::onePage:
  case RadixBankKind::pagesUnderOneBranch:
  case RadixBankKind::maximumSequential:
    for (std::uint32_t index = 0; index < bank.length; ++index) {
      if (!observe(index))
        return false;
    }
    return true;
  case RadixBankKind::pageUnderEveryBranch:
    for (std::uint32_t branch = 0; branch < 32; ++branch) {
      for (std::uint32_t slot = 0; slot < 32; ++slot) {
        if (!observe(branch * 1'024 + slot))
          return false;
      }
    }
    return true;
  case RadixBankKind::valueInEveryPage:
    for (std::uint32_t page = 0; page < 1'024; ++page) {
      if (!observe(page * 32))
        return false;
    }
    return true;
  }
  return false;
}

[[nodiscard]] std::size_t
radixAllocationCount(probe::RadixAllocationLedger const &ledger) noexcept {
  return static_cast<std::size_t>(ledger.rootAllocations) +
         ledger.branchAllocations + ledger.leafAllocations;
}

[[nodiscard]] bool sameRadixLedger(probe::RadixAllocationLedger const &left,
                                   probe::RadixAllocationLedger const &right) {
  return left.rootAllocations == right.rootAllocations &&
         left.branchAllocations == right.branchAllocations &&
         left.leafAllocations == right.leafAllocations &&
         left.hitPoisonAllocations == right.hitPoisonAllocations &&
         left.requestedBytes == right.requestedBytes;
}

[[nodiscard]] bool
verifyRadixMetrics(RadixBank const &bank,
                   probe::ArrayCacheMetrics const &metrics,
                   probe::RadixAllocationLedger const &ledger) {
  std::uint32_t const expectedFields = bank.length + 1;
  std::uint32_t const expectedScopes = bank.length + 2;
  std::uint32_t const expectedBytes =
      bank.kind == RadixBankKind::maximumSequential ? 1'048'576
                                                    : bank.length * 2 + 2;
  return metrics.rootPresent && metrics.branchCount == bank.expectedBranches &&
         metrics.pageCount == bank.expectedPages &&
         metrics.valueCount == bank.expectedValues &&
         metrics.reachableBranches == bank.expectedBranches &&
         metrics.reachablePages == bank.expectedPages &&
         metrics.reachableValues == bank.expectedValues &&
         metrics.reservedVersion == 0 && metrics.arrayLength == bank.length &&
         metrics.ownerByteCount == expectedBytes &&
         metrics.ownerFieldCount == expectedFields &&
         metrics.ownerScopeCount == expectedScopes &&
         metrics.rootBytes == nativeRootBytes &&
         metrics.branchBytes == nativeBranchBytes &&
         metrics.leafBytes == nativeLeafBytes &&
         metrics.requestedBytes == bank.expectedBytes &&
         metrics.allocationCount == bank.expectedAllocations &&
         ledger.rootAllocations == 1 &&
         ledger.branchAllocations == bank.expectedBranches &&
         ledger.leafAllocations == bank.expectedPages &&
         ledger.hitPoisonAllocations == 0 &&
         ledger.requestedBytes == bank.expectedBytes &&
         radixAllocationCount(ledger) == bank.expectedAllocations;
}

[[nodiscard]] RadixBankFailure
observeAndHit(JSContext *ctx, JSValueConst array, JSValueConst atMethod,
              std::uint32_t index, JSValueConst expected = JS_UNDEFINED) {
  LocalValue first(ctx, JS_IsUndefined(expected)
                            ? JS_GetPropertyUint32(ctx, array, index)
                            : JS_DupValue(ctx, expected));
  if (first.isException() || !types::isSTObject(first.get()))
    return RadixBankFailure::access;
  auto const beforeHits = probe::gcProbeRadixLedger();
  LocalValue numericHit(ctx, JS_GetPropertyUint32(ctx, array, index));
  LocalValue argument(ctx, JS_NewUint32(ctx, index));
  JSValueConst arguments[] = {argument.get()};
  LocalValue methodHit(ctx, JS_Call(ctx, atMethod, array, 1, arguments));
  if (numericHit.isException() || methodHit.isException())
    return RadixBankFailure::access;
  if (JS_StrictEq(ctx, first.get(), numericHit.get()) != 1 ||
      JS_StrictEq(ctx, first.get(), methodHit.get()) != 1)
    return RadixBankFailure::identity;
  if (!sameRadixLedger(beforeHits, probe::gcProbeRadixLedger()))
    return RadixBankFailure::hitAllocation;
  return RadixBankFailure::none;
}

[[nodiscard]] RadixBankFailure
exerciseMaximumSequential(JSContext *ctx, JSValueConst array,
                          JSValueConst atMethod, RadixBank const &bank) {
  LocalValue iterator = iteratorFor(ctx, array);
  LocalValue next(ctx, JS_GetPropertyStr(ctx, iterator.get(), "next"));
  if (iterator.isException() || next.isException() ||
      !JS_IsFunction(ctx, next.get()))
    return RadixBankFailure::iterator;

  for (std::uint32_t index = 0; index < bank.length; ++index) {
    LocalValue result(ctx,
                      JS_Call(ctx, next.get(), iterator.get(), 0, nullptr));
    LocalValue value(ctx, JS_GetPropertyStr(ctx, result.get(), "value"));
    LocalValue done(ctx, JS_GetPropertyStr(ctx, result.get(), "done"));
    if (result.isException() || value.isException() || done.isException() ||
        JS_ToBool(ctx, done.get()) != 0 || !types::isSTObject(value.get()))
      return RadixBankFailure::iterator;
    auto const observed =
        observeAndHit(ctx, array, atMethod, index, value.get());
    if (observed != RadixBankFailure::none)
      return observed;
  }
  LocalValue terminal(ctx,
                      JS_Call(ctx, next.get(), iterator.get(), 0, nullptr));
  LocalValue done(ctx, JS_GetPropertyStr(ctx, terminal.get(), "done"));
  if (terminal.isException() || done.isException() ||
      JS_ToBool(ctx, done.get()) != 1)
    return RadixBankFailure::iterator;
  return RadixBankFailure::none;
}

[[nodiscard]] RadixBankFailure exerciseRadixBank(RadixBank const &bank) {
  RuntimeFixture fixture;
  if (!fixture.ready())
    return RadixBankFailure::setup;
  auto *ctx = fixture.context();
  auto const bytes = makeRadixWire(bank);
  std::size_t const expectedWire = bank.kind == RadixBankKind::maximumSequential
                                       ? std::size_t{1'048'576}
                                       : std::size_t{bank.length} * 2 + 2;
  if (bytes.size() != expectedWire)
    return RadixBankFailure::setup;

  LocalValue root = mint(ctx, bytes.data(), bytes.size());
  LocalValue array(ctx, JS_GetPropertyStr(ctx, root.get(), "Memos"));
  LocalValue atMethod(ctx, JS_GetPropertyStr(ctx, array.get(), "at"));
  if (root.isException() || array.isException() || atMethod.isException() ||
      !types::isSTArray(array.get()) || !JS_IsFunction(ctx, atMethod.get()))
    return RadixBankFailure::setup;

  probe::ArrayCacheMetrics untouched;
  if (!probe::inspectArrayCache(array.get(), untouched) ||
      untouched.rootPresent || untouched.branchCount != 0 ||
      untouched.pageCount != 0 || untouched.valueCount != 0 ||
      untouched.reachableBranches != 0 || untouched.reachablePages != 0 ||
      untouched.reachableValues != 0 || untouched.requestedBytes != 0 ||
      untouched.allocationCount != 0 ||
      radixAllocationCount(probe::gcProbeRadixLedger()) != 0)
    return RadixBankFailure::metrics;

  RadixBankFailure failure = RadixBankFailure::none;
  if (bank.kind == RadixBankKind::maximumSequential) {
    failure = exerciseMaximumSequential(ctx, array.get(), atMethod.get(), bank);
  } else {
    bool const completed = forEachRadixIndex(bank, [&](std::uint32_t index) {
      failure = observeAndHit(ctx, array.get(), atMethod.get(), index);
      return failure == RadixBankFailure::none;
    });
    if (!completed && failure == RadixBankFailure::none)
      failure = RadixBankFailure::access;
  }
  if (failure != RadixBankFailure::none)
    return failure;

  probe::ArrayCacheMetrics metrics;
  if (!probe::inspectArrayCache(array.get(), metrics) ||
      !verifyRadixMetrics(bank, metrics, probe::gcProbeRadixLedger()))
    return RadixBankFailure::metrics;
  return RadixBankFailure::none;
}

[[nodiscard]] int runRadixTopology(probe::RadixPoison poison) {
  if (sizeof(void *) != 8 || sizeof(JSValue) != 16) {
    std::fprintf(stderr, "radix topology probe requires native 64-bit ABI\n");
    return 75;
  }

  if (poison != probe::RadixPoison::none) {
    probe::configureRadixProbe(poison);
    auto const failure = exerciseRadixBank(radixBanks.front());
    auto const expected = poison == probe::RadixPoison::corruptCounts
                              ? RadixBankFailure::metrics
                              : RadixBankFailure::hitAllocation;
    if (failure != expected) {
      std::fprintf(stderr,
                   "radix poison escaped or failed incorrectly: observed=%u "
                   "expected=%u\n",
                   static_cast<unsigned>(failure),
                   static_cast<unsigned>(expected));
      return 75;
    }
    std::printf("radix %s poison detected\n",
                poison == probe::RadixPoison::corruptCounts ? "topology-count"
                                                            : "hit-allocation");
    return 23;
  }

  for (auto const &bank : radixBanks) {
    probe::configureRadixProbe(probe::RadixPoison::none);
    auto const failure = exerciseRadixBank(bank);
    if (failure != RadixBankFailure::none) {
      std::fprintf(stderr, "radix bank %s failed: %u\n", bank.name,
                   static_cast<unsigned>(failure));
      return 75;
    }
    std::printf("radix %s B=%u P=%u M=%u bytes=%zu allocations=%zu\n",
                bank.name, bank.expectedBranches, bank.expectedPages,
                bank.expectedValues, bank.expectedBytes,
                bank.expectedAllocations);
  }
  std::printf("radix maximum wire=1048576 fields=32768 scopes=32769 "
              "length=32767\n");
  return 0;
}

[[nodiscard]] int runLifetimeOrder() {
  probe::configureGCProbe(probe::HiddenEdge::none, false);
  {
    RuntimeFixture fixture;
    if (!fixture.ready() || !runParentBeforeChild(fixture)) {
      std::fprintf(stderr, "parent-before-child lifetime case failed: %s\n",
                   fixture.takeException("no pending exception").c_str());
      return 74;
    }
  }

  probe::configureGCProbe(probe::HiddenEdge::none, false);
  RuntimeFixture fixture;
  if (!fixture.ready() || !runIteratorBeforeElement(fixture)) {
    std::fprintf(stderr, "iterator-before-element lifetime case failed: %s\n",
                 fixture.takeException("no pending exception").c_str());
    return 74;
  }
  std::printf("lifetime order: ok\n");
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && std::strcmp(argv[1], "radix-topology") == 0)
    return runRadixTopology(probe::RadixPoison::none);
  if (argc == 2 && std::strcmp(argv[1], "radix-topology-count-poison") == 0)
    return runRadixTopology(probe::RadixPoison::corruptCounts);
  if (argc == 2 && std::strcmp(argv[1], "radix-hit-allocation-poison") == 0)
    return runRadixTopology(probe::RadixPoison::allocateOnHit);
  if (argc == 2 && std::strcmp(argv[1], "lifetime-order") == 0)
    return runLifetimeOrder();
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s EDGE enabled|disabled | lifetime-order\n",
                 argv[0]);
    return 64;
  }
  probe::HiddenEdge edge = probe::HiddenEdge::none;
  if (!probe::parseHiddenEdge(argv[1], edge)) {
    std::fprintf(stderr, "unknown edge: %s\n", argv[1]);
    return 64;
  }
  bool const disabled = std::strcmp(argv[2], "disabled") == 0;
  if (!disabled && std::strcmp(argv[2], "enabled") != 0) {
    std::fprintf(stderr, "unknown mark mode: %s\n", argv[2]);
    return 64;
  }
  return runEdge(edge, disabled);
}
