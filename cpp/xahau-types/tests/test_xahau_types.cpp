#include "amount/amount_js.hpp"
#include "js.hpp"
#include "leaf/leaf.hpp"
#include "object/field_js.hpp"
#include "object/object.hpp"
#include "pathset/pathset_js.hpp"
#include "result.hpp"

#include <jshookz/qjs.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

extern "C" bool register_cpp_types(JSContext *ctx);
extern "C" bool register_uint_types(JSContext *ctx);

namespace {

std::vector<std::uint8_t>
hexBytes(char const* text)
{
    auto nibble = [](char value) -> std::uint8_t {
        if (value >= '0' && value <= '9')
            return static_cast<std::uint8_t>(value - '0');
        if (value >= 'A' && value <= 'F')
            return static_cast<std::uint8_t>(value - 'A' + 10);
        return static_cast<std::uint8_t>(value - 'a' + 10);
    };
    std::size_t const length = std::strlen(text);
    std::vector<std::uint8_t> bytes(length / 2);
    for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<std::uint8_t>(
            (nibble(text[i * 2]) << 4) | nibble(text[i * 2 + 1]));
    return bytes;
}

JSValue
makeIssueForAmount(
    JSContext* ctx,
    jshookz::provider::types::AmountIssueKind kind,
    std::uint8_t const* identity,
    std::uint32_t length)
{
    namespace types = jshookz::provider::types;
    if (kind == types::AmountIssueKind::native) {
        std::uint8_t native[20] = {};
        return types::makeIssueBytes(ctx, native, sizeof(native));
    }
    if (kind == types::AmountIssueKind::iou)
        return types::makeIssueBytes(ctx, identity, length);
    if (identity == nullptr || length != 24)
        return JS_ThrowInternalError(ctx, "invalid certified MPT issue identity");
    std::uint8_t issue[44] = {};
    std::memcpy(issue, identity + 4, 20);
    issue[39] = 1;
    issue[40] = identity[3];
    issue[41] = identity[2];
    issue[42] = identity[1];
    issue[43] = identity[0];
    return types::makeIssueBytes(ctx, issue, sizeof(issue));
}

}  // namespace

class XahauTypes : public ::testing::Test
{
protected:
    JSRuntime *rt = nullptr;
    JSContext *ctx = nullptr;

    void
    SetUp() override
    {
        rt = JS_NewRuntime();
        ASSERT_NE(rt, nullptr);
        ctx = JS_NewContext(rt);
        ASSERT_NE(ctx, nullptr);
        ASSERT_TRUE(jshookz::provider::bindings::registerResult(ctx));
        ASSERT_TRUE(register_cpp_types(ctx));
        ASSERT_TRUE(register_uint_types(ctx));
        jshookz::qjs::OwnedValue global(ctx, JS_GetGlobalObject(ctx));
        ASSERT_FALSE(global.isException());
        jshookz::qjs::OwnedValue util(
            ctx, JS_GetPropertyStr(ctx, global.get(), "util"));
        ASSERT_FALSE(util.isException());
        if (JS_IsUndefined(util.get())) {
            ASSERT_TRUE(jshookz::provider::types::registerRichLeafTypes(ctx));
            namespace types = jshookz::provider::types;
            types::AmountLeafMaterializers const amountLeaves{
                types::makeAccountIDBytes,
                types::makeCurrencyBytes,
                types::makeHash192Bytes,
                types::makeXFLDecimalParts,
                makeIssueForAmount,
                types::readAccountIDBytes,
                types::readCurrencyBytes,
                types::readHash192Bytes,
                types::readXFLDecimalParts,
            };
            ASSERT_TRUE(types::registerAmount(
                ctx, global.get(), amountLeaves));
            ASSERT_TRUE(types::registerObjectTypes(ctx));
            types::PathSetLeafMaterializers const pathLeaves{
                types::makeAccountIDBytes,
                types::makeCurrencyBytes,
                types::isCertifiedObjectRange,
            };
            ASSERT_TRUE(types::registerPathSet(
                ctx, global.get(), pathLeaves));
            ASSERT_TRUE(types::registerFieldDescriptors(ctx, global.get()));
        } else
            ASSERT_TRUE(JS_IsObject(util.get()));
        ASSERT_FALSE(JS_HasException(ctx));
    }

    void
    TearDown() override
    {
        if (ctx)
            JS_FreeContext(ctx);
        if (rt) {
            jshookz::provider::types::unregisterObjectTypes(rt);
            jshookz::provider::types::unregisterRichLeafTypes(rt);
            JS_FreeRuntime(rt);
        }
    }

