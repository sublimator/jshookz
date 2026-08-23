#include "object/canonical_json.hpp"

#include "account/account_json.hpp"

#include <catl/core/types.h>
#include <catl/xdata/amount-rules.h>
#include <catl/xdata/parser-context.h>
#include <catl/xdata/pathset-rules.h>
#include <catl/xdata/types/issue.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace jshookz::provider::types {
namespace {

namespace xdata = catl::xdata;

constexpr int jsonPropertyFlags =
    JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE | JS_PROP_ENUMERABLE;

class LocalValue {
public:
  explicit LocalValue(JSContext *ctx, JSValue value = JS_UNDEFINED) noexcept
      : ctx_(ctx), value_(value) {}

  ~LocalValue() { JS_FreeValue(ctx_, value_); }

  LocalValue(LocalValue const &) = delete;
  LocalValue &operator=(LocalValue const &) = delete;

  [[nodiscard]] JSValueConst get() const noexcept { return value_; }

  [[nodiscard]] bool isException() const noexcept {
    return JS_IsException(value_);
  }

  [[nodiscard]] JSValue release() noexcept {
    JSValue value = value_;
    value_ = JS_UNDEFINED;
    return value;
  }

  void reset(JSValue value = JS_UNDEFINED) noexcept {
    JS_FreeValue(ctx_, value_);
    value_ = value;
  }

private:
  JSContext *ctx_;
  JSValue value_;
};

[[nodiscard]] JSValue internalError(JSContext *ctx,
                                    char const *message) noexcept {
  return JS_HasException(ctx) ? JS_EXCEPTION
                              : JS_ThrowInternalError(ctx, "%s", message);
}

[[nodiscard]] bool inputAvailable(std::uint8_t const *bytes,
                                  std::uint32_t length) noexcept {
  return length == 0 || bytes != nullptr;
}

[[nodiscard]] bool defineProperty(JSContext *ctx, JSValueConst object,
                                  char const *key, LocalValue &value) noexcept {
  return JS_DefinePropertyValueStr(ctx, object, key, value.release(),
                                   jsonPropertyFlags) >= 0;
}

[[nodiscard]] bool defineElement(JSContext *ctx, JSValueConst array,
                                 std::uint32_t index,
                                 LocalValue &value) noexcept {
  return JS_DefinePropertyValueUint32(ctx, array, index, value.release(),
                                      jsonPropertyFlags) >= 0;
}

[[nodiscard]] bool allZero(std::uint8_t const *bytes,
                           std::uint32_t length) noexcept {
  for (std::uint32_t index = 0; index < length; ++index) {
    if (bytes[index] != 0)
      return false;
  }
  return true;
}

[[nodiscard]] bool isNoAccount(std::uint8_t const *bytes) noexcept {
  for (std::uint32_t index = 0; index < 19; ++index) {
    if (bytes[index] != 0)
      return false;
  }
  return bytes[19] == 1;
}

[[nodiscard]] bool isISOCodeCharacter(std::uint8_t value) noexcept {
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
         (value >= '0' && value <= '9') || value == '<' || value == '>' ||
         value == '(' || value == ')' || value == '{' || value == '}' ||
         value == '[' || value == ']' || value == '|' || value == '?' ||
         value == '!' || value == '@' || value == '#' || value == '$' ||
         value == '%' || value == '^' || value == '&' || value == '*';
}

[[nodiscard]] JSValue hexString(JSContext *ctx, std::uint8_t const *bytes,
                                std::uint32_t length) noexcept {
  if (!inputAvailable(bytes, length) || length > 32)
    return internalError(ctx, "canonical JSON hex extent is invalid");
  constexpr char digits[] = "0123456789ABCDEF";
  char output[64];
  for (std::uint32_t index = 0; index < length; ++index) {
    output[index * 2] = digits[bytes[index] >> 4];
    output[index * 2 + 1] = digits[bytes[index] & 15];
  }
  return JS_NewStringLen(ctx, output, std::size_t{length} * 2);
}

[[nodiscard]] std::uint32_t decimalDigits(std::uint64_t value,
                                          char output[20]) noexcept {
  char reverse[20];
  std::uint32_t count = 0;
  do {
    reverse[count++] = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value != 0);
  for (std::uint32_t index = 0; index < count; ++index)
    output[index] = reverse[count - index - 1];
  return count;
}

[[nodiscard]] JSValue
amountText(JSContext *ctx, xdata::AmountRules::Parts const &parts) noexcept {
  if (parts.magnitude == 0)
    return JS_NewStringLen(ctx, "0", 1);

  char raw[20];
  std::uint32_t const rawLength = decimalDigits(parts.magnitude, raw);
  char output[128];
  std::uint32_t position = 0;
  if (parts.negative)
    output[position++] = '-';
  auto append = [&](char const *source, std::uint32_t count) noexcept {
    std::memcpy(output + position, source, count);
    position += count;
  };

  using Kind = xdata::AmountRules::Kind;
  if (parts.kind == Kind::Native || parts.kind == Kind::Mpt) {
    append(raw, rawLength);
    return JS_NewStringLen(ctx, output, position);
  }

  bool const scientific =
      parts.exponent != 0 && (parts.exponent < -25 || parts.exponent > -5);
  if (scientific) {
    append(raw, rawLength);
    output[position++] = 'e';
    std::int32_t exponent = parts.exponent;
    if (exponent < 0) {
      output[position++] = '-';
      exponent = -exponent;
    }
    char exponentDigits[20];
    auto const exponentLength =
        decimalDigits(static_cast<std::uint64_t>(exponent), exponentDigits);
    append(exponentDigits, exponentLength);
    return JS_NewStringLen(ctx, output, position);
  }

  std::int32_t const decimalPosition =
      static_cast<std::int32_t>(rawLength) + parts.exponent;
  if (decimalPosition <= 0) {
    output[position++] = '0';
    output[position++] = '.';
    for (std::int32_t index = 0; index < -decimalPosition; ++index)
      output[position++] = '0';
    append(raw, rawLength);
  } else if (decimalPosition < static_cast<std::int32_t>(rawLength)) {
    append(raw, static_cast<std::uint32_t>(decimalPosition));
    output[position++] = '.';
    append(raw + decimalPosition,
           rawLength - static_cast<std::uint32_t>(decimalPosition));
  } else {
    append(raw, rawLength);
    for (std::int32_t index = static_cast<std::int32_t>(rawLength);
         index < decimalPosition; ++index)
      output[position++] = '0';
  }

  char *const begin = output + (parts.negative ? 1 : 0);
  char *end = output + position;
  char *dot = nullptr;
  for (char *cursor = begin; cursor < end; ++cursor) {
    if (*cursor == '.') {
      dot = cursor;
      break;
    }
  }
  if (dot != nullptr) {
    while (end > dot + 1 && end[-1] == '0')
      --end;
    if (end == dot + 1)
      end = dot;
    position = static_cast<std::uint32_t>(end - output);
  }
  return JS_NewStringLen(ctx, output, position);
}

[[nodiscard]] bool validIssue(std::uint8_t const *bytes,
                              std::uint32_t length) noexcept {
  if (bytes == nullptr)
    return false;
  if (length == 20)
    return allZero(bytes, 20);
  if (length == 44)
    return !allZero(bytes, 20) && isNoAccount(bytes + 20);
  if (length != 40)
    return false;
  return !allZero(bytes, 20) && !allZero(bytes + 20, 20) &&
         !isNoAccount(bytes + 20);
}

[[nodiscard]] bool issueExtent(std::uint8_t const *bytes,
                               std::uint32_t remaining,
                               std::uint32_t &length) noexcept {
  if (bytes == nullptr || remaining < 20)
    return false;
  if (allZero(bytes, 20)) {
    length = 20;
    return true;
  }
  if (remaining < 40)
    return false;
  if (isNoAccount(bytes + 20)) {
    if (remaining < 44)
      return false;
    length = 44;
    return true;
  }
  length = 40;
  return true;
}

struct ByteRange {
  std::uint8_t const *bytes = nullptr;
  std::uint32_t length = 0;
};

struct BridgeParts {
  ByteRange lockingDoor;
  ByteRange lockingIssue;
  ByteRange issuingDoor;
  ByteRange issuingIssue;
};

[[nodiscard]] bool parseBridge(std::uint8_t const *bytes, std::uint32_t length,
                               BridgeParts &output) noexcept {
  output = {};
  if (!inputAvailable(bytes, length) ||
      length > canonicalJSONMaximumPayloadBytes)
    return false;
  std::uint32_t position = 0;
  ByteRange *doors[2] = {&output.lockingDoor, &output.issuingDoor};
  ByteRange *issues[2] = {&output.lockingIssue, &output.issuingIssue};
  for (std::uint32_t side = 0; side < 2; ++side) {
    if (position >= length)
      return false;
    std::uint32_t const doorLength = bytes[position++];
    if (doorLength != 0 && doorLength != 20)
      return false;
    if (doorLength > length - position)
      return false;
    *doors[side] = {bytes + position, doorLength};
    position += doorLength;

    std::uint32_t issueLength = 0;
    if (!issueExtent(bytes + position, length - position, issueLength) ||
        !validIssue(bytes + position, issueLength))
      return false;
    *issues[side] = {bytes + position, issueLength};
    position += issueLength;
  }
  return position == length;
}

} // namespace

