#include "amount/amount_js.hpp"

#include "js.hpp"

#include <catl/core/types.h>
#include <catl/xdata/amount-rules.h>

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>

namespace jshookz::provider::types {
namespace {

namespace qjs = jshookz::provider::qjs;
namespace xdata = catl::xdata;

JSClassID amountClassId;
AmountLeafMaterializers leafMaterializers{};

struct AmountState {
  std::array<std::uint8_t, 48> bytes{};
  std::uint8_t length = 0;
};

[[nodiscard]] JSValue oom(JSContext *ctx) {
  return JS_HasException(ctx) ? JS_EXCEPTION : JS_ThrowOutOfMemory(ctx);
}

void amountFinalizer(JSRuntime *runtime, JSValue value) {
  auto *state = static_cast<AmountState *>(JS_GetOpaque(value, amountClassId));
  if (state != nullptr)
    js_free_rt(runtime, state);
}

JSClassDef const amountClass = {
    .class_name = "Amount",
    .finalizer = amountFinalizer,
    .gc_mark = nullptr,
    .call = nullptr,
    .exotic = nullptr,
};

[[nodiscard]] AmountState *amountState(JSContext *ctx, JSValueConst value) {
  return static_cast<AmountState *>(JS_GetOpaque2(ctx, value, amountClassId));
}

[[nodiscard]] Slice payload(AmountState const &state) noexcept {
  return {state.bytes.data(), state.length};
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

[[nodiscard]] JSValue newAmount(JSContext *ctx, std::uint8_t const *bytes,
                                std::uint32_t length) {
  if ((length != 8 && length != 33 && length != 48) || bytes == nullptr)
    return JS_ThrowTypeError(ctx, "Amount: invalid canonical byte length");
  Slice const wire{bytes, length};
  if (char const *error = xdata::AmountRules::certify(wire))
    return JS_ThrowTypeError(ctx, "Amount: %s", error);

  auto *state =
      static_cast<AmountState *>(js_mallocz(ctx, sizeof(AmountState)));
  if (state == nullptr)
    return oom(ctx);
  std::memcpy(state->bytes.data(), bytes, length);
  state->length = static_cast<std::uint8_t>(length);

  JSValue value = JS_NewObjectClass(ctx, amountClassId);
  if (JS_IsException(value)) {
    js_free(ctx, state);
    return value;
  }
  JS_SetOpaque(value, state);
  if (JS_PreventExtensions(ctx, value) < 0) {
    JS_FreeValue(ctx, value);
    return JS_EXCEPTION;
  }
  return value;
}

[[nodiscard]] JSValue amountFrom(JSContext *ctx, JSValueConst, int argc,
                                 JSValueConst *argv) {
  if (argc < 1)
    return JS_ThrowTypeError(ctx, "Amount.from() expects canonical bytes");
  auto bytes = qjs::ByteView::getBinding(ctx, argv[0], "Amount.from", 0,
                                         qjs::BytePolicy::bytesLikeOrSTBlob);
  if (!bytes)
    return qjs::byteInputTypeError(ctx, "Amount.from()",
                                   qjs::BytePolicy::bytesLikeOrSTBlob);
  return newAmount(ctx, bytes.data(), bytes.size());
}

void writeBigEndian(std::uint8_t *output, std::uint64_t value) noexcept {
  for (int index = 7; index >= 0; --index) {
    output[index] = static_cast<std::uint8_t>(value);
    value >>= 8;
  }
}

[[nodiscard]] bool readSignedInteger(JSContext *ctx, JSValueConst input,
                                     std::int64_t &output) {
  if (JS_IsBigInt(ctx, input))
    return JS_ToBigInt64(ctx, &output, input) >= 0;
  double number = 0;
  if (!JS_IsNumber(input) || JS_ToFloat64(ctx, &number, input) < 0 ||
      !std::isfinite(number) || std::trunc(number) != number ||
      number < -9007199254740991.0 || number > 9007199254740991.0 ||
      JS_ToInt64(ctx, &output, input) < 0)
    return false;
  return true;
}

[[nodiscard]] JSValue amountDropsFactory(JSContext *ctx, JSValueConst, int argc,
                                         JSValueConst *argv) {
  std::int64_t signedValue = 0;
  if (argc < 1 || !readSignedInteger(ctx, argv[0], signedValue))
    return JS_HasException(ctx)
               ? JS_EXCEPTION
               : JS_ThrowTypeError(ctx,
                                   "Amount.drops() expects an exact integer");
  std::uint64_t const magnitude =
      signedValue < 0
          ? std::uint64_t{0} - static_cast<std::uint64_t>(signedValue)
          : static_cast<std::uint64_t>(signedValue);
  if (magnitude >= xdata::AmountRules::kMpt)
    return JS_ThrowRangeError(ctx, "Amount.drops() is out of range");
  std::uint64_t const wire = magnitude == 0 || signedValue > 0
                                 ? magnitude | xdata::AmountRules::kPositive
                                 : magnitude;
  std::uint8_t bytes[8];
  writeBigEndian(bytes, wire);
  return newAmount(ctx, bytes, sizeof(bytes));
}

[[nodiscard]] JSValue amountIouFactory(JSContext *ctx, JSValueConst, int argc,
                                       JSValueConst *argv) {
  if (argc < 3)
    return JS_ThrowTypeError(
        ctx, "Amount.iou() expects decimal, currency, and issuer");
  bool negative = false;
  std::uint64_t magnitude = 0;
  std::int32_t exponent = 0;
  std::uint8_t currency[20];
  std::uint8_t issuer[20];
  if (!leafMaterializers.readDecimal(ctx, argv[0], &negative, &magnitude,
                                     &exponent) ||
      !leafMaterializers.readCurrency(ctx, argv[1], currency) ||
      !leafMaterializers.readAccountID(ctx, argv[2], issuer))
    return JS_HasException(ctx)
               ? JS_EXCEPTION
               : JS_ThrowTypeError(
                     ctx, "Amount.iou() received a wrong nominal value");

  std::uint64_t wire = xdata::AmountRules::kIssued;
  if (magnitude != 0) {
    if (magnitude < xdata::AmountRules::kMinMant ||
        magnitude > xdata::AmountRules::kMaxMant || exponent < -96 ||
        exponent > 80)
      return JS_ThrowRangeError(ctx, "Amount.iou() decimal is out of range");
    std::uint64_t const high =
        std::uint64_t{static_cast<std::uint32_t>(exponent + 97)} |
        (negative ? 0 : 0x100u) | 0x200u;
    wire = (high << 54) | magnitude;
  }
  std::uint8_t bytes[48];
  writeBigEndian(bytes, wire);
  std::memcpy(bytes + 8, currency, sizeof(currency));
  std::memcpy(bytes + 28, issuer, sizeof(issuer));
  return newAmount(ctx, bytes, sizeof(bytes));
}

[[nodiscard]] JSValue amountMptFactory(JSContext *ctx, JSValueConst, int argc,
                                       JSValueConst *argv) {
  std::int64_t signedValue = 0;
  std::uint8_t issuance[24];
  if (argc < 2 || !readSignedInteger(ctx, argv[0], signedValue) ||
      !leafMaterializers.readHash192(ctx, argv[1], issuance))
    return JS_HasException(ctx)
               ? JS_EXCEPTION
               : JS_ThrowTypeError(
                     ctx, "Amount.mpt() expects an exact integer and Hash192");
  std::uint64_t const magnitude =
      signedValue < 0
          ? std::uint64_t{0} - static_cast<std::uint64_t>(signedValue)
          : static_cast<std::uint64_t>(signedValue);
  std::uint8_t bytes[33] = {};
  bytes[0] = 0x20;
  if (signedValue >= 0)
    bytes[0] |= 0x40;
  writeBigEndian(bytes + 1, magnitude);
  std::memcpy(bytes + 9, issuance, sizeof(issuance));
  return newAmount(ctx, bytes, sizeof(bytes));
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
  return state == nullptr
             ? JS_EXCEPTION
             : qjs::uint8Array(ctx, {state->bytes.data(), state->length});
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
             ? leafMaterializers.currency(ctx, value.currency.data(),
                                          value.currency.size())
             : JS_UNDEFINED;
}

[[nodiscard]] JSValue amountIssuer(JSContext *ctx, JSValueConst thisValue) {
  auto const *state = amountState(ctx, thisValue);
  if (state == nullptr)
    return JS_EXCEPTION;
  auto const value = parts(*state);
  return value.kind == xdata::AmountRules::Kind::Iou
             ? leafMaterializers.accountID(ctx, value.issuer.data(),
                                           value.issuer.size())
             : JS_UNDEFINED;
}

[[nodiscard]] JSValue amountMptId(JSContext *ctx, JSValueConst thisValue) {
  auto const *state = amountState(ctx, thisValue);
  if (state == nullptr)
    return JS_EXCEPTION;
  auto const value = parts(*state);
  return value.kind == xdata::AmountRules::Kind::Mpt
             ? leafMaterializers.hash192(ctx, value.mpt_id.data(),
                                         value.mpt_id.size())
             : JS_UNDEFINED;
}

[[nodiscard]] JSValue amountIssue(JSContext *ctx, JSValueConst thisValue) {
  auto const *state = amountState(ctx, thisValue);
  if (state == nullptr)
    return JS_EXCEPTION;
  auto const value = parts(*state);
  using Kind = xdata::AmountRules::Kind;
  if (value.kind == Kind::Native)
    return leafMaterializers.issue(ctx, AmountIssueKind::native, nullptr, 0);
  if (value.kind == Kind::Iou)
    return leafMaterializers.issue(ctx, AmountIssueKind::iou,
                                   state->bytes.data() + 8, 40);
  return leafMaterializers.issue(ctx, AmountIssueKind::mpt,
                                 state->bytes.data() + 9, 24);
}

[[nodiscard]] bool sameIssue(AmountState const &left, AmountState const &right,
                             xdata::AmountRules::Kind kind) noexcept {
  if (kind == xdata::AmountRules::Kind::Native)
    return true;
  std::size_t const offset = kind == xdata::AmountRules::Kind::Iou ? 8 : 9;
  std::size_t const length = kind == xdata::AmountRules::Kind::Iou ? 40 : 24;
  return std::memcmp(left.bytes.data() + offset, right.bytes.data() + offset,
                     length) == 0;
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
    JS_CGETSET_DEF("kind", amountKind, nullptr),
    JS_CGETSET_DEF("issue", amountIssue, nullptr),
    JS_CGETSET_DEF("currency", amountCurrency, nullptr),
    JS_CGETSET_DEF("issuer", amountIssuer, nullptr),
    JS_CGETSET_DEF("mptIssuanceId", amountMptId, nullptr),
    JS_CGETSET_DEF("value", amountValue, nullptr),
    JS_CGETSET_DEF("drops", amountDrops, nullptr),
    JS_CGETSET_DEF("byteLength", amountByteLength, nullptr),
    JS_CFUNC_DEF("toBytes", 0, amountToBytes),
    JS_CFUNC_DEF("toXFL", 0, amountToXfl),
    JS_CFUNC_DEF("toString", 0, amountToString),
    JS_CFUNC_DEF("isNative", 0, amountIsNative),
    JS_CFUNC_DEF("isIOU", 0, amountIsIou),
    JS_CFUNC_DEF("isMPT", 0, amountIsMpt),
    JS_CFUNC_DEF("asNative", 0, amountAsNative),
    JS_CFUNC_DEF("asIOU", 0, amountAsIou),
    JS_CFUNC_DEF("asMPT", 0, amountAsMpt),
    JS_CFUNC_DEF("equals", 1, amountEquals),
    JS_CFUNC_DEF("compare", 1, amountCompare),
};

JSCFunctionListEntry const amountStatics[] = {
    JS_CFUNC_DEF("from", 1, amountFrom),
    JS_CFUNC_DEF("drops", 1, amountDropsFactory),
    JS_CFUNC_DEF("iou", 3, amountIouFactory),
    JS_CFUNC_DEF("mpt", 2, amountMptFactory),
};

} // namespace

bool registerAmount(JSContext *ctx, JSValueConst global,
                    AmountLeafMaterializers const &leaves) {
  if (leaves.accountID == nullptr || leaves.currency == nullptr ||
      leaves.hash192 == nullptr || leaves.decimal == nullptr ||
      leaves.issue == nullptr || leaves.readAccountID == nullptr ||
      leaves.readCurrency == nullptr || leaves.readHash192 == nullptr ||
      leaves.readDecimal == nullptr)
    return false;
  leafMaterializers = leaves;
  return registerClass(ctx, global, "Amount", &amountClassId, &amountClass,
                       amountPrototype, amountStatics,
                       qjs::ByteClassFamily::serializedType, amountToBytes);
}

JSValue makeAmountBytes(JSContext *ctx, std::uint8_t const *bytes,
                        std::uint32_t length) {
  return newAmount(ctx, bytes, length);
}

bool isAmount(JSValueConst value) noexcept {
  return JS_IsObject(value) && JS_GetClassID(value) == amountClassId &&
         JS_GetOpaque(value, amountClassId) != nullptr;
}

} // namespace jshookz::provider::types