    jshookz::qjs::OwnedValue
    eval(char const *src)
    {
        return jshookz::qjs::OwnedValue(
            ctx, JS_Eval(ctx, src, std::strlen(src), "<test>", JS_EVAL_TYPE_GLOBAL));
    }

    std::string
    to_string(JSValueConst value)
    {
        char const *text = JS_ToCString(ctx, value);
        std::string out = text ? text : "";
        if (text)
            JS_FreeCString(ctx, text);
        return out;
    }

    void
    installRoot(std::vector<std::uint8_t> const& bytes)
    {
        jshookz::qjs::OwnedValue value(
            ctx,
            jshookz::provider::types::makeCertifiedObjectCopy(
                ctx, bytes.data(), static_cast<std::uint32_t>(bytes.size())));
        ASSERT_FALSE(value.isException());
        jshookz::qjs::OwnedValue global(ctx, JS_GetGlobalObject(ctx));
        ASSERT_FALSE(global.isException());
        ASSERT_GE(
            JS_SetPropertyStr(ctx, global.get(), "root", value.release()), 0);
    }

    void
    installValue(char const* name, JSValue value)
    {
        ASSERT_FALSE(JS_IsException(value));
        jshookz::qjs::OwnedValue owned(ctx, value);
        jshookz::qjs::OwnedValue global(ctx, JS_GetGlobalObject(ctx));
        ASSERT_FALSE(global.isException());
        ASSERT_GE(
            JS_SetPropertyStr(ctx, global.get(), name, owned.release()), 0);
    }
};

TEST_F(XahauTypes, Hash256Roundtrip)
{
    auto v = eval(
        "Hash256.fromHex("
        "'00000000000000000000000000000000000000000000000000000000000000ff'"
        ").toHex()");
    ASSERT_FALSE(v.isException());
    EXPECT_EQ(
        to_string(v.get()),
        "00000000000000000000000000000000000000000000000000000000000000FF");
}

TEST_F(XahauTypes, AccountIDZero)
{
    auto v = eval("AccountID.zero.isZero()");
    ASSERT_FALSE(v.isException());
    EXPECT_TRUE(JS_ToBool(ctx, v.get()));
}

TEST_F(XahauTypes, STBlobFromBytes)
{
    auto v = eval("STBlob.from(new Uint8Array([1, 2, 3])).byteLength");
    ASSERT_FALSE(v.isException());
    int32_t n = 0;
    ASSERT_EQ(JS_ToInt32(ctx, &n, v.get()), 0);
    EXPECT_EQ(n, 3);
}

TEST_F(XahauTypes, UIntAddOverflowIsResult)
{
    auto v = eval("UInt8.from(200).okOr(null).add(200)");
    ASSERT_FALSE(v.isException());
    auto ok = jshookz::qjs::property(ctx, v.get(), "ok");
    EXPECT_FALSE(JS_ToBool(ctx, ok.get()));
    auto domain = jshookz::qjs::property(
        ctx, jshookz::qjs::property(ctx, v.get(), "error").get(), "domain");
    EXPECT_EQ(to_string(domain.get()), "uint");
}

TEST_F(XahauTypes, CertifiedObjectIsLazyImmutableAndCanonical)
{
    // Sequence is first on wire; canonical field-code order puts Flags first.
    installRoot({
        0x24, 0x00, 0x00, 0x00, 0x07,
        0x22, 0x00, 0x00, 0x00, 0x09,
    });
    auto value = eval(R"JS(
        (() => {
          const flags = root.Flags;
          const fieldBytes = root.fieldBytes("Flags").toBytes();
          const canonical = root.toBytes();
          let assignmentFailed = false;
          try {
            (() => {
              "use strict";
              root.Flags = UInt32.from(1).okOr(null);
            })();
          }
          catch (_) { assignmentFailed = true; }
          return JSON.stringify({
            keys: Object.keys(root),
            same: flags === root.Flags && flags === root.get("Flags"),
            flags: flags.toNumber(),
            sequence: root.Sequence.toNumber(),
            fieldBytes: Array.from(fieldBytes),
            canonical: Array.from(canonical),
            json: root.toJSON(),
            jsonFresh: root.toJSON() !== root.toJSON(),
            extensible: Object.isExtensible(root),
            assignmentFailed,
            absent: root.get("NotAField") === undefined,
          });
        })()
    )JS");
    ASSERT_FALSE(value.isException());
    EXPECT_EQ(
        to_string(value.get()),
        R"({"keys":["Flags","Sequence"],"same":true,"flags":9,"sequence":7,"fieldBytes":[0,0,0,9],"canonical":[34,0,0,0,9,36,0,0,0,7],"json":{"Flags":9,"Sequence":7},"jsonFresh":true,"extensible":false,"assignmentFailed":true,"absent":true})");
}

