#include "amount/amount_js.hpp"
#include "pathset/pathset_js.hpp"
#include "quickjs.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <span>

namespace types = jshookz::provider::types;
namespace qjs = jshookz::provider::qjs;

namespace {

JSValue bytesLeaf(JSContext *ctx, std::uint8_t const *bytes,
                  std::uint32_t length) {
  return qjs::uint8Array(ctx, {bytes, length});
}

JSValue decimalLeaf(JSContext *ctx, bool negative, std::uint64_t magnitude,
                    std::int32_t) {
  if (negative && magnitude != 0 && magnitude <= 0x7fffffffffffffffULL)
    return JS_NewBigInt64(ctx, -static_cast<std::int64_t>(magnitude));
  return JS_NewBigUint64(ctx, magnitude);
}

JSValue issueLeaf(JSContext *ctx, types::AmountIssueKind kind,
                  std::uint8_t const *, std::uint32_t) {
  return JS_NewUint32(ctx, static_cast<std::uint8_t>(kind));
}

bool certifiedRange(JSContext *ctx, JSValueConst owner,
                    std::uint8_t const *bytes, std::uint32_t length) noexcept {
  std::size_t size = 0;
  std::uint8_t const *begin = JS_GetArrayBuffer(ctx, &size, owner);
  return begin != nullptr && bytes >= begin &&
         static_cast<std::size_t>(bytes - begin) <= size &&
         length <= size - static_cast<std::size_t>(bytes - begin);
}

bool setGlobal(JSContext *ctx, char const *name, JSValue value) {
  qjs::OwnedValue global(ctx, JS_GetGlobalObject(ctx));
  return !global.isException() &&
         JS_SetPropertyStr(ctx, global.get(), name, value) >= 0;
}

void clearException(JSContext *ctx) {
  if (JS_HasException(ctx))
    JS_FreeValue(ctx, JS_GetException(ctx));
}

int hexNibble(char value) noexcept {
  if (value >= '0' && value <= '9')
    return value - '0';
  if (value >= 'A' && value <= 'F')
    return value - 'A' + 10;
  if (value >= 'a' && value <= 'f')
    return value - 'a' + 10;
  return -1;
}

bool decodeHex(char const *text, std::uint8_t *output,
               std::size_t size) noexcept {
  if (std::strlen(text) != size * 2)
    return false;
  for (std::size_t index = 0; index < size; ++index) {
    int const high = hexNibble(text[index * 2]);
    int const low = hexNibble(text[index * 2 + 1]);
    if (high < 0 || low < 0)
      return false;
    output[index] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return true;
}

JSValue makeOwnedPathSet(JSContext *ctx, std::uint8_t const *bytes,
                         std::uint32_t length) {
  JSValue owner = JS_NewArrayBufferCopy(ctx, bytes, length);
  if (JS_IsException(owner))
    return owner;
  std::size_t ownerLength = 0;
  std::uint8_t const *ownerBytes = JS_GetArrayBuffer(ctx, &ownerLength, owner);
  JSValue value = types::makePathSetBytes(
      ctx, owner, ownerBytes, static_cast<std::uint32_t>(ownerLength));
  JS_FreeValue(ctx, owner);
  return value;
}

bool pathSetMaskMatrix(JSContext *ctx) {
  for (std::uint8_t const mask : {0x01, 0x10, 0x20, 0x11, 0x21, 0x30, 0x31}) {
    std::array<std::uint8_t, 62> bytes{};
    bytes[0] = mask;
    std::uint32_t length = 1;
    for (std::uint8_t const component : {0x01, 0x10, 0x20}) {
      if ((mask & component) == 0)
        continue;
      std::memset(bytes.data() + length, component, 20);
      length += 20;
    }
    bytes[length++] = 0;
    qjs::OwnedValue value(ctx, makeOwnedPathSet(ctx, bytes.data(), length));
    if (value.isException())
      return false;
    qjs::OwnedValue count(ctx, JS_GetPropertyStr(ctx, value.get(), "length"));
    std::uint32_t paths = 0;
    if (count.isException() || JS_ToUint32(ctx, &paths, count.get()) < 0 ||
        paths != 1)
      return false;
  }

  std::uint8_t const malformed[][3] = {
      {0x00, 0x00, 0x00},
      {0xff, 0x00, 0x00},
      {0x02, 0x00, 0x00},
      {0x01, 0x00, 0x00},
  };
  std::uint32_t const lengths[] = {1, 2, 2, 1};
  for (std::size_t index = 0; index < std::size(malformed); ++index) {
    JSValue value = makeOwnedPathSet(ctx, malformed[index], lengths[index]);
    if (!JS_IsException(value) || !JS_HasException(ctx))
      return false;
    clearException(ctx);
  }
  return true;
}

void printException(JSContext *ctx) {
  qjs::OwnedValue exception(ctx, JS_GetException(ctx));
  qjs::OwnedValue rendered(ctx, JS_ToString(ctx, exception.get()));
  char const *text = JS_ToCString(ctx, rendered.get());
  if (text != nullptr) {
    std::fprintf(stderr, "%s\n", text);
    JS_FreeCString(ctx, text);
  }
}

} // namespace

int main() {
  JSRuntime *runtime = JS_NewRuntime();
  JSContext *ctx = runtime == nullptr ? nullptr : JS_NewContext(runtime);
  if (ctx == nullptr)
    return 1;

  qjs::resetByteClassRegistry();
  qjs::OwnedValue global(ctx, JS_GetGlobalObject(ctx));
  types::AmountLeafMaterializers const amountLeaves{
      bytesLeaf, bytesLeaf, bytesLeaf, decimalLeaf, issueLeaf,
  };
  types::PathSetLeafMaterializers const pathLeaves{bytesLeaf, bytesLeaf,
                                                   certifiedRange};
  if (!types::registerAmount(ctx, amountLeaves) ||
      !types::registerPathSet(ctx, pathLeaves)) {
    printException(ctx);
    return 2;
  }

  std::uint8_t const nativeOne[8] = {0x40, 0, 0, 0, 0, 0, 0, 1};
  JSValue amount = types::makeAmountBytes(ctx, nativeOne, sizeof(nativeOne));
  JSValue amountCopy =
      types::makeAmountBytes(ctx, nativeOne, sizeof(nativeOne));
  if (JS_IsException(amount) || JS_IsException(amountCopy) ||
      !setGlobal(ctx, "amount", amount) ||
      !setGlobal(ctx, "amountCopy", amountCopy))
    return 3;

  std::array<std::uint8_t, 48> iou{};
  std::array<std::uint8_t, 33> mpt{};
  if (!decodeHex("C0438D7EA4C680000000000000000000000000005553440000000000"
                 "B5F762798A53D543A014CAF8B297CFF8F2F937E8",
                 iou.data(), iou.size()) ||
      !decodeHex(
          "200000000000000001000000000000000000000000000000000000000000000000",
          mpt.data(), mpt.size()))
    return 7;
  JSValue iouValue = types::makeAmountBytes(ctx, iou.data(), iou.size());
  JSValue mptValue = types::makeAmountBytes(ctx, mpt.data(), mpt.size());
  if (JS_IsException(iouValue) || JS_IsException(mptValue) ||
      !setGlobal(ctx, "iouAmount", iouValue) ||
      !setGlobal(ctx, "mptAmount", mptValue))
    return 8;

  std::array<std::uint8_t, 22> pathBytes{};
  pathBytes[0] = 0x01;
  std::memset(pathBytes.data() + 1, 0x5a, 20);
  pathBytes[21] = 0x00;
  JSValue owner =
      JS_NewArrayBufferCopy(ctx, pathBytes.data(), pathBytes.size());
  std::size_t ownerLength = 0;
  std::uint8_t const *ownerBytes = JS_GetArrayBuffer(ctx, &ownerLength, owner);
  JSValue paths = types::makePathSetBytes(
      ctx, owner, ownerBytes, static_cast<std::uint32_t>(ownerLength));
  JS_FreeValue(ctx, owner);
  if (JS_IsException(paths) || !setGlobal(ctx, "paths", paths))
    return 4;

  if (!pathSetMaskMatrix(ctx))
    return 9;

  std::uint8_t invalidAmount[8] = {};
  JSValue invalid =
      types::makeAmountBytes(ctx, invalidAmount, sizeof(invalidAmount));
  if (!JS_IsException(invalid) || !JS_HasException(ctx))
    return 5;
  clearException(ctx);

  char const script[] = R"JS(
        (() => {
          if (Object.hasOwn(globalThis, "Amount") ||
              Object.hasOwn(globalThis, "PathSet")) return false;
          if (amount.kind !== "native" || amount.drops !== 1n ||
              amount.byteLength !== 8 || amount.toString() !== "1" ||
              !amount.isNative() || amount.isIOU() ||
              amount.asNative() !== amount || amount.asIOU() !== undefined ||
              Object.isExtensible(amount)) return false;
          if (!amountCopy.equals(amount) || amountCopy.compare(amount) !== 0)
            return false;
          const amountBytes = amount.toBytes();
          amountBytes[7] = 9;
          if (amount.toBytes()[7] !== 1) return false;
          if (iouAmount.kind !== "iou" || iouAmount.byteLength !== 48 ||
              iouAmount.value !== 1000000000000000n ||
              mptAmount.kind !== "mpt" || mptAmount.value !== -1n ||
              mptAmount.byteLength !== 33) return false;

          if (paths.length !== 1 || paths.at(1) !== undefined ||
              Object.isExtensible(paths) ||
              !Object.isFrozen(Object.getPrototypeOf(paths))) return false;
          const path = paths.at(0);
          if (path.length !== 1 || path.at(1) !== undefined ||
              path === paths.at(0) || Object.isExtensible(path)) return false;
          const hop = path.at(0);
          if (hop === path.at(0) || hop.account.length !== 20 ||
              hop.account[0] !== 0x5a || hop.currency !== undefined ||
              hop.issuer !== undefined || Object.isExtensible(hop) ||
              hop.account === hop.account) return false;
          const exported = paths.toBytes();
          exported[0] = 0;
          if (paths.toBytes()[0] !== 1) return false;
          return [...paths].length === 1 && [...path].length === 1;
        })()
    )JS";
  qjs::OwnedValue result(ctx,
                         JS_Eval(ctx, script, sizeof(script) - 1,
                                 "leaf-slice-probe.js", JS_EVAL_TYPE_GLOBAL));
  int const passed = result.isException() ? -1 : JS_ToBool(ctx, result.get());
  if (passed < 0)
    printException(ctx);

  global = qjs::OwnedValue(ctx);
  JS_FreeContext(ctx);
  JS_FreeRuntime(runtime);
  return passed == 1 ? 0 : 6;
}