JSValue makeAccountIDCanonicalJSON(JSContext *ctx, std::uint8_t const *bytes,
                                   std::uint32_t length) noexcept {
  if (!inputAvailable(bytes, length) || (length != 0 && length != 20))
    return internalError(ctx, "canonical AccountID payload is invalid");
  if (length == 0)
    return JS_NewStringLen(ctx, "", 0);
  return makeAccountIDCanonicalJSONString(ctx, bytes, length);
}

JSValue makeCurrencyCanonicalJSON(JSContext *ctx, std::uint8_t const *bytes,
                                  std::uint32_t length) noexcept {
  if (bytes == nullptr || length != 20)
    return internalError(ctx, "canonical Currency payload is invalid");
  if (allZero(bytes, 20))
    return JS_NewStringLen(ctx, "XAH", 3);

  if (allZero(bytes, 19) && bytes[19] == 1)
    return JS_NewStringLen(ctx, "1", 1);

  bool standard = allZero(bytes, 12) && allZero(bytes + 15, 5);
  for (std::uint32_t index = 12; standard && index < 15; ++index)
    standard = isISOCodeCharacter(bytes[index]);
  standard =
      standard && !(bytes[12] == 'X' && bytes[13] == 'A' && bytes[14] == 'H');
  if (!standard)
    return hexString(ctx, bytes, length);
  return JS_NewStringLen(ctx, reinterpret_cast<char const *>(bytes + 12), 3);
}