TEST_F(XahauTypes,
       ObjectReplacementIsIndependentAndAbsentRemovalPreservesIdentity) {
  installRoot({
      0x24,
      0x00,
      0x00,
      0x00,
      0x07,
      0x22,
      0x00,
      0x00,
      0x00,
      0x09,
  });
  auto value = eval(R"JS(
        (() => {
          const changed = root.withField(
            Field.Flags, new Uint8Array([0, 0, 0, 10]));
          const identical = root.withField(
            "Flags", new Uint8Array([0, 0, 0, 9]));
          const removed = root.withoutField(Field.Flags);
          const absent = root.withoutField("Account");
          return changed !== root && identical !== root && removed !== root &&
            absent === root &&
            root.Flags.toNumber() === 9 && changed.Flags.toNumber() === 10 &&
            identical.Flags.toNumber() === 9 &&
            removed.Flags === undefined && removed.Sequence.toNumber() === 7 &&
            Array.from(changed.toBytes()).join(",") ===
              "34,0,0,0,10,36,0,0,0,7";
        })()
    )JS");
  ASSERT_FALSE(value.isException());
  EXPECT_TRUE(JS_ToBool(ctx, value.get()));
}

TEST_F(XahauTypes, ObjectReplacementAcceptsNumberEnumAndCanonicalRawContainer) {
  installRoot({});
  auto value = eval(R"JS(
        (() => {
          const number = root.withField("Number", "1.25");
          const transaction = root.withField("TransactionType", 0);
          const nested = root.withField("Memo", new Uint8Array([0xE1]));
          return JSON.stringify({
            number: number.Number,
            bytes: Array.from(number.fieldBytes("Number").toBytes()),
            transaction: transaction.TransactionType,
            nestedSize: nested.Memo.toBytes().length,
          });
        })()
    )JS");
  ASSERT_FALSE(value.isException());
  EXPECT_EQ(
      to_string(value.get()),
      R"({"number":"1.25","bytes":[0,4,112,222,77,248,32,0,255,255,255,241],"transaction":0,"nestedSize":1})");

  auto rejects = eval(R"JS(
        (() => {
          const invalid = [
            () => root.withField("Flags", new Uint8Array([0, 0, 1])),
            () => root.withField("Memo", new Uint8Array([])),
            () => root.withField("Number", "0001.25"),
            () => root.withField("TransactionType", 1.5),
            () => root.withField("Flags", undefined),
            () => root.withField("NotAField", new Uint8Array()),
          ];
          return invalid.every(fn => {
            try { fn(); return false; }
            catch (error) { return error instanceof TypeError; }
          });
        })()
    )JS");
  ASSERT_FALSE(rejects.isException());
  EXPECT_TRUE(JS_ToBool(ctx, rejects.get()));
}

TEST_F(XahauTypes, ObjectReplacementNumberStringsMatchPinnedOracleFormatting) {
  installRoot({});
  auto value = eval(R"JS(
        JSON.stringify([
          "0", "-0", "1", "1.25", "0.5", "-0.00125",
          "1e10", "1e11", "1e-10", "1e-11",
          "10000000000000005", "10000000000000015",
        ].map(input => root.withField("Number", input).Number))
    )JS");
  ASSERT_FALSE(value.isException());
  EXPECT_EQ(
      to_string(value.get()),
      R"(["0","0","1","1.25","0.5","-0.00125","10000000000","1000000000000000e-4","0.0000000001","1000000000000000e-26","1000000000000000e1","1000000000000002e1"])");
}

TEST_F(XahauTypes, ObjectReplacementStreamsCertifiedObjectAndArrayValues) {
  installRoot({
      0xF9,
      0xEA,
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
      0xE1,
      0xF1,
  });
  auto value = eval(R"JS(
        (() => {
          const element = root.Memos[0];
          const objectCopy = root.withField("Memo", element);
          const arrayCopy = root.withField("Memos", root.Memos);
          let mismatch = false;
          try { root.withField("Memo", root.Memos); }
          catch (error) { mismatch = error instanceof TypeError; }
          return objectCopy !== root && arrayCopy !== root &&
            objectCopy.Memo.Flags.toNumber() === 2 &&
            objectCopy.Memo.Sequence.toNumber() === 4 &&
            arrayCopy.Memos !== root.Memos &&
            arrayCopy.Memos[0].Flags.toNumber() === 2 && mismatch;
        })()
    )JS");
  ASSERT_FALSE(value.isException());
  EXPECT_TRUE(JS_ToBool(ctx, value.get()));
}

