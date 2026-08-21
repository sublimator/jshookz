// Eight-quadrant control-verb truth table (0072:892-935) plus the onFail
// falsy-preservation proofs (0072:190-340 gates). Authored by the
// declaration lane as an executable handoff; CMake wiring is deliberately
// left to the cpp bindings lane (mirror cpp/xahau-types/tests: gtest_main
// plus the provider bindings objects, and link THIS file's host-import
// stubs instead of a wasm host).
//
// Native caveat: the real accept/rollback terminals never return — the
// wasm host unwinds the instance. The recording stubs below return, so
// evaluation continues past a terminal; these tests therefore assert
// DISPATCH (which terminal fired, or which value returned), not unwind.
// The stubs cannot recover message text: the bindings pass
// (uint32_t)(uintptr_t)ptr, which truncates on a 64-bit native build.

#include "common.hpp"
#include "result.hpp"

#include <jshookz/qjs.hpp>

#include <gtest/gtest.h>

#include <cstring>
#include <string>

namespace {
int g_accepts = 0;
int g_rollbacks = 0;
int64_t g_last_accept_code = -1;
int64_t g_last_rollback_code = -1;
}  // namespace

extern "C" int64_t
hook_accept(uint32_t, uint32_t, int64_t code)
{
    ++g_accepts;
    g_last_accept_code = code;
    return 0;
}

extern "C" int64_t
hook_rollback(uint32_t, uint32_t, int64_t code)
{
    ++g_rollbacks;
    g_last_rollback_code = code;
    return 0;
}

class ControlTruthTable : public ::testing::Test
{
protected:
    JSRuntime *rt = nullptr;
    JSContext *ctx = nullptr;

