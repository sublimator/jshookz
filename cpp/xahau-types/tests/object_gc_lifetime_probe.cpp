#include "amount/amount_js.hpp"
#include "js.hpp"
#include "leaf/leaf.hpp"
#include "object/field_js.hpp"
#include "object/object.hpp"
#include "object_gc_lifetime_probe_hooks.hpp"
#include "pathset/pathset_js.hpp"
#include "quickjs.hpp"

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

[[nodiscard]] JSValue makeIssueForAmount(JSContext *ctx, JSValueConst owner,
                                         types::AmountIssueKind kind,
                                         std::uint8_t const *identity,
                                         std::uint32_t length) {
  if (kind == types::AmountIssueKind::native) {
    std::uint8_t native[20] = {};
    return JS_IsUndefined(owner)
               ? types::makeIssueBytes(ctx, native, sizeof(native))
               : types::makeIssueDerivedBytes(ctx, owner, native,
                                              sizeof(native));
  }
  if (kind == types::AmountIssueKind::iou)
    return JS_IsUndefined(owner)
               ? types::makeIssueBytes(ctx, identity, length)
               : types::makeIssueView(ctx, owner, identity, length);
  if (identity == nullptr || length != 24)
    return JS_ThrowInternalError(ctx, "invalid certified MPT issue identity");
  std::uint8_t issue[44] = {};
  std::memcpy(issue, identity + 4, 20);
  issue[39] = 1;
  issue[40] = identity[3];
  issue[41] = identity[2];
  issue[42] = identity[1];
  issue[43] = identity[0];
  return JS_IsUndefined(owner)
             ? types::makeIssueBytes(ctx, issue, sizeof(issue))
             : types::makeIssueDerivedBytes(ctx, owner, issue, sizeof(issue));
}

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
    jshookz::provider::qjs::resetByteClassRegistry();
    LocalValue global(context_, JS_GetGlobalObject(context_));
    types::AmountLeafMaterializers const amountLeaves{
        types::makeAccountIDView, types::makeCurrencyView,
        types::makeHash192View,   types::makeXFLDecimalParts,
        makeIssueForAmount,
    };
    types::PathSetLeafMaterializers const pathLeaves{
        types::makeAccountIDView,
        types::makeCurrencyView,
        types::isCertifiedObjectRange,
    };
    if (global.isException() ||
        !types::registerSTBlob(context_, global.get()) ||
        !types::registerHash256(context_, global.get()) ||
        !types::registerAccountID(context_, global.get()) ||
        !types::registerXFL(context_) ||
        !types::registerRichLeafTypes(context_) ||
        !types::registerAmount(context_, amountLeaves) ||
        !types::registerObjectTypes(context_) ||
        !types::registerPathSet(context_, pathLeaves))
      error_ = takeException("registerObjectTypes failed");
  }

  RuntimeFixture(RuntimeFixture const &) = delete;
  RuntimeFixture &operator=(RuntimeFixture const &) = delete;

  ~RuntimeFixture() {
    if (context_ != nullptr)
      JS_FreeContext(context_);
    if (runtime_ != nullptr) {
      types::unregisterObjectTypes(runtime_);
      types::unregisterRichLeafTypes(runtime_);
      jshookz::provider::qjs::resetByteClassRegistry();
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
  case probe::TrackedEntity::blob:
    return "blob";
  case probe::TrackedEntity::hash256:
    return "hash256";
  case probe::TrackedEntity::accountID:
    return "account-id";
  case probe::TrackedEntity::amount:
    return "amount";
  case probe::TrackedEntity::hash128:
    return "hash128";
  case probe::TrackedEntity::hash160:
    return "hash160";
  case probe::TrackedEntity::hash192:
    return "hash192";
  case probe::TrackedEntity::currency:
    return "currency";
  case probe::TrackedEntity::issue:
    return "issue";
  case probe::TrackedEntity::vector256:
    return "vector256";
  case probe::TrackedEntity::vectorIterator:
    return "vector-iterator";
  case probe::TrackedEntity::xchainBridge:
    return "xchain-bridge";
  case probe::TrackedEntity::pathSet:
    return "path-set";
  case probe::TrackedEntity::path:
    return "path";
  case probe::TrackedEntity::pathHop:
    return "path-hop";
  case probe::TrackedEntity::pathIterator:
    return "path-iterator";
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

void dumpLiveCounts() {
  for (std::size_t i = 0; i < entityCount; ++i) {
    auto const entity = static_cast<probe::TrackedEntity>(i);
    auto const observed = probe::gcProbeCounts(entity);
    if (observed.created != 0)
      std::fprintf(stderr, "%s live counts: %u/%u\n", entityName(entity),
                   observed.created, observed.finalized);
  }
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

[[nodiscard]] LocalValue callAt(JSContext *ctx, JSValueConst value,
                                std::uint32_t index) {
  LocalValue method(ctx, JS_GetPropertyStr(ctx, value, "at"));
  LocalValue argument(ctx, JS_NewUint32(ctx, index));
  if (method.isException() || argument.isException())
    return LocalValue(ctx, JS_EXCEPTION);
  JSValueConst arguments[] = {argument.get()};
  return LocalValue(ctx, JS_Call(ctx, method.get(), value, 1, arguments));
}

[[nodiscard]] std::vector<std::uint8_t> decodeHex(char const *hex) {
  auto nibble = [](char value) -> std::uint8_t {
    if (value >= '0' && value <= '9')
      return static_cast<std::uint8_t>(value - '0');
    if (value >= 'A' && value <= 'F')
      return static_cast<std::uint8_t>(value - 'A' + 10);
    return static_cast<std::uint8_t>(value - 'a' + 10);
  };
  std::size_t const length = std::strlen(hex);
  std::vector<std::uint8_t> bytes(length / 2);
  for (std::size_t i = 0; i < bytes.size(); ++i)
    bytes[i] = static_cast<std::uint8_t>((nibble(hex[i * 2]) << 4) |
                                         nibble(hex[i * 2 + 1]));
  return bytes;
}

[[nodiscard]] bool prepareRichCycle(RuntimeFixture &fixture,
                                    probe::HiddenEdge edge) {
  auto *ctx = fixture.context();
  char const *field = nullptr;
  char const *wire = nullptr;
  switch (edge) {
  case probe::HiddenEdge::blobOwner:
    field = "PublicKey";
    wire = "710701020304050607";
    break;
  case probe::HiddenEdge::hash256Owner:
    field = "LedgerHash";
    wire = "510102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F20";
    break;
  case probe::HiddenEdge::accountIDOwner:
    field = "Account";
    wire = "8114B5F762798A53D543A014CAF8B297CFF8F2F937E8";
    break;
  case probe::HiddenEdge::amountOwner:
    field = "Amount";
    wire = "61400000000000002A";
    break;
  case probe::HiddenEdge::hash128Owner:
    field = "EmailHash";
    wire = "410102030405060708090A0B0C0D0E0F10";
    break;
  case probe::HiddenEdge::hash160Owner:
    field = "TakerPaysCurrency";
    wire = "01110102030405060708090A0B0C0D0E0F1011121314";
    break;
  case probe::HiddenEdge::hash192Owner:
    field = "MPTokenIssuanceID";
    wire = "01150102030405060708090A0B0C0D0E0F101112131415161718";
    break;
  case probe::HiddenEdge::currencyOwner:
    field = "BaseAsset";
    wire = "011A0000000000000000000000005553440000000000";
    break;
  case probe::HiddenEdge::issueOwner:
  case probe::HiddenEdge::issueCacheValue:
    field = "LockingChainIssue";
    wire = "01180000000000000000000000000000000000000000";
    break;
  case probe::HiddenEdge::vector256Owner:
  case probe::HiddenEdge::vector256CacheValue:
  case probe::HiddenEdge::vector256Iterator:
    field = "Indexes";
    wire = "0113200102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1"
           "F20";
    break;
  case probe::HiddenEdge::xchainBridgeOwner:
  case probe::HiddenEdge::xchainBridgeCacheValue:
    field = "XChainBridge";
    wire = "011914B5F762798A53D543A014CAF8B297CFF8F2F937E8"
           "0000000000000000000000000000000000000000"
           "14B5F762798A53D543A014CAF8B297CFF8F2F937E8"
           "0000000000000000000000000000000000000000";
    break;
  default:
    return false;
  }
  auto const bytes = decodeHex(wire);
  LocalValue root = mint(ctx, bytes.data(), bytes.size());
  LocalValue parent(ctx, JS_GetPropertyStr(ctx, root.get(), field));
  if (root.isException() || parent.isException() || !JS_IsObject(parent.get()))
    return false;

  if (edge == probe::HiddenEdge::issueCacheValue) {
    LocalValue child;
    {
      PendingTarget pending(parent.get());
      child = LocalValue(ctx, JS_GetPropertyStr(ctx, parent.get(), "currency"));
    }
    return !child.isException() && JS_IsObject(child.get());
  }
  if (edge == probe::HiddenEdge::vector256CacheValue) {
    LocalValue child;
    {
      PendingTarget pending(parent.get());
      child = LocalValue(ctx, JS_GetPropertyUint32(ctx, parent.get(), 0));
    }
    return !child.isException() && JS_IsObject(child.get());
  }
  if (edge == probe::HiddenEdge::xchainBridgeCacheValue) {
    LocalValue child;
    {
      PendingTarget pending(parent.get());
      child = LocalValue(
          ctx, JS_GetPropertyStr(ctx, parent.get(), "LockingChainDoor"));
    }
    return !child.isException() && JS_IsObject(child.get());
  }
  if (edge == probe::HiddenEdge::vector256Iterator) {
    LocalValue iterator = iteratorFor(ctx, parent.get());
    return !iterator.isException() && JS_IsObject(iterator.get());
  }
  return true;
}

[[nodiscard]] bool preparePathCycle(RuntimeFixture &fixture,
                                    probe::HiddenEdge edge) {
  auto *ctx = fixture.context();
  static constexpr std::uint8_t bytes[] = {
      0x81, 0x14, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
      0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53, 0x01, 0x12,
      0x01, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A,
      0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53, 0x00,
  };
  LocalValue root = mint(ctx, bytes, sizeof(bytes));
  LocalValue pathSet(ctx, JS_GetPropertyStr(ctx, root.get(), "Paths"));
  if (root.isException() || pathSet.isException() ||
      !types::isPathSet(pathSet.get()))
    return false;
  if (edge == probe::HiddenEdge::pathSetOwner)
    return true;
  if (edge == probe::HiddenEdge::pathIteratorParent) {
    LocalValue iterator = iteratorFor(ctx, pathSet.get());
    return !iterator.isException() && JS_IsObject(iterator.get());
  }

  LocalValue path = callAt(ctx, pathSet.get(), 0);
  if (path.isException() || !types::isPath(path.get()))
    return false;
  if (edge == probe::HiddenEdge::pathParent)
    return true;
  if (edge == probe::HiddenEdge::pathHopParent) {
    LocalValue hop = callAt(ctx, path.get(), 0);
    return !hop.isException() && types::isPathHop(hop.get());
  }
  return false;
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
  case probe::HiddenEdge::blobOwner:
  case probe::HiddenEdge::hash256Owner:
  case probe::HiddenEdge::accountIDOwner:
  case probe::HiddenEdge::amountOwner:
  case probe::HiddenEdge::hash128Owner:
  case probe::HiddenEdge::hash160Owner:
  case probe::HiddenEdge::hash192Owner:
  case probe::HiddenEdge::currencyOwner:
  case probe::HiddenEdge::issueOwner:
  case probe::HiddenEdge::issueCacheValue:
  case probe::HiddenEdge::vector256Owner:
  case probe::HiddenEdge::vector256CacheValue:
  case probe::HiddenEdge::vector256Iterator:
  case probe::HiddenEdge::xchainBridgeOwner:
  case probe::HiddenEdge::xchainBridgeCacheValue:
    set(probe::TrackedEntity::owner, 1);
    set(probe::TrackedEntity::object, 1);
    if (edge == probe::HiddenEdge::blobOwner)
      set(probe::TrackedEntity::blob, 1);
    else if (edge == probe::HiddenEdge::hash256Owner)
      set(probe::TrackedEntity::hash256, 1);
    else if (edge == probe::HiddenEdge::accountIDOwner)
      set(probe::TrackedEntity::accountID, 1);
    else if (edge == probe::HiddenEdge::amountOwner)
      set(probe::TrackedEntity::amount, 1);
    else if (edge == probe::HiddenEdge::hash128Owner)
      set(probe::TrackedEntity::hash128, 1);
    else if (edge == probe::HiddenEdge::hash160Owner)
      set(probe::TrackedEntity::hash160, 1);
    else if (edge == probe::HiddenEdge::hash192Owner)
      set(probe::TrackedEntity::hash192, 1);
    else if (edge == probe::HiddenEdge::currencyOwner)
      set(probe::TrackedEntity::currency, 1);
    else if (edge == probe::HiddenEdge::issueOwner)
      set(probe::TrackedEntity::issue, 1);
    else if (edge == probe::HiddenEdge::issueCacheValue) {
      set(probe::TrackedEntity::issue, 1);
      set(probe::TrackedEntity::currency, 1);
    } else if (edge == probe::HiddenEdge::vector256Owner)
      set(probe::TrackedEntity::vector256, 1);
    else if (edge == probe::HiddenEdge::vector256CacheValue) {
      set(probe::TrackedEntity::vector256, 1);
      set(probe::TrackedEntity::hash256, 1);
    } else if (edge == probe::HiddenEdge::vector256Iterator) {
      set(probe::TrackedEntity::vector256, 1);
      set(probe::TrackedEntity::vectorIterator, 1);
    } else if (edge == probe::HiddenEdge::xchainBridgeOwner)
      set(probe::TrackedEntity::xchainBridge, 1);
    else {
      set(probe::TrackedEntity::xchainBridge, 1);
      set(probe::TrackedEntity::accountID, 1);
    }
    break;
  case probe::HiddenEdge::pathSetOwner:
  case probe::HiddenEdge::pathParent:
  case probe::HiddenEdge::pathHopParent:
  case probe::HiddenEdge::pathIteratorParent:
    set(probe::TrackedEntity::owner, 1);
    set(probe::TrackedEntity::object, 1);
    set(probe::TrackedEntity::pathSet, 1);
    if (edge == probe::HiddenEdge::pathParent ||
        edge == probe::HiddenEdge::pathHopParent)
      set(probe::TrackedEntity::path, 1);
    if (edge == probe::HiddenEdge::pathHopParent)
      set(probe::TrackedEntity::pathHop, 1);
    if (edge == probe::HiddenEdge::pathIteratorParent)
      set(probe::TrackedEntity::pathIterator, 1);
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

  if (edge >= probe::HiddenEdge::blobOwner &&
      edge <= probe::HiddenEdge::xchainBridgeCacheValue)
    return prepareRichCycle(fixture, edge);
  if (edge >= probe::HiddenEdge::pathSetOwner &&
      edge <= probe::HiddenEdge::pathIteratorParent)
    return preparePathCycle(fixture, edge);

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
      dumpLiveCounts();
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
    dumpLiveCounts();
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