TEST_F(XahauTypes, ObjectReplacementAcceptsEveryExactNominalMaterializer) {
  namespace types = jshookz::provider::types;
  auto expose = [&](char const *name, JSValue value) {
    EXPECT_FALSE(JS_IsException(value)) << name;
    if (JS_IsException(value))
      return;
    jshookz::qjs::OwnedValue global(ctx, JS_GetGlobalObject(ctx));
    ASSERT_FALSE(global.isException());
    ASSERT_EQ(JS_SetPropertyStr(ctx, global.get(), name, value), 1);
  };

  std::array<std::uint8_t, 32> bytes{};
  for (std::uint32_t i = 0; i < bytes.size(); ++i)
    bytes[i] = static_cast<std::uint8_t>(i + 1);
  std::array<std::uint8_t, 20> account{};
  for (std::uint32_t i = 0; i < account.size(); ++i)
    account[i] = static_cast<std::uint8_t>(0x40 + i);
  std::array<std::uint8_t, 20> currency{};
  currency[12] = 'U';
  currency[13] = 'S';
  currency[14] = 'D';
  std::array<std::uint8_t, 8> amount{0x40, 0, 0, 0, 0, 0, 0, 42};
  std::array<std::uint8_t, 20> nativeIssue{};
  std::array<std::uint8_t, 82> bridge{};
  bridge[0] = 20;
  std::memcpy(bridge.data() + 1, account.data(), account.size());
  bridge[41] = 20;
  std::memcpy(bridge.data() + 42, account.data(), account.size());

  expose("vUInt8", types::makeUIntValue(ctx, 8, 0x7f));
  expose("vUInt16", types::makeUIntValue(ctx, 16, 0x1234));
  expose("vUInt32", types::makeUIntValue(ctx, 32, 0x12345678));
  expose("vUInt64", types::makeUIntValue(ctx, 64, 0x123456789abcdef0));
  expose("vHash128", types::makeHash128Bytes(ctx, bytes.data(), 16));
  expose("vHash160", types::makeHash160Bytes(ctx, bytes.data(), 20));
  expose("vHash192", types::makeHash192Bytes(ctx, bytes.data(), 24));
  expose("vHash256", types::makeHash256Bytes(ctx, bytes.data(), 32));
  expose("vBlob", types::makeSTBlobBytes(ctx, bytes.data(), 7));
  expose("vAccount",
         types::makeAccountIDBytes(ctx, account.data(), account.size()));
  expose("vAmount", types::makeAmountBytes(ctx, amount.data(), amount.size()));
  expose("vCurrency",
         types::makeCurrencyBytes(ctx, currency.data(), currency.size()));
  expose("vIssue",
         types::makeIssueBytes(ctx, nativeIssue.data(), nativeIssue.size()));
  expose("vVector", types::makeVector256Bytes(ctx, bytes.data(), bytes.size()));
  expose("vBridge",
         types::makeXChainBridgeBytes(ctx, bridge.data(), bridge.size()));

  installRoot(hexBytes("011201404142434445464748494A4B4C4D4E4F5051525300"));
  auto path = eval("root.Paths");
  ASSERT_FALSE(path.isException());
  expose("vPathSet", path.release());
  installRoot({});

  auto result = eval(R"JS(
        (() => {
          const inputs = [
            ["CloseResolution", vUInt8],
            ["LedgerEntryType", vUInt16],
            ["NetworkID", vUInt32],
            ["IndexNext", vUInt64],
            ["EmailHash", vHash128],
            ["TakerPaysCurrency", vHash160],
            ["MPTokenIssuanceID", vHash192],
            ["LedgerHash", vHash256],
            ["PublicKey", vBlob],
            ["Account", vAccount],
            ["Amount", vAmount],
            ["BaseAsset", vCurrency],
            ["LockingChainIssue", vIssue],
            ["Paths", vPathSet],
            ["Indexes", vVector],
            ["XChainBridge", vBridge],
          ];
          let current = root;
          for (const [field, value] of inputs)
            current = current.withField(field, value);
          const exact = inputs.every(([field, input]) => {
            const observed = current.get(field);
            return observed !== input &&
              (typeof input.toBytes === "function"
                ? Array.from(current.fieldBytes(field).toBytes()).join(",") ===
                    Array.from(input.toBytes()).join(",")
                : observed.toString() === input.toString());
          });
          let wrongNominal = false;
          try { root.withField("BaseAsset", vAccount); }
          catch (error) { wrongNominal = error instanceof TypeError; }
          return exact && wrongNominal && Reflect.ownKeys(current).length === 16 &&
            Reflect.ownKeys(root).length === 0;
        })()
    )JS");
  if (result.isException()) {
    jshookz::qjs::OwnedValue error(ctx, JS_GetException(ctx));
    FAIL() << to_string(error.get());
  }
  EXPECT_TRUE(JS_ToBool(ctx, result.get()));
}