JSValue makeAmountCanonicalJSON(JSContext *ctx, std::uint8_t const *bytes,
                                std::uint32_t length) noexcept {
  if (bytes == nullptr || length > canonicalJSONMaximumPayloadBytes ||
      xdata::AmountRules::certify(Slice{bytes, length}) != nullptr)
    return internalError(ctx, "canonical Amount payload is invalid");
  auto const parts = xdata::AmountRules::parts(Slice{bytes, length});
  using Kind = xdata::AmountRules::Kind;
  if (parts.kind == Kind::Native)
    return amountText(ctx, parts);

  LocalValue result(ctx, JS_NewObject(ctx));
  if (result.isException())
    return result.release();
  if (parts.kind == Kind::Mpt) {
    LocalValue id(ctx,
                  hexString(ctx, parts.mpt_id.data(),
                            static_cast<std::uint32_t>(parts.mpt_id.size())));
    LocalValue value(ctx, amountText(ctx, parts));
    if (id.isException() || value.isException() ||
        !defineProperty(ctx, result.get(), "mpt_issuance_id", id) ||
        !defineProperty(ctx, result.get(), "value", value))
      return JS_EXCEPTION;
    return result.release();
  }

  LocalValue currency(ctx,
                      makeCurrencyCanonicalJSON(
                          ctx, parts.currency.data(),
                          static_cast<std::uint32_t>(parts.currency.size())));
  LocalValue issuer(ctx, makeAccountIDCanonicalJSON(
                             ctx, parts.issuer.data(),
                             static_cast<std::uint32_t>(parts.issuer.size())));
  LocalValue value(ctx, amountText(ctx, parts));
  if (currency.isException() || issuer.isException() || value.isException() ||
      !defineProperty(ctx, result.get(), "currency", currency) ||
      !defineProperty(ctx, result.get(), "issuer", issuer) ||
      !defineProperty(ctx, result.get(), "value", value))
    return JS_EXCEPTION;
  return result.release();
}

