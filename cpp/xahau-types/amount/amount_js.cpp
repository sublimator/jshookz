#include "amount/amount_js.hpp"

#include "js.hpp"
#include "object/nominal_payload.hpp"
#include "object/object.hpp"

#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
#include "tests/object_gc_lifetime_probe_hooks.hpp"
#endif

#include <catl/core/types.h>
#include <catl/xdata/amount-rules.h>

#include <array>
#include <cstring>
#include <limits>

namespace jshookz::provider::types {
namespace {

namespace qjs = jshookz::provider::qjs;
namespace xdata = catl::xdata;

JSClassID amountClassId;
AmountLeafMaterializers leafMaterializers{};

struct AmountState {
  JSValue owner = JS_UNDEFINED;
  std::uint8_t const *data = nullptr;
  std::array<std::uint8_t, 48> bytes{};
  std::uint8_t length = 0;
};

[[nodiscard]] JSValue oom(JSContext *ctx) {
  return JS_HasException(ctx) ? JS_EXCEPTION : JS_ThrowOutOfMemory(ctx);
}

void amountFinalizer(JSRuntime *runtime, JSValue value) {
  auto *state = static_cast<AmountState *>(JS_GetOpaque(value, amountClassId));
  if (state == nullptr)
    return;
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
  if (!JS_IsUndefined(state->owner))
    test::gcProbeFinalized(test::TrackedEntity::amount, value);
#endif
  JS_FreeValueRT(runtime, state->owner);
  js_free_rt(runtime, state);
}

void amountMark(JSRuntime *runtime, JSValueConst value, JS_MarkFunc *mark) {
  auto *state = static_cast<AmountState *>(JS_GetOpaque(value, amountClassId));
  if (state != nullptr) {
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
    if (!test::gcProbeMarkEnabled(test::HiddenEdge::amountOwner))
      return;
#endif
    JS_MarkValue(runtime, state->owner, mark);
  }
}

JSClassDef const amountClass = {
    .class_name = "Amount",
    .finalizer = amountFinalizer,
    .gc_mark = amountMark,
    .call = nullptr,
    .exotic = nullptr,
};

[[nodiscard]] AmountState *amountState(JSContext *ctx, JSValueConst value) {
  return static_cast<AmountState *>(JS_GetOpaque2(ctx, value, amountClassId));
}

[[nodiscard]] Slice payload(AmountState const &state) noexcept {
  return {state.data, state.length};
}

[[nodiscard]] xdata::AmountRules::Parts
parts(AmountState const &state) noexcept {
  return xdata::AmountRules::parts(payload(state));
}

[[nodiscard]] char const *kindName(xdata::AmountRules::Kind kind) noexcept {
  using Kind = xdata::AmountRules::Kind;
  switch (kind) {
  case Kind::Native:
    return "native";
  case Kind::Iou:
    return "iou";
  case Kind::Mpt:
    return "mpt";
  }
  return "native";
}

[[nodiscard]] JSValue newAmount(JSContext *ctx, JSValueConst owner,
                                std::uint8_t const *bytes,
                                std::uint32_t length) {
  if ((length != 8 && length != 33 && length != 48) || bytes == nullptr)
    return JS_ThrowTypeError(ctx, "Amount: invalid canonical byte length");
  Slice const wire{bytes, length};
  if (char const *error = xdata::AmountRules::certify(wire))
    return JS_ThrowTypeError(ctx, "Amount: %s", error);
  if (!JS_IsUndefined(owner) &&
      !isCertifiedObjectRange(ctx, owner, bytes, length))
    return JS_HasException(ctx)
               ? JS_EXCEPTION
               : JS_ThrowTypeError(ctx, "Amount: certified owner is required");

  auto *state =
      static_cast<AmountState *>(js_mallocz(ctx, sizeof(AmountState)));
  if (state == nullptr)
    return oom(ctx);
  if (JS_IsUndefined(owner)) {
    std::memcpy(state->bytes.data(), bytes, length);
    state->data = state->bytes.data();
  } else {
    state->owner = JS_DupValue(ctx, owner);
    state->data = bytes;
  }
  state->length = static_cast<std::uint8_t>(length);

  JSValue value = JS_NewObjectClass(ctx, amountClassId);
  if (JS_IsException(value)) {
    JS_FreeValue(ctx, state->owner);
    js_free(ctx, state);
    return value;
  }
  JS_SetOpaque(value, state);
#if defined(JSHOOKZ_XAHAU_TYPES_GC_PROBE)
  if (!JS_IsUndefined(owner)) {
    test::gcProbeCreated(test::TrackedEntity::amount, value);
    if (!test::gcProbePlantCycle(ctx, test::HiddenEdge::amountOwner, owner,
                                 value, value)) {
      JS_FreeValue(ctx, value);
      return JS_EXCEPTION;
    }
  }
#endif
  if (JS_PreventExtensions(ctx, value) < 0) {
    JS_FreeValue(ctx, value);
    return JS_EXCEPTION;
  }
  return value;
}

constexpr std::uint64_t kMaximumNativeDrops = 100'000'000'000'000'000ULL;

enum class DropsInputStatus : std::uint8_t {
  valid,
  outOfRange,
  exception,
};

[[nodiscard]] DropsInputStatus readDrops(JSContext *ctx, JSValueConst input,
                                         std::uint64_t &output) {
  qjs::OwnedValue rendered(ctx, JS_ToString(ctx, input));
  if (rendered.isException())
    return DropsInputStatus::exception;
  std::size_t length = 0;
  char const *text = JS_ToCStringLen(ctx, &length, rendered.get());
  if (text == nullptr)
    return DropsInputStatus::exception;

  bool valid = length != 0;
  std::uint64_t value = 0;
  for (std::size_t index = 0; valid && index < length; ++index) {
    char const digit = text[index];
    valid = digit >= '0' && digit <= '9';
    if (!valid)
      break;
    std::uint64_t const part = static_cast<std::uint64_t>(digit - '0');
    if (value > (kMaximumNativeDrops - part) / 10) {
      valid = false;
      break;
    }
    value = value * 10 + part;
  }
  JS_FreeCString(ctx, text);
  if (!valid)
    return DropsInputStatus::outOfRange;
  output = value;
  return DropsInputStatus::valid;
}

[[nodiscard]] JSValue amountFactoryDrops(JSContext *ctx, JSValueConst, int argc,
                                         JSValueConst *argv) {
  if (argc < 1 || !JS_IsBigInt(ctx, argv[0]))
    return JS_ThrowTypeError(ctx, "Amount.drops expects bigint");

  std::uint64_t drops = 0;
  DropsInputStatus const status = readDrops(ctx, argv[0], drops);
  if (status == DropsInputStatus::exception)
    return JS_EXCEPTION;
  if (status == DropsInputStatus::outOfRange)
    return JS_ThrowRangeError(ctx, "Amount.drops value is out of range");

  std::uint64_t word = drops | xdata::AmountRules::kPositive;
  std::uint8_t wire[8];
  for (std::size_t index = sizeof(wire); index-- > 0;) {
    wire[index] = static_cast<std::uint8_t>(word);
    word >>= 8;
  }
  return newAmount(ctx, JS_UNDEFINED, wire, sizeof(wire));
}

[[nodiscard]] JSValue amountKind(JSContext *ctx, JSValueConst thisValue) {
  auto const *state = amountState(ctx, thisValue);
  return state == nullptr
             ? JS_EXCEPTION
             : JS_NewString(
                   ctx, kindName(xdata::AmountRules::kind(payload(*state))));
}

[[nodiscard]] JSValue amountByteLength(JSContext *ctx, JSValueConst thisValue) {
  auto const *state = amountState(ctx, thisValue);
  return state == nullptr ? JS_EXCEPTION : JS_NewUint32(ctx, state->length);
}

[[nodiscard]] JSValue amountToBytes(JSContext *ctx, JSValueConst thisValue, int,
                                    JSValueConst *) {
  auto const *state = amountState(ctx, thisValue);
  return state == nullptr ? JS_EXCEPTION
                          : qjs::uint8Array(ctx, {payload(*state).data(),
                                                  payload(*state).size()});
}

[[nodiscard]] JSValue amountIsKind(JSContext *ctx, JSValueConst thisValue,
                                   xdata::AmountRules::Kind expected) {
  auto const *state = amountState(ctx, thisValue);
  return state == nullptr
             ? JS_EXCEPTION
             : JS_NewBool(ctx, xdata::AmountRules::kind(payload(*state)) ==
                                   expected);
}

[[nodiscard]] JSValue amountIsNative(JSContext *ctx, JSValueConst value, int,
                                     JSValueConst *) {
  return amountIsKind(ctx, value, xdata::AmountRules::Kind::Native);
}

[[nodiscard]] JSValue amountIsIou(JSContext *ctx, JSValueConst value, int,
                                  JSValueConst *) {
  return amountIsKind(ctx, value, xdata::AmountRules::Kind::Iou);
}

[[nodiscard]] JSValue amountIsMpt(JSContext *ctx, JSValueConst value, int,
                                  JSValueConst *) {
  return amountIsKind(ctx, value, xdata::AmountRules::Kind::Mpt);
}

[[nodiscard]] JSValue amountAsKind(JSContext *ctx, JSValueConst value,
                                   xdata::AmountRules::Kind expected) {
  auto const *state = amountState(ctx, value);
  if (state == nullptr)
    return JS_EXCEPTION;
  return xdata::AmountRules::kind(payload(*state)) == expected
             ? JS_DupValue(ctx, value)
             : JS_UNDEFINED;
}

[[nodiscard]] JSValue amountAsNative(JSContext *ctx, JSValueConst value, int,
                                     JSValueConst *) {
  return amountAsKind(ctx, value, xdata::AmountRules::Kind::Native);
}

[[nodiscard]] JSValue amountAsIou(JSContext *ctx, JSValueConst value, int,
                                  JSValueConst *) {
  return amountAsKind(ctx, value, xdata::AmountRules::Kind::Iou);
}

[[nodiscard]] JSValue amountAsMpt(JSContext *ctx, JSValueConst value, int,
                                  JSValueConst *) {
  return amountAsKind(ctx, value, xdata::AmountRules::Kind::Mpt);
}

[[nodiscard]] JSValue signedBigInt(JSContext *ctx, bool negative,
                                   std::uint64_t magnitude) {
  if (!negative)
    return JS_NewBigUint64(ctx, magnitude);
  if (magnitude > (std::uint64_t{1} << 63))
    return JS_ThrowRangeError(ctx, "Amount magnitude exceeds signed bigint");
  if (magnitude == (std::uint64_t{1} << 63))
    return JS_NewBigInt64(ctx, std::numeric_limits<std::int64_t>::min());
  return JS_NewBigInt64(ctx, -static_cast<std::int64_t>(magnitude));
}

[[nodiscard]] JSValue amountDrops(JSContext *ctx, JSValueConst thisValue) {
  auto const *state = amountState(ctx, thisValue);
  if (state == nullptr)
    return JS_EXCEPTION;
  auto const value = parts(*state);
  return value.kind == xdata::AmountRules::Kind::Native
             ? signedBigInt(ctx, value.negative, value.magnitude)
             : JS_UNDEFINED;
}

[[nodiscard]] JSValue amountValue(JSContext *ctx, JSValueConst thisValue) {
  auto const *state = amountState(ctx, thisValue);
  if (state == nullptr)
    return JS_EXCEPTION;
  auto const value = parts(*state);
  if (value.kind == xdata::AmountRules::Kind::Iou)
    return leafMaterializers.decimal(ctx, value.negative, value.magnitude,
                                     value.exponent);
  if (value.kind == xdata::AmountRules::Kind::Mpt)
    return signedBigInt(ctx, value.negative, value.magnitude);
  return JS_UNDEFINED;
}

[[nodiscard]] JSValue amountCurrency(JSContext *ctx, JSValueConst thisValue) {
  auto const *state = amountState(ctx, thisValue);
  if (state == nullptr)
    return JS_EXCEPTION;
  auto const value = parts(*state);
  return value.kind == xdata::AmountRules::Kind::Iou
             ? leafMaterializers.currency(ctx, state->owner,
                                          payload(*state).data() + 8, 20)
             : JS_UNDEFINED;
}

[[nodiscard]] JSValue amountIssuer(JSContext *ctx, JSValueConst thisValue) {
  auto const *state = amountState(ctx, thisValue);
  if (state == nullptr)
    return JS_EXCEPTION;
  auto const value = parts(*state);
  return value.kind == xdata::AmountRules::Kind::Iou
             ? leafMaterializers.accountID(ctx, state->owner,
                                           payload(*state).data() + 28, 20)
             : JS_UNDEFINED;
}

[[nodiscard]] JSValue amountMptId(JSContext *ctx, JSValueConst thisValue) {
  auto const *state = amountState(ctx, thisValue);
  if (state == nullptr)
    return JS_EXCEPTION;
  auto const value = parts(*state);
  return value.kind == xdata::AmountRules::Kind::Mpt
             ? leafMaterializers.hash192(ctx, state->owner,
                                         payload(*state).data() + 9, 24)
             : JS_UNDEFINED;
}

[[nodiscard]] JSValue amountIssue(JSContext *ctx, JSValueConst thisValue) {
  auto const *state = amountState(ctx, thisValue);
  if (state == nullptr)
    return JS_EXCEPTION;
  auto const value = parts(*state);
  using Kind = xdata::AmountRules::Kind;
  if (value.kind == Kind::Native)
    return leafMaterializers.issue(ctx, state->owner, AmountIssueKind::native,
                                   nullptr, 0);
  if (value.kind == Kind::Iou)
    return leafMaterializers.issue(ctx, state->owner, AmountIssueKind::iou,
                                   payload(*state).data() + 8, 40);
  return leafMaterializers.issue(ctx, state->owner, AmountIssueKind::mpt,
                                 payload(*state).data() + 9, 24);
}

[[nodiscard]] bool sameIssue(AmountState const &left, AmountState const &right,
                             xdata::AmountRules::Kind kind) noexcept {
  if (kind == xdata::AmountRules::Kind::Native)
    return true;
  std::size_t const offset = kind == xdata::AmountRules::Kind::Iou ? 8 : 9;
  std::size_t const length = kind == xdata::AmountRules::Kind::Iou ? 40 : 24;
  return std::memcmp(payload(left).data() + offset,
                     payload(right).data() + offset, length) == 0;
}

[[nodiscard]] int
compareParts(xdata::AmountRules::Parts const &left,
             xdata::AmountRules::Parts const &right) noexcept {
  if (left.negative != right.negative)
    return left.negative ? -1 : 1;
  if (left.magnitude == 0 || right.magnitude == 0) {
    if (left.magnitude == right.magnitude)
      return 0;
    int const order = left.magnitude == 0 ? -1 : 1;
    return left.negative ? -order : order;
  }
  if (left.exponent != right.exponent) {
    int const order = left.exponent < right.exponent ? -1 : 1;
    return left.negative ? -order : order;
  }
  if (left.magnitude == right.magnitude)
    return 0;
  int const order = left.magnitude < right.magnitude ? -1 : 1;
  return left.negative ? -order : order;
}

[[nodiscard]] JSValue amountEquals(JSContext *ctx, JSValueConst thisValue,
                                   int argc, JSValueConst *argv) {
  auto const *left = amountState(ctx, thisValue);
  if (left == nullptr)
    return JS_EXCEPTION;
  if (argc < 1 || !isAmount(argv[0]))
    return JS_FALSE;
  auto const *right =
      static_cast<AmountState const *>(JS_GetOpaque(argv[0], amountClassId));
  auto const leftParts = parts(*left);
  auto const rightParts = parts(*right);
  return JS_NewBool(ctx, leftParts.kind == rightParts.kind &&
                             sameIssue(*left, *right, leftParts.kind) &&
                             compareParts(leftParts, rightParts) == 0);
}

[[nodiscard]] JSValue amountCompare(JSContext *ctx, JSValueConst thisValue,
                                    int argc, JSValueConst *argv) {
  auto const *left = amountState(ctx, thisValue);
  auto const *right = argc > 0 && isAmount(argv[0])
                          ? static_cast<AmountState const *>(
                                JS_GetOpaque(argv[0], amountClassId))
                          : nullptr;
  if (left == nullptr)
    return JS_EXCEPTION;
  if (right == nullptr)
    return JS_ThrowTypeError(ctx, "Amount.compare() expects Amount");
  auto const leftParts = parts(*left);
  auto const rightParts = parts(*right);
  if (leftParts.kind != rightParts.kind ||
      !sameIssue(*left, *right, leftParts.kind))
    return JS_ThrowTypeError(ctx, "Amount values have different issues");
  return JS_NewInt32(ctx, compareParts(leftParts, rightParts));
}

[[nodiscard]] JSValue amountToXfl(JSContext *ctx, JSValueConst thisValue, int,
                                  JSValueConst *) {
  auto const *state = amountState(ctx, thisValue);
  if (state == nullptr)
    return JS_EXCEPTION;
  auto const value = parts(*state);
  return leafMaterializers.decimal(ctx, value.negative, value.magnitude,
                                   value.exponent);
}

[[nodiscard]] JSValue amountToString(JSContext *ctx, JSValueConst thisValue,
                                     int, JSValueConst *) {
  auto const *state = amountState(ctx, thisValue);
  if (state == nullptr)
    return JS_EXCEPTION;
  auto const value = parts(*state);
  if (value.magnitude == 0)
    return JS_NewString(ctx, "0");

  char digits[24];
  char *end = digits + sizeof(digits);
  char *cursor = end;
  std::uint64_t magnitude = value.magnitude;
  do {
    *--cursor = static_cast<char>('0' + magnitude % 10);
    magnitude /= 10;
  } while (magnitude != 0);
  std::uint32_t const digitCount = static_cast<std::uint32_t>(end - cursor);
  char output[128];
  std::uint32_t position = 0;
  if (value.negative)
    output[position++] = '-';
  auto append = [&](char const *source, std::uint32_t count) noexcept {
    std::memcpy(output + position, source, count);
    position += count;
  };
  std::int32_t const decimal =
      static_cast<std::int32_t>(digitCount) + value.exponent;
  if (decimal <= 0) {
    output[position++] = '0';
    output[position++] = '.';
    for (std::int32_t index = 0; index < -decimal; ++index)
      output[position++] = '0';
    append(cursor, digitCount);
  } else if (decimal < static_cast<std::int32_t>(digitCount)) {
    append(cursor, static_cast<std::uint32_t>(decimal));
    output[position++] = '.';
    append(cursor + decimal, digitCount - static_cast<std::uint32_t>(decimal));
  } else {
    append(cursor, digitCount);
    for (std::int32_t index = digitCount; index < decimal; ++index)
      output[position++] = '0';
  }
  return JS_NewStringLen(ctx, output, position);
}

JSCFunctionListEntry const amountPrototype[] = {
    // @binding provider:Amount.kind
    JS_CGETSET_DEF("kind", amountKind, nullptr),
    // @binding provider:Amount.issue
    JS_CGETSET_DEF("issue", amountIssue, nullptr),
    // @binding provider:Amount.currency
    JS_CGETSET_DEF("currency", amountCurrency, nullptr),
    // @binding provider:Amount.issuer
    JS_CGETSET_DEF("issuer", amountIssuer, nullptr),
    // @binding provider:Amount.mptIssuanceId
    JS_CGETSET_DEF("mptIssuanceId", amountMptId, nullptr),
    // @binding provider:Amount.value
    JS_CGETSET_DEF("value", amountValue, nullptr),
    // @binding provider:Amount.drops
    JS_CGETSET_DEF("drops", amountDrops, nullptr),
    // @binding provider:Amount.byteLength
    JS_CGETSET_DEF("byteLength", amountByteLength, nullptr),
    // @binding provider:Amount.toBytes
    JS_CFUNC_DEF("toBytes", 0, amountToBytes),
    // @binding provider:Amount.toXFL
    JS_CFUNC_DEF("toXFL", 0, amountToXfl),
    // @binding provider:Amount.toString
    JS_CFUNC_DEF("toString", 0, amountToString),
    // @binding provider:Amount.isNative
    JS_CFUNC_DEF("isNative", 0, amountIsNative),
    // @binding provider:Amount.isIOU
    JS_CFUNC_DEF("isIOU", 0, amountIsIou),
    // @binding provider:Amount.isMPT
    JS_CFUNC_DEF("isMPT", 0, amountIsMpt),
    // @binding provider:Amount.asNative
    JS_CFUNC_DEF("asNative", 0, amountAsNative),
    // @binding provider:Amount.asIOU
    JS_CFUNC_DEF("asIOU", 0, amountAsIou),
    // @binding provider:Amount.asMPT
    JS_CFUNC_DEF("asMPT", 0, amountAsMpt),
    // @binding provider:Amount.equals
    JS_CFUNC_DEF("equals", 1, amountEquals),
    // @binding provider:Amount.compare
    JS_CFUNC_DEF("compare", 1, amountCompare),
};

JSCFunctionListEntry const amountFactory[] = {
    JS_CFUNC_DEF("drops", 1, amountFactoryDrops),
};

} // namespace

bool registerAmount(JSContext *ctx, AmountLeafMaterializers const &leaves) {
  if (leaves.accountID == nullptr || leaves.currency == nullptr ||
      leaves.hash192 == nullptr || leaves.decimal == nullptr ||
      leaves.issue == nullptr)
    return false;
  leafMaterializers = leaves;
  return registerHiddenClass(ctx, &amountClassId, &amountClass, amountPrototype,
                             qjs::ByteClassFamily::serializedType,
                             amountToBytes);
}

bool publishAmountFactory(JSContext *ctx, JSValueConst global) {
  qjs::OwnedValue factory(ctx, JS_NewObject(ctx));
  if (factory.isException() ||
      !qjs::installFunctions(ctx, factory.get(), amountFactory) ||
      !installRuntimeTypeClassifier(ctx, factory.get(), RuntimeTypeId::amount) ||
      !qjs::freezeObject(ctx, factory.get()))
    return false;
  return JS_SetPropertyStr(ctx, global, "Amount", factory.release()) >= 0;
}

JSValue makeAmountBytes(JSContext *ctx, std::uint8_t const *bytes,
                        std::uint32_t length) {
  return newAmount(ctx, JS_UNDEFINED, bytes, length);
}

JSValue makeAmountView(JSContext *ctx, JSValueConst owner,
                       std::uint8_t const *bytes, std::uint32_t length) {
  return newAmount(ctx, owner, bytes, length);
}

bool isAmount(JSValueConst value) noexcept {
  return JS_IsObject(value) && JS_GetClassID(value) == amountClassId &&
         JS_GetOpaque(value, amountClassId) != nullptr;
}

bool
isAmountKind(JSValueConst value, AmountIssueKind expected) noexcept
{
    if (!JS_IsObject(value) || JS_GetClassID(value) != amountClassId)
        return false;
    auto const* state =
        static_cast<AmountState const*>(JS_GetOpaque(value, amountClassId));
    if (state == nullptr || state->data == nullptr)
        return false;
    using Kind = xdata::AmountRules::Kind;
    Kind const actual = xdata::AmountRules::kind(payload(*state));
    switch (expected)
    {
        case AmountIssueKind::native:
            return actual == Kind::Native;
        case AmountIssueKind::iou:
            return actual == Kind::Iou;
        case AmountIssueKind::mpt:
            return actual == Kind::Mpt;
    }
    return false;
}

bool detail::readAmountNominalPayload(JSValueConst input,
                                      NominalPayloadView &output) noexcept {
  output = {};
  if (!JS_IsObject(input) || JS_GetClassID(input) != amountClassId)
    return false;
  auto const *state =
      static_cast<AmountState const *>(JS_GetOpaque(input, amountClassId));
  if (state == nullptr || (state->length != 8 && state->length != 33 &&
                           state->length != 48))
    return false;
  output = {state->data, state->length};
  return true;
}

} // namespace jshookz::provider::types