TEST_F(XahauTypes, NestedArraySharesElementIdentityAndRealIteratorSymbol)
{
    // Memos -> two Memo object elements, each with a Flags leaf.
    installRoot({
        0xF9,
        0xEA, 0x22, 0x00, 0x00, 0x00, 0x01, 0xE1,
        0xEA, 0x22, 0x00, 0x00, 0x00, 0x02, 0xE1,
        0xF1,
    });
    auto value = eval(R"JS(
        (() => {
          const values = root.Memos;
          const first = values[0];
          const iterated = Array.from(values);
          return JSON.stringify({
            rootKeys: Object.keys(root),
            arrayKeys: Reflect.ownKeys(values),
            lengthOwn: Object.hasOwn(values, "length"),
            length: values.length,
            firstSame: first === values.at(0) && first === iterated[0],
            numbers: iterated.map(v => v.Flags.toNumber()),
            json: root.toJSON(),
            arrayJson: values.toJSON(),
            extensible: Object.isExtensible(values),
            absent: values.at(99) === undefined,
          });
        })()
    )JS");
    ASSERT_FALSE(value.isException());
    EXPECT_EQ(
        to_string(value.get()),
        R"({"rootKeys":["Memos"],"arrayKeys":["0","1","length"],"lengthOwn":true,"length":2,"firstSame":true,"numbers":[1,2],"json":{"Memos":[{"Memo":{"Flags":1}},{"Memo":{"Flags":2}}]},"arrayJson":[{"Memo":{"Flags":1}},{"Memo":{"Flags":2}}],"extensible":false,"absent":true})");
}

TEST_F(XahauTypes, ExactMintRejectsMalformedInputAndRegistrarRetries)
{
    std::uint8_t const malformed[] = {0x22, 0x00};
    jshookz::qjs::OwnedValue value(
        ctx,
        jshookz::provider::types::makeCertifiedObjectCopy(
            ctx, malformed, sizeof(malformed)));
    EXPECT_TRUE(value.isException());
    jshookz::qjs::OwnedValue exception(ctx, JS_GetException(ctx));
    EXPECT_FALSE(exception.isException());
    EXPECT_TRUE(JS_IsError(ctx, exception.get()));

    EXPECT_TRUE(jshookz::provider::types::registerObjectTypes(ctx));
    EXPECT_FALSE(JS_HasException(ctx));
    installRoot({});
    auto empty = eval(
        "Object.keys(root).length === 0 && root.toBytes().length === 0");
    ASSERT_FALSE(empty.isException());
    EXPECT_TRUE(JS_ToBool(ctx, empty.get()));
}