JSValue makeIssueCanonicalJSON(JSContext *ctx, std::uint8_t const *bytes,
                               std::uint32_t length) noexcept {
  if (length > canonicalJSONMaximumPayloadBytes || !validIssue(bytes, length))
    return internalError(ctx, "canonical Issue payload is invalid");
  LocalValue result(ctx, JS_NewObject(ctx));
  if (result.isException())
    return result.release();

  if (length == 44) {
    std::uint8_t mptID[24];
    mptID[0] = bytes[43];
    mptID[1] = bytes[42];
    mptID[2] = bytes[41];
    mptID[3] = bytes[40];
    std::memcpy(mptID + 4, bytes, 20);
    LocalValue id(ctx, hexString(ctx, mptID, sizeof(mptID)));
    if (id.isException() ||
        !defineProperty(ctx, result.get(), "mpt_issuance_id", id))
      return JS_EXCEPTION;
    return result.release();
  }

  LocalValue currency(ctx, makeCurrencyCanonicalJSON(ctx, bytes, 20));
  if (currency.isException() ||
      !defineProperty(ctx, result.get(), "currency", currency))
    return JS_EXCEPTION;
  if (length == 40) {
    LocalValue issuer(ctx, makeAccountIDCanonicalJSON(ctx, bytes + 20, 20));
    if (issuer.isException() ||
        !defineProperty(ctx, result.get(), "issuer", issuer))
      return JS_EXCEPTION;
  }
  return result.release();
}

JSValue makeVector256CanonicalJSON(JSContext *ctx, std::uint8_t const *bytes,
                                   std::uint32_t length) noexcept {
  if (!inputAvailable(bytes, length) ||
      length > canonicalJSONMaximumPayloadBytes || length % 32 != 0 ||
      length / 32 > canonicalJSONMaximumNodes)
    return internalError(ctx, "canonical Vector256 payload is invalid");
  LocalValue result(ctx, JS_NewArray(ctx));
  if (result.isException())
    return result.release();
  std::uint32_t const count = length / 32;
  for (std::uint32_t index = 0; index < count; ++index) {
    LocalValue hash(ctx, hexString(ctx, bytes + index * 32, 32));
    if (hash.isException() || !defineElement(ctx, result.get(), index, hash))
      return JS_EXCEPTION;
  }
  return result.release();
}

