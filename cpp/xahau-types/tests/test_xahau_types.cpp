#include "result.hpp"

#include <jshookz/qjs.hpp>

#include <gtest/gtest.h>

#include <cstring>
#include <string>

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
        ASSERT_FALSE(JS_HasException(ctx));
    }

    void
    TearDown() override
    {
        if (ctx)
            JS_FreeContext(ctx);
        if (rt)
            JS_FreeRuntime(rt);
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