TEST_F(XahauTypes, ObjectByteUtilitiesUseExactInputAndNominalResultLaws)
{
    auto valid = eval("new Uint8Array([0x22, 0, 0, 0, 9])");
    ASSERT_FALSE(valid.isException());
    jshookz::qjs::OwnedValue predicate(
        ctx,
        jshookz::provider::types::validateObjectBytes(ctx, valid.get()));
    ASSERT_FALSE(predicate.isException());
    EXPECT_TRUE(JS_ToBool(ctx, predicate.get()));

    installValue(
        "decodedResult",
        jshookz::provider::types::safeDecodeObjectBytes(ctx, valid.get()));
    auto success = eval(
        "decodedResult.ok && decodedResult.value.Flags.toNumber() === 9 && "
        "Object.isExtensible(decodedResult) === false");
    ASSERT_FALSE(success.isException());
    EXPECT_TRUE(JS_ToBool(ctx, success.get()));

    auto malformed = eval("new Uint8Array([0x22, 0])");
    ASSERT_FALSE(malformed.isException());
    jshookz::qjs::OwnedValue invalidPredicate(
        ctx,
        jshookz::provider::types::validateObjectBytes(
            ctx, malformed.get()));
    ASSERT_FALSE(invalidPredicate.isException());
    EXPECT_FALSE(JS_ToBool(ctx, invalidPredicate.get()));

    installValue(
        "failedResult",
        jshookz::provider::types::safeDecodeObjectBytes(
            ctx, malformed.get()));
    auto failure = eval(
        "JSON.stringify({ok: failedResult.ok, "
        "domain: failedResult.error.domain, "
        "issue: failedResult.error.issue, "
        "offset: failedResult.error.offset, "
        "fieldCode: failedResult.error.fieldCode, "
        "proto: Object.getPrototypeOf(failedResult.error) === null})");
    ASSERT_FALSE(failure.isException());
    EXPECT_EQ(
        to_string(failure.get()),
        R"({"ok":false,"domain":"parse","issue":"invalid-field","offset":1,"fieldCode":131074,"proto":true})");

    auto blob = eval("STBlob.from(new Uint8Array([0x22, 0, 0, 0, 7]))");
    ASSERT_FALSE(blob.isException());
    jshookz::qjs::OwnedValue fromBlob(
        ctx,
        jshookz::provider::types::decodeObjectBytes(ctx, blob.get()));
    ASSERT_FALSE(fromBlob.isException());
    EXPECT_TRUE(jshookz::provider::types::isSTObject(fromBlob.get()));
}

TEST_F(XahauTypes, ObjectByteUtilitiesPublishExactWrongTerminatorAndTypeErrors)
{
    auto wrongClose = eval("new Uint8Array([0xF1])");
    ASSERT_FALSE(wrongClose.isException());
    installValue(
        "closeResult",
        jshookz::provider::types::safeDecodeObjectBytes(
            ctx, wrongClose.get()));
    auto close = eval(
        "JSON.stringify(closeResult.error)");
    ASSERT_FALSE(close.isException());
    EXPECT_EQ(
        to_string(close.get()),
        R"({"domain":"parse","issue":"wrong-terminator","offset":0,"expected":"root-eof","actual":983041})");

    auto malformed = eval("new Uint8Array([0x22, 0])");
    ASSERT_FALSE(malformed.isException());
    jshookz::qjs::OwnedValue decoded(
        ctx,
        jshookz::provider::types::decodeObjectBytes(
            ctx, malformed.get()));
    ASSERT_TRUE(decoded.isException());
    jshookz::qjs::OwnedValue assertionError(ctx, JS_GetException(ctx));
    ASSERT_TRUE(JS_IsError(ctx, assertionError.get()));
    installValue("assertionError", assertionError.release());
    auto assertion = eval(
        "JSON.stringify({type: assertionError instanceof TypeError, "
        "message: assertionError.message, offset: assertionError.offset, "
        "fieldCode: assertionError.fieldCode, "
        "stack: Object.hasOwn(assertionError, 'stack')})");
    ASSERT_FALSE(assertion.isException());
    EXPECT_EQ(
        to_string(assertion.get()),
        R"({"type":true,"message":"truncated field","offset":1,"fieldCode":131074,"stack":false})");

    auto wrongKind = eval("[0x22, 0, 0, 0, 1]");
    ASSERT_FALSE(wrongKind.isException());
    jshookz::qjs::OwnedValue wrong(
        ctx,
        jshookz::provider::types::validateObjectBytes(
            ctx, wrongKind.get()));
    ASSERT_TRUE(wrong.isException());
    jshookz::qjs::OwnedValue contractError(ctx, JS_GetException(ctx));
    installValue("contractError", contractError.release());
    auto contract = eval(
        "contractError instanceof TypeError && "
        "contractError.message === "
        "'expected Uint8Array, ArrayBuffer, or STBlob' && "
        "Reflect.ownKeys(contractError).length === 1");
    ASSERT_FALSE(contract.isException());
    EXPECT_TRUE(JS_ToBool(ctx, contract.get()));
}

