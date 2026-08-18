#pragma once

#include <jshookz/qjs.hpp>
#include <qjs_visitor.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

extern "C" void register_protocol_functions(JSContext *ctx);

inline constexpr char const *kPaymentHex =
    "12000024000000016140000000000F424068400000000000000A"
    "8114B5F762798A53D543A014CAF8B297CFF8F2F937E8"
    "831449FF0C73CA6AF9733DA805F76CA2C37776B7C46B";

inline constexpr char const *kMemoHex =
    "1200002200000000240000006461400000000007A12068400000000000000C"
    "8114B5F762798A53D543A014CAF8B297CFF8F2F937E88314F667B0CA50"
    "CC7709A220B0561B85E53A48461FA8F9EA7C0A746578742F706C61696E"
    "7D0548656C6C6FE1F1";

class XdataQjs : public ::testing::Test
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
        register_protocol_functions(ctx);
        ASSERT_FALSE(JS_HasException(ctx));
    }

    void
    TearDown() override
    {
        if (ctx)
            JS_FreeContext(ctx);
        if (rt)
            JS_FreeRuntime(rt);
        ctx = nullptr;
        rt = nullptr;
    }

    jshookz::qjs::OwnedValue
    global()
    {
        return jshookz::qjs::OwnedValue(ctx, JS_GetGlobalObject(ctx));
    }

    std::string
    to_std_string(JSValueConst value)
    {
        char const *text = JS_ToCString(ctx, value);
        std::string out = text ? text : "";
        if (text)
            JS_FreeCString(ctx, text);
        return out;
    }

    std::string
    exception_text()
    {
        if (!JS_HasException(ctx))
            return {};
        jshookz::qjs::OwnedValue exc(ctx, JS_GetException(ctx));
        return to_std_string(exc.get());
    }

    void
    expect_clean()
    {
        if (JS_HasException(ctx))
            ADD_FAILURE() << exception_text();
    }

    jshookz::qjs::OwnedValue
    call1(char const *name, JSValueConst arg)
    {
        auto g = global();
        auto fn = jshookz::qjs::property(ctx, g.get(), name);
        JSValueConst argv[] = {arg};
        auto out = jshookz::qjs::OwnedValue(
            ctx, JS_Call(ctx, fn.get(), g.get(), 1, argv));
        if (!out.isException())
            expect_clean();
        return out;
    }

    jshookz::qjs::OwnedValue
    decode_hex(std::string_view hex)
    {
        jshookz::qjs::OwnedValue s(
            ctx, JS_NewStringLen(ctx, hex.data(), hex.size()));
        return call1("decode_object", s.get());
    }

    jshookz::qjs::OwnedValue
    encode(JSValueConst obj)
    {
        return call1("encode_object", obj);
    }

    std::string
    util_hex(JSValueConst bytes)
    {
        auto hex = call1("util_hex", bytes);
        EXPECT_FALSE(hex.isException()) << exception_text();
        auto out = to_std_string(hex.get());
        for (char &c : out) {
            if (c >= 'a' && c <= 'f')
                c = static_cast<char>(c - 'a' + 'A');
        }
        return out;
    }

    std::string
    encode_hex(JSValueConst obj)
    {
        auto bytes = encode(obj);
        EXPECT_FALSE(bytes.isException()) << exception_text();
        return util_hex(bytes.get());
    }

    jshookz::qjs::OwnedValue
    prop(JSValueConst obj, char const *name)
    {
        return jshookz::qjs::property(ctx, obj, name);
    }

    std::string
    prop_string(JSValueConst obj, char const *name)
    {
        return to_std_string(prop(obj, name).get());
    }

    int64_t
    prop_int(JSValueConst obj, char const *name)
    {
        int64_t value = 0;
        EXPECT_EQ(JS_ToInt64(ctx, &value, prop(obj, name).get()), 0);
        return value;
    }

    bool
    has_own(JSValueConst obj, char const *name)
    {
        JSAtom atom = JS_NewAtom(ctx, name);
        int ok = JS_GetOwnProperty(ctx, nullptr, obj, atom);
        JS_FreeAtom(ctx, atom);
        return ok > 0;
    }

    jshookz::qjs::OwnedValue
    json_stringify(JSValueConst value)
    {
        auto g = global();
        auto json = prop(g.get(), "JSON");
        auto stringify = prop(json.get(), "stringify");
        JSValueConst argv[] = {value};
        return jshookz::qjs::OwnedValue(
            ctx, JS_Call(ctx, stringify.get(), json.get(), 1, argv));
    }

    std::string
    json_text(JSValueConst value)
    {
        auto s = json_stringify(value);
        EXPECT_FALSE(s.isException()) << exception_text();
        return to_std_string(s.get());
    }

    jshookz::qjs::OwnedValue
    parse_json(std::string_view text)
    {
        return jshookz::qjs::OwnedValue(
            ctx, JS_ParseJSON(ctx, text.data(), text.size(), "<json>"));
    }

    bool
    json_equal(JSValueConst a, JSValueConst b)
    {
        auto fn = eval(R"JS((function(a, b) {
            function canon(v) {
                if (v === null || typeof v !== "object")
                    return v;
                if (Array.isArray(v))
                    return v.map(canon);
                var o = {};
                Object.keys(v).sort().forEach(function (k) { o[k] = canon(v[k]); });
                return o;
            }
            return JSON.stringify(canon(a)) === JSON.stringify(canon(b));
        }))JS");
        JSValueConst argv[] = {a, b};
        auto r = jshookz::qjs::OwnedValue(
            ctx, JS_Call(ctx, fn.get(), JS_UNDEFINED, 2, argv));
        EXPECT_FALSE(r.isException()) << exception_text();
        return JS_ToBool(ctx, r.get()) > 0;
    }

    jshookz::qjs::OwnedValue
    eval(std::string_view src, char const *label = "<test>")
    {
        return jshookz::qjs::OwnedValue(
            ctx,
            JS_Eval(ctx, src.data(), src.size(), label, JS_EVAL_TYPE_GLOBAL));
    }

    bool
    is_stobject(JSValueConst value)
    {
        auto g = global();
        auto ctor = prop(g.get(), "STObject");
        int rc = JS_IsInstanceOf(ctx, value, ctor.get());
        EXPECT_GE(rc, 0) << exception_text();
        return rc > 0;
    }

    catl::xdata::STObjectData *
    stobject_data(JSValueConst value)
    {
        return jshookz::qjs::tryOpaque<catl::xdata::STObjectData>(
            value, catl::xdata::STObjectClass::class_id);
    }

    jshookz::qjs::OwnedValue
    uint8_from_hex(std::string_view hex)
    {
        std::vector<std::uint8_t> bytes;
        EXPECT_TRUE(jshookz::qjs::hexDecode(hex, bytes));
        return jshookz::qjs::OwnedValue(
            ctx, jshookz::qjs::uint8Array(ctx, bytes));
    }

    static std::string
    read_file(char const *path)
    {
        std::ifstream in(path);
        EXPECT_TRUE(in) << path;
        std::ostringstream buf;
        buf << in.rdbuf();
        return buf.str();
    }
};