    void
    SetUp() override
    {
        g_accepts = 0;
        g_rollbacks = 0;
        g_last_accept_code = -1;
        g_last_rollback_code = -1;
        rt = JS_NewRuntime();
        ASSERT_NE(rt, nullptr);
        ctx = JS_NewContext(rt);
        ASSERT_NE(ctx, nullptr);
        using namespace jshookz::provider::bindings;
        ASSERT_TRUE(registerResult(ctx));
        jshookz::qjs::OwnedValue global(ctx, JS_GetGlobalObject(ctx));
        ASSERT_TRUE(registerControl(ctx, global.get()));

        set("S_SEVEN", result_success(ctx, JS_NewInt32(ctx, 7)));
        set("S_ZERO", result_success(ctx, JS_NewInt32(ctx, 0)));
        set("S_BIGZERO", result_success(ctx, JS_NewBigInt64(ctx, 0)));
        set("S_EMPTY", result_success(ctx, JS_NewString(ctx, "")));
        set("S_FALSE", result_success(ctx, JS_FALSE));
        set("S_UNDEF", result_success(ctx, JS_UNDEFINED));
        set("S_NULL", result_success(ctx, JS_NULL));
        set("FAILED", result_failure(ctx, result_error(ctx, "test")));
        set("VOIDOK", effect_success(ctx));
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

    void
    set(char const *name, JSValue value)
    {
        jshookz::qjs::OwnedValue global(ctx, JS_GetGlobalObject(ctx));
        ASSERT_GE(JS_SetPropertyStr(ctx, global.get(), name, value), 0);
    }

    jshookz::qjs::OwnedValue
    eval(char const *src)
    {
        return jshookz::qjs::OwnedValue(
            ctx,
            JS_Eval(ctx, src, std::strlen(src), "<truth-table>",
                    JS_EVAL_TYPE_GLOBAL));
    }

    // The verb returned a value: no terminal, no exception.
    void
    expectReturn(char const *src, double expected)
    {
        auto out = eval(src);
        ASSERT_FALSE(out.isException()) << src;
        EXPECT_EQ(g_accepts, 0) << src;
        EXPECT_EQ(g_rollbacks, 0) << src;
        double got = 0;
        ASSERT_GE(JS_ToFloat64(ctx, &got, out.get()), 0) << src;
        EXPECT_EQ(got, expected) << src;
        reset();
    }

    void
    expectAccept(char const *src)
    {
        auto out = eval(src);
        ASSERT_FALSE(out.isException()) << src;
        EXPECT_EQ(g_accepts, 1) << src;
        EXPECT_EQ(g_rollbacks, 0) << src;
        reset();
    }

    void
    expectRollback(char const *src)
    {
        auto out = eval(src);
        ASSERT_FALSE(out.isException()) << src;
        EXPECT_EQ(g_rollbacks, 1) << src;
        EXPECT_EQ(g_accepts, 0) << src;
        reset();
    }

    void
    expectThrow(char const *src)
    {
        auto out = eval(src);
        EXPECT_TRUE(out.isException()) << src;
        JS_GetException(ctx);  // clear
        EXPECT_EQ(g_accepts, 0) << src;
        EXPECT_EQ(g_rollbacks, 0) << src;
        reset();
    }

    void
    reset()
    {
        g_accepts = 0;
        g_rollbacks = 0;
    }
};

// ---- rollback.requirePresent -----------------------------------------

TEST_F(ControlTruthTable, RequirePresentResultCarrier)
{
    expectRollback("rollback.requirePresent(FAILED, 'm')");
    expectRollback("rollback.requirePresent(S_UNDEF, 'm')");
    expectRollback("rollback.requirePresent(S_NULL, 'm')");
    expectReturn("rollback.requirePresent(S_ZERO, 'm')", 0);
    expectReturn("rollback.requirePresent(S_SEVEN, 'm')", 7);
    expectReturn("rollback.requirePresent(S_FALSE, 'm') ? 1 : 2", 2);
    expectReturn("rollback.requirePresent(S_EMPTY, 'm').length", 0);
}

TEST_F(ControlTruthTable, RequirePresentDirectCarrier)
{
    expectRollback("rollback.requirePresent(undefined, 'm')");
    expectRollback("rollback.requirePresent(null, 'm')");
    expectReturn("rollback.requirePresent(0, 'm')", 0);
    expectReturn("rollback.requirePresent(7, 'm')", 7);
    expectReturn("rollback.requirePresent(false, 'm') ? 1 : 2", 2);
}

// ---- rollback.requireTruthy ------------------------------------------

TEST_F(ControlTruthTable, RequireTruthyResultCarrier)
{
    expectRollback("rollback.requireTruthy(FAILED, 'm')");
    expectRollback("rollback.requireTruthy(S_UNDEF, 'm')");
    expectRollback("rollback.requireTruthy(S_NULL, 'm')");
    expectRollback("rollback.requireTruthy(S_ZERO, 'm')");
    expectRollback("rollback.requireTruthy(S_BIGZERO, 'm')");
    expectRollback("rollback.requireTruthy(S_EMPTY, 'm')");
    expectRollback("rollback.requireTruthy(S_FALSE, 'm')");
    expectReturn("rollback.requireTruthy(S_SEVEN, 'm')", 7);
}

TEST_F(ControlTruthTable, RequireTruthyDirectCarrier)
{
    expectRollback("rollback.requireTruthy(undefined, 'm')");
    expectRollback("rollback.requireTruthy(null, 'm')");
    expectRollback("rollback.requireTruthy(0, 'm')");
    expectRollback("rollback.requireTruthy(0n, 'm')");
    expectRollback("rollback.requireTruthy('', 'm')");
    expectRollback("rollback.requireTruthy(false, 'm')");
    expectRollback("rollback.requireTruthy(NaN, 'm')");
    expectReturn("rollback.requireTruthy(7, 'm')", 7);
}

// ---- accept.unlessPresent --------------------------------------------

TEST_F(ControlTruthTable, UnlessPresentResultCarrier)
{
    expectAccept("accept.unlessPresent(FAILED, 'm')");
    expectAccept("accept.unlessPresent(S_UNDEF, 'm')");
    expectAccept("accept.unlessPresent(S_NULL, 'm')");
    expectReturn("accept.unlessPresent(S_ZERO, 'm')", 0);
    expectReturn("accept.unlessPresent(S_SEVEN, 'm')", 7);
    expectReturn("accept.unlessPresent(S_FALSE, 'm') ? 1 : 2", 2);
}

TEST_F(ControlTruthTable, UnlessPresentDirectCarrier)
{
    expectAccept("accept.unlessPresent(undefined, 'm')");
    expectAccept("accept.unlessPresent(null, 'm')");
    expectReturn("accept.unlessPresent(0, 'm')", 0);
    expectReturn("accept.unlessPresent(7, 'm')", 7);
}

// ---- accept.unlessTruthy ---------------------------------------------

TEST_F(ControlTruthTable, UnlessTruthyResultCarrier)
{
    expectAccept("accept.unlessTruthy(FAILED, 'm')");
    expectAccept("accept.unlessTruthy(S_UNDEF, 'm')");
    expectAccept("accept.unlessTruthy(S_ZERO, 'm')");
    expectAccept("accept.unlessTruthy(S_EMPTY, 'm')");
    expectAccept("accept.unlessTruthy(S_FALSE, 'm')");
    expectReturn("accept.unlessTruthy(S_SEVEN, 'm')", 7);
}

TEST_F(ControlTruthTable, UnlessTruthyDirectCarrier)
{
    expectAccept("accept.unlessTruthy(undefined, 'm')");
    expectAccept("accept.unlessTruthy(0, 'm')");
    expectAccept("accept.unlessTruthy('', 'm')");
    expectAccept("accept.unlessTruthy(NaN, 'm')");
    expectReturn("accept.unlessTruthy(7, 'm')", 7);
}

// ---- onFail preserves every success ----------------------------------

TEST_F(ControlTruthTable, OnFailPreservesFalsySuccesses)
{
    expectReturn("rollback.onFail(S_ZERO, 'm')", 0);
    expectReturn("rollback.onFail(S_FALSE, 'm') ? 1 : 2", 2);
    expectReturn("rollback.onFail(S_EMPTY, 'm').length", 0);
    expectReturn("rollback.onFail(S_UNDEF, 'm') === undefined ? 1 : 2", 1);
    expectRollback("rollback.onFail(FAILED, 'm')");
}

TEST_F(ControlTruthTable, PresenceIdiomDoesNotFireOnFalsySuccess)
{
    // 0072:190-340 gate: onFail(success(0)) ?? rollback(...) must NOT
    // roll back — 0 is present.
    expectReturn("rollback.onFail(S_ZERO, 'm') ?? rollback('absent')", 0);
}

// ---- exact-void ineligibility and when -------------------------------

TEST_F(ControlTruthTable, VoidResultsThrowOnValueVerbs)
{
    expectThrow("rollback.requirePresent(VOIDOK, 'm')");
    expectThrow("rollback.requireTruthy(VOIDOK, 'm')");
    expectThrow("accept.unlessPresent(VOIDOK, 'm')");
    expectThrow("accept.unlessTruthy(VOIDOK, 'm')");
}

TEST_F(ControlTruthTable, WhenIsPlainBooleanDispatch)
{
    expectRollback("rollback.when(true, 'm')");
    expectAccept("accept.when(true, 'm')");
    auto out = eval("rollback.when(false, 'm'); accept.when(false, 'm'); 1");
    ASSERT_FALSE(out.isException());
    EXPECT_EQ(g_accepts, 0);
    EXPECT_EQ(g_rollbacks, 0);
}