TEST_F(XahauTypes, GeneratedFieldDescriptorsAreExactNominalAndComplete)
{
    installRoot({0x22, 0x00, 0x00, 0x00, 0x09});
    auto value = eval(R"JS(
        (() => {
          const descriptor = Field.Flags;
          return JSON.stringify({
            count: Object.keys(Field).length,
            code: descriptor.code,
            typeCode: descriptor.typeCode,
            fieldCode: descriptor.fieldCode,
            frozen: Object.isFrozen(Field),
            descriptorExtensible: Object.isExtensible(descriptor),
            lookup: root.get(descriptor).toNumber(),
            structuralMiss: root.get({code: descriptor.code}) === undefined,
          });
        })()
    )JS");
    ASSERT_FALSE(value.isException());
    EXPECT_EQ(
        to_string(value.get()),
        R"({"count":325,"code":131074,"typeCode":2,"fieldCode":2,"frozen":true,"descriptorExtensible":false,"lookup":9,"structuralMiss":true})");
}

TEST_F(XahauTypes, RichFixedIssueVectorAndBridgeLeavesAreNominalAndImmutable)
{
    std::uint8_t hash192[24] = {};
    hash192[0] = 0x12;
    hash192[23] = 0xEF;
    std::uint8_t currency[20] = {};
    currency[12] = 'U';
    currency[13] = 'S';
    currency[14] = 'D';
    std::uint8_t issue[40] = {};
    std::memcpy(issue, currency, sizeof(currency));
    issue[20] = 0xB5;
    issue[39] = 0xE8;
    std::uint8_t vector[64] = {};
    vector[0] = 1;
    vector[32] = 2;
    std::uint8_t bridge[82] = {};
    bridge[0] = 20;
    bridge[1] = 3;
    bridge[41] = 20;
    bridge[42] = 4;

    installValue(
        "hash192",
        jshookz::provider::types::makeHash192Bytes(
            ctx, hash192, sizeof(hash192)));
    installValue(
        "currency",
        jshookz::provider::types::makeCurrencyBytes(
            ctx, currency, sizeof(currency)));
    installValue(
        "issue",
        jshookz::provider::types::makeIssueBytes(
            ctx, issue, sizeof(issue)));
    installValue(
        "vector",
        jshookz::provider::types::makeVector256Bytes(
            ctx, vector, sizeof(vector)));
    installValue(
        "bridge",
        jshookz::provider::types::makeXChainBridgeBytes(
            ctx, bridge, sizeof(bridge)));

    auto value = eval(R"JS(
        (() => {
          const first = vector.at(0);
          const iterated = [...vector];
          const bytes = vector.toBytes();
          bytes[0] = 9;
          return JSON.stringify({
            hash: hash192.toHex(),
            currency: currency.toString(),
            issueKind: issue.kind,
            issueCurrencySame: issue.currency === issue.currency,
            issueIssuerSame: issue.issuer === issue.issuer,
            vectorLength: vector.length,
            vectorKeys: Reflect.ownKeys(vector),
            vectorIdentity: first === vector[0] && first === iterated[0],
            vectorFresh: vector.toBytes()[0] === 1,
            bridgeDoorSame:
              bridge.LockingChainDoor === bridge.LockingChainDoor,
            bridgeIssueSame:
              bridge.LockingChainIssue === bridge.LockingChainIssue,
            immutable: !Object.isExtensible(hash192) &&
              !Object.isExtensible(currency) && !Object.isExtensible(issue) &&
              !Object.isExtensible(vector) && !Object.isExtensible(bridge),
          });
        })()
    )JS");
    ASSERT_FALSE(value.isException());
    EXPECT_EQ(
        to_string(value.get()),
        R"({"hash":"1200000000000000000000000000000000000000000000EF","currency":"USD","issueKind":"iou","issueCurrencySame":true,"issueIssuerSame":true,"vectorLength":2,"vectorKeys":["0","1","length"],"vectorIdentity":true,"vectorFresh":true,"bridgeDoorSame":true,"bridgeIssueSame":true,"immutable":true})");

    std::uint8_t copiedCurrency[20] = {};
    std::uint8_t copiedHash192[24] = {};
    auto currencyValue = eval("currency");
    auto hashValue = eval("hash192");
    ASSERT_FALSE(currencyValue.isException());
    ASSERT_FALSE(hashValue.isException());
    EXPECT_TRUE(jshookz::provider::types::readCurrencyBytes(
        ctx, currencyValue.get(), copiedCurrency));
    EXPECT_TRUE(jshookz::provider::types::readHash192Bytes(
        ctx, hashValue.get(), copiedHash192));
    EXPECT_EQ(std::memcmp(copiedCurrency, currency, sizeof(currency)), 0);
    EXPECT_EQ(std::memcmp(copiedHash192, hash192, sizeof(hash192)), 0);
}