JSValue makePathSetCanonicalJSON(JSContext *ctx, std::uint8_t const *bytes,
                                 std::uint32_t length) noexcept {
  if (bytes == nullptr || length == 0 ||
      length > canonicalJSONMaximumPayloadBytes)
    return internalError(ctx, "canonical PathSet payload is invalid");

  struct JsonSink {
    JSContext *ctx;
    LocalValue paths;
    LocalValue current;
    std::uint32_t pathCount = 0;
    std::uint32_t hopCount = 0;
    bool failed = false;

    explicit JsonSink(JSContext *context) noexcept
        : ctx(context), paths(context, JS_NewArray(context)), current(context) {
      failed = paths.isException();
    }

    [[nodiscard]] bool ensureCurrent() noexcept {
      if (!JS_IsUndefined(current.get()))
        return true;
      current.reset(JS_NewArray(ctx));
      if (current.isException())
        failed = true;
      return !failed;
    }

    void on_hop(xdata::PathSetHop const &hop) noexcept {
      if (failed)
        return;
      if (hopCount == canonicalJSONMaximumNodes || !ensureCurrent()) {
        failed = true;
        return;
      }
      LocalValue value(ctx, JS_NewObject(ctx));
      if (value.isException()) {
        failed = true;
        return;
      }
      if (!hop.account.empty()) {
        LocalValue account(ctx,
                           makeAccountIDCanonicalJSON(
                               ctx, hop.account.data(),
                               static_cast<std::uint32_t>(hop.account.size())));
        if (account.isException() ||
            !defineProperty(ctx, value.get(), "account", account)) {
          failed = true;
          return;
        }
      }
      if (!hop.currency.empty()) {
        LocalValue currency(
            ctx, makeCurrencyCanonicalJSON(
                     ctx, hop.currency.data(),
                     static_cast<std::uint32_t>(hop.currency.size())));
        if (currency.isException() ||
            !defineProperty(ctx, value.get(), "currency", currency)) {
          failed = true;
          return;
        }
      }
      if (!hop.issuer.empty()) {
        LocalValue issuer(ctx,
                          makeAccountIDCanonicalJSON(
                              ctx, hop.issuer.data(),
                              static_cast<std::uint32_t>(hop.issuer.size())));
        if (issuer.isException() ||
            !defineProperty(ctx, value.get(), "issuer", issuer)) {
          failed = true;
          return;
        }
      }
      LocalValue type(ctx, JS_NewInt32(ctx, hop.type));
      if (type.isException() ||
          !defineProperty(ctx, value.get(), "type", type)) {
        failed = true;
        return;
      }
      if (!defineElement(ctx, current.get(), hopCountInPath(), value)) {
        failed = true;
        return;
      }
      ++hopCount;
      ++currentHopCount;
    }

    void on_path_end() noexcept {
      if (failed)
        return;
      if (pathCount == canonicalJSONMaximumNodes ||
          JS_IsUndefined(current.get()) ||
          !defineElement(ctx, paths.get(), pathCount, current)) {
        failed = true;
        return;
      }
      ++pathCount;
      currentHopCount = 0;
    }

    void on_end() const noexcept {}

    [[nodiscard]] std::uint32_t hopCountInPath() const noexcept {
      return currentHopCount;
    }

    std::uint32_t currentHopCount = 0;
  } sink{ctx};

  if (sink.failed)
    return internalError(ctx, "canonical PathSet JSON construction failed");
  xdata::ParserContext parser{Slice{bytes, length}};
  bool const valid =
      xdata::PathSetRules::walk<xdata::PathSetRuleMode::CertifyWire>(parser,
                                                                     sink) &&
      !parser.failed() && parser.pos() == length;
  if (!valid)
    return internalError(ctx, "canonical PathSet payload is invalid");
  if (sink.failed)
    return internalError(ctx, "canonical PathSet JSON construction failed");
  return sink.paths.release();
}

JSValue makeXChainBridgeCanonicalJSON(JSContext *ctx, std::uint8_t const *bytes,
                                      std::uint32_t length) noexcept {
  BridgeParts parts;
  if (!parseBridge(bytes, length, parts))
    return internalError(ctx, "canonical XChainBridge payload is invalid");
  LocalValue result(ctx, JS_NewObject(ctx));
  if (result.isException())
    return result.release();

  // JsonCpp/xahaud emits object keys lexically. Preserve that observable
  // order even though the bridge wire stores the Locking side first.
  LocalValue issuingDoor(
      ctx, makeAccountIDCanonicalJSON(ctx, parts.issuingDoor.bytes,
                                      parts.issuingDoor.length));
  LocalValue issuingIssue(ctx,
                          makeIssueCanonicalJSON(ctx, parts.issuingIssue.bytes,
                                                 parts.issuingIssue.length));
  LocalValue lockingDoor(
      ctx, makeAccountIDCanonicalJSON(ctx, parts.lockingDoor.bytes,
                                      parts.lockingDoor.length));
  LocalValue lockingIssue(ctx,
                          makeIssueCanonicalJSON(ctx, parts.lockingIssue.bytes,
                                                 parts.lockingIssue.length));
  if (issuingDoor.isException() || issuingIssue.isException() ||
      lockingDoor.isException() || lockingIssue.isException() ||
      !defineProperty(ctx, result.get(), "IssuingChainDoor", issuingDoor) ||
      !defineProperty(ctx, result.get(), "IssuingChainIssue", issuingIssue) ||
      !defineProperty(ctx, result.get(), "LockingChainDoor", lockingDoor) ||
      !defineProperty(ctx, result.get(), "LockingChainIssue", lockingIssue))
    return JS_EXCEPTION;
  return result.release();
}

} // namespace jshookz::provider::types
