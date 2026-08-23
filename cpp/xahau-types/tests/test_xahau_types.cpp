#include "result.hpp"
#include "object/object.hpp"

#include <jshookz/qjs.hpp>

#include <gtest/gtest.h>

#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

extern "C" bool register_cpp_types(JSContext *ctx);
extern "C" bool register_uint_types(JSContext *ctx);

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
        ASSERT_TRUE(jshookz::provider::types::registerObjectTypes(ctx));
        ASSERT_FALSE(JS_HasException(ctx));
    }

    void
    TearDown() override
    {
        if (ctx)
            JS_FreeContext(ctx);
        if (rt) {
            jshookz::provider::types::unregisterObjectTypes(rt);
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
            extensible: Object.isExtensible(root),
            assignmentFailed,
            absent: root.get("NotAField") === undefined,
          });
        })()
    )JS");
    ASSERT_FALSE(value.isException());
    EXPECT_EQ(
        to_string(value.get()),
        R"({"keys":["Flags","Sequence"],"same":true,"flags":9,"sequence":7,"fieldBytes":[0,0,0,9],"canonical":[34,0,0,0,9,36,0,0,0,7],"extensible":false,"assignmentFailed":true,"absent":true})");
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
            extensible: Object.isExtensible(values),
            absent: values.at(99) === undefined,
          });
        })()
    )JS");
    ASSERT_FALSE(value.isException());
    EXPECT_EQ(
        to_string(value.get()),
        R"({"rootKeys":["Memos"],"arrayKeys":["0","1","length"],"lengthOwn":true,"length":2,"firstSame":true,"numbers":[1,2],"extensible":false,"absent":true})");
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