TEST_F(XahauTypes, ObjectMaterializesAmountPathSetAndBridgeWithExactIdentity)
{
    installRoot(hexBytes(
        "61D4838D7EA4C680000000000000000000000000005553440000000000"
        "B5F762798A53D543A014CAF8B297CFF8F2F937E8"
        "8114B5F762798A53D543A014CAF8B297CFF8F2F937E8"));
    auto amount = eval(R"JS(
        (() => {
          const value = root.Amount;
          return value === root.get(Field.Amount) && value === root.Amount &&
            value.kind === "iou" && value.value.mantissa() === 1000000000000000n &&
            value.currency.toString() === "USD" &&
            value.issuer.toHex() === "B5F762798A53D543A014CAF8B297CFF8F2F937E8" &&
            value.issue.kind === "iou" && !Object.isExtensible(value);
        })()
    )JS");
    ASSERT_FALSE(amount.isException());
    EXPECT_TRUE(JS_ToBool(ctx, amount.get()));

    installRoot(hexBytes(
        "8114B5F762798A53D543A014CAF8B297CFF8F2F937E8"
        "011201B5F762798A53D543A014CAF8B297CFF8F2F937E800"));
    auto paths = eval(R"JS(
        (() => {
          const value = root.Paths;
          const path = value.at(0);
          const hop = path.at(0);
          return value === root.Paths && value.length === 1 &&
            path !== value.at(0) && hop !== path.at(0) &&
            hop.account.toHex() ===
              "B5F762798A53D543A014CAF8B297CFF8F2F937E8" &&
            [...value].length === 1 && [...path].length === 1;
        })()
    )JS");
    ASSERT_FALSE(paths.isException());
    EXPECT_TRUE(JS_ToBool(ctx, paths.get()));

    installRoot(hexBytes(
        "011914B5F762798A53D543A014CAF8B297CFF8F2F937E8"
        "0000000000000000000000000000000000000000"
        "14B5F762798A53D543A014CAF8B297CFF8F2F937E8"
        "0000000000000000000000000000000000000000"));
    auto bridge = eval(R"JS(
        (() => {
          const value = root.XChainBridge;
          return value === root.XChainBridge &&
            value.LockingChainDoor.toHex() ===
              "B5F762798A53D543A014CAF8B297CFF8F2F937E8" &&
            value.LockingChainIssue.kind === "native" &&
            value.IssuingChainIssue.kind === "native";
        })()
    )JS");
    ASSERT_FALSE(bridge.isException());
    EXPECT_TRUE(JS_ToBool(ctx, bridge.get()));
}

TEST_F(XahauTypes, ObjectJSONDispatchesEveryStructuredLeafCanonically)
{
    struct Vector {
        char const* wire;
        char const* json;
    };
    Vector const vectors[] = {
        {
            "8114B5F762798A53D543A014CAF8B297CFF8F2F937E8",
            R"({"Account":"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh"})",
        },
        {
            "61D4838D7EA4C680000000000000000000000000005553440000000000"
            "B5F762798A53D543A014CAF8B297CFF8F2F937E8",
            R"({"Amount":{"currency":"USD","issuer":"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh","value":"1"}})",
        },
        {
            "011A0000000000000000000000000000000000000000",
            R"({"BaseAsset":"XAH"})",
        },
        {
            "01180000000000000000000000000000000000000000",
            R"({"LockingChainIssue":{"currency":"XAH"}})",
        },
        {
            "011201B5F762798A53D543A014CAF8B297CFF8F2F937E800",
            R"({"Paths":[[{"account":"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh","type":1}]]})",
        },
        {
            "011320000102030405060708090A0B0C0D0E0F"
            "101112131415161718191A1B1C1D1E1F",
            R"({"Indexes":["000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F"]})",
        },
        {
            "011914B5F762798A53D543A014CAF8B297CFF8F2F937E8"
            "0000000000000000000000000000000000000000"
            "14B5F762798A53D543A014CAF8B297CFF8F2F937E8"
            "0000000000000000000000000000000000000000",
            R"({"XChainBridge":{"IssuingChainDoor":"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh","IssuingChainIssue":{"currency":"XAH"},"LockingChainDoor":"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh","LockingChainIssue":{"currency":"XAH"}}})",
        },
    };
    for (auto const& vector : vectors) {
        SCOPED_TRACE(vector.wire);
        installRoot(hexBytes(vector.wire));
        auto value = eval("JSON.stringify(root.toJSON())");
        ASSERT_FALSE(value.isException());
        EXPECT_EQ(to_string(value.get()), vector.json);
    }
}
