#include "provider_internal.hpp"

#include <math.h>
#include <string.h>

namespace jshookz::provider {
namespace {

/* ---- Deterministic PRNG: native seed cell (issue 0030) ----
 *
 * The seed lives in C++, not in a JS closure and not on globalThis. The
 * arithmetic deliberately mirrors ECMAScript double semantics: the multiply
 * loses precision before the mask, and the seeded-sequence tests are the
 * compatibility oracle.
 */
static uint32_t prng_seed = 42;

static uint32_t
js_to_uint32(double d)
{
    if (!isfinite(d))
        return 0;
    double m = fmod(trunc(d), 4294967296.0);
    if (m < 0)
        m += 4294967296.0;
    return (uint32_t)m;
}

static JSValue
js_prng_random(JSContext *ctx, JSValueConst this_val,
               int argc, JSValueConst *argv)
{
    /* Mirrors: seed = (seed * 1103515245 + 12345) & 0x7fffffff */
    double product = (double)prng_seed * 1103515245.0 + 12345.0;
    prng_seed = js_to_uint32(product) & 0x7fffffffu;
    return JS_NewFloat64(ctx, (double)prng_seed / 2147483647.0);
}

static void
sandbox_make_deterministic(JSContext *ctx)
{
#ifdef CONFIG_XAHAU_HOOK_PROVIDER
    /* The underlying Date intrinsic reads the previous ledger close time and
       is UTC.  Do not wrap it: wrapping leaves constructor escapes and breaks
       ordinary explicit Date arguments. */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue math = JS_GetPropertyStr(ctx, global, "Math");
    JSAtom random = JS_NewAtom(ctx, "random");
    JS_DeleteProperty(ctx, math, random, 0);
    JS_FreeAtom(ctx, random);
    JS_FreeValue(ctx, math);

    /* The v1 Hook profile has no shared memory, Atomics, weak references, or
       finalizer scheduling.  WeakRef intrinsics are never installed; remove
       the shared-memory globals created with the TypedArray intrinsic.
       Promise remains available because ES-module evaluation requires it. */
    static const char *const disabled[] = {
        "SharedArrayBuffer", "Atomics"};
    for (size_t i = 0; i < sizeof(disabled) / sizeof(disabled[0]); ++i) {
        JSAtom atom = JS_NewAtom(ctx, disabled[i]);
        JS_DeleteProperty(ctx, global, atom, 0);
        JS_FreeAtom(ctx, atom);
    }
    JS_FreeValue(ctx, global);
#else
    /* Installed exactly once, from qjs_init. Anything non-deterministic that
       can be replaced in JS goes in this patch; Math.random is installed
       natively below because its seed is host state, not contract state.
       For smart contracts, the host should seed per-transaction via
       qjs_set_seed; the default seed keeps things deterministic if it does
       not. */
    const char *patch =
        "(function() {"
        "  /* Date.now and new Date() return fixed epoch */"
        "  Date.now = function() { return 0; };"
        "  Date = (function(OrigDate) {"
        "    function FrozenDate() { return new OrigDate(0); }"
        "    FrozenDate.now = function() { return 0; };"
        "    FrozenDate.parse = OrigDate.parse;"
        "    FrozenDate.UTC = OrigDate.UTC;"
        "    FrozenDate.prototype = OrigDate.prototype;"
        "    return FrozenDate;"
        "  })(Date);"
        "})();";

    JSValue r = JS_Eval(ctx, patch, strlen(patch), "<sandbox>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(ctx, r);

    /* Math.random is native and reads the seed cell — installed once. */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue math = JS_GetPropertyStr(ctx, global, "Math");
    JS_SetPropertyStr(ctx, math, "random",
        JS_NewCFunction(ctx, js_prng_random, "random", 0));
    JS_FreeValue(ctx, math);
    JS_FreeValue(ctx, global);
#endif
}

#ifndef CONFIG_XAHAU_HOOK_PROVIDER
extern "C" {
__attribute__((import_module("env"), import_name("host_coverage_hit")))
void host_coverage_hit(const char *filename_ptr, uint32_t filename_len,
                       uint32_t line_num);
}

static int coverage_enabled = 0;

static int
coverage_interrupt_handler(JSRuntime *rt, void *opaque)
{
    JSContext *ctx = (JSContext *)opaque;
    const char *filename = NULL;
    int line = JS_GetCurrentLineNumber(ctx, &filename);
    if (line > 0 && filename) {
        host_coverage_hit(filename, strlen(filename), (uint32_t)line);
        JS_FreeCString(ctx, filename);
    }
    return 0; /* 0 = continue execution */
}
#endif

}  // namespace

void
installDeterministicSandbox(JSContext *ctx)
{
    sandbox_make_deterministic(ctx);
}

void
setRandomSeed(uint32_t seed) noexcept
{
    prng_seed = seed & 0x7fffffffu;
}

void
setCoverageEnabled(JSRuntime *rt, JSContext *ctx, bool enabled)
{
#ifdef CONFIG_XAHAU_HOOK_PROVIDER
    (void)rt;
    (void)ctx;
    (void)enabled;
#else
    coverage_enabled = enabled;
    if (enabled) {
        /* fire interrupt handler every instruction.
           Set both the reload value AND the current counter so it
           fires immediately (not after 10000 more instructions). */
        JS_SetInterruptCounterInit(rt, 1);
        JS_SetInterruptHandler(rt, coverage_interrupt_handler, ctx);
    } else {
        /* restore default */
        JS_SetInterruptCounterInit(rt, 10000);
        JS_SetInterruptHandler(rt, NULL, NULL);
    }
#endif
}

}  // namespace jshookz::provider
