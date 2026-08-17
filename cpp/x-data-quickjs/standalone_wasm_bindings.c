// WASM bindings for the standalone XRPL/Xahau codec provider.
//
// Exports:
//   wizer.initialize   - Wizer pre-init: snapshot QuickJS + xahaud statics
//   qjs_init           - init runtime + register ripple_decode/encode
//   qjs_eval(ptr, len) - evaluate JS source code
//   qjs_eval_bytecode(ptr, len) - evaluate precompiled QJS bytecode
//   qjs_get_result_ptr/len - read last eval result
//   qjs_destroy        - tear down runtime
//   malloc / free      - memory management for host

#include "quickjs.h"
#include <string.h>
#include <stdlib.h>

// --- Host function imports ---

__attribute__((import_module("env"), import_name("host_log")))
extern void host_log(const char* ptr, uint32_t len);

// --- Globals ---

static JSRuntime* rt = NULL;
static JSContext* ctx = NULL;
static char* result_buf = NULL;
static uint32_t result_len = 0;

// --- Result storage helpers ---

static void clear_result(void)
{
    if (result_buf) { free(result_buf); result_buf = NULL; }
    result_len = 0;
}

static void store_result_str_impl(const char* str, int line)
{
    // Prepend "wasm_bindings.c:NNN: " for debugging
    char buf[16];
    int prefix_len = snprintf(buf, sizeof(buf), "L%d: ", line);
    size_t str_len = strlen(str);
    result_len = prefix_len + str_len;
    result_buf = (char*)malloc(result_len);
    memcpy(result_buf, buf, prefix_len);
    memcpy(result_buf + prefix_len, str, str_len);
}
#define store_result_str(s) store_result_str_impl((s), __LINE__)

static void store_exception(void)
{
    JSValue exc = JS_GetException(ctx);
    const char* str = JS_ToCString(ctx, exc);
    if (str) {
        store_result_str(str);
        JS_FreeCString(ctx, str);
    }
    JS_FreeValue(ctx, exc);
}

static void store_value(JSValue val)
{
    if (!JS_IsUndefined(val)) {
        const char* str = JS_ToCString(ctx, val);
        if (str) {
            // No debug prefix for values - only errors get it
            result_len = strlen(str);
            result_buf = (char*)malloc(result_len);
            memcpy(result_buf, str, result_len);
            JS_FreeCString(ctx, str);
        }
    }
}

// --- JS wrapper for host_log ---

static JSValue js_host_log(JSContext* ctx, JSValueConst this_val,
                           int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_UNDEFINED;
    const char* str = JS_ToCString(ctx, argv[0]);
    if (str) {
        host_log(str, strlen(str));
        JS_FreeCString(ctx, str);
    }
    return JS_UNDEFINED;
}

// --- Bridge function (implemented in bridge.cpp / bridge_xdata.cpp) ---

extern void register_protocol_functions(JSContext* ctx);

// --- Exports ---

// Initialize QuickJS runtime + context, register all JS globals.
// Idempotent - safe to call after Wizer snapshot.
__attribute__((export_name("qjs_init")))
void qjs_init(void)
{
    if (rt) return;

    rt = JS_NewRuntime();
    ctx = JS_NewContext(rt);

    // Register host_log
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "host_log",
        JS_NewCFunction(ctx, js_host_log, "host_log", 1));
    JS_FreeValue(ctx, global);

    // Register ripple_decode + ripple_encode from bridge.cpp
    register_protocol_functions(ctx);
}

// Wizer pre-initialization entry point.
// Called at build time to snapshot the fully initialized state:
// QuickJS runtime, context, registered functions, AND all xahaud
// static constructors (SField registry, Feature hashes, etc.)
__attribute__((export_name("wizer.initialize")))
void wizer_initialize(void)
{
    qjs_init();
}

// Evaluate JS source code string.
__attribute__((export_name("qjs_eval")))
int32_t qjs_eval(const char* code, uint32_t len)
{
    if (!ctx) qjs_init();
    clear_result();

    JSValue val = JS_Eval(ctx, code, len, "<eval>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(val)) {
        store_exception();
        JS_FreeValue(ctx, val);
        return -1;
    }
    store_value(val);
    JS_FreeValue(ctx, val);
    return 0;
}

// Evaluate precompiled QuickJS bytecode.
// The bytecode is produced by qjsbc or JS_WriteObject(JS_EVAL_FLAG_COMPILE_ONLY).
__attribute__((export_name("qjs_eval_bytecode")))
int32_t qjs_eval_bytecode(const uint8_t* buf, uint32_t buf_len)
{
    if (!ctx) qjs_init();
    clear_result();

    JSValue obj = JS_ReadObject(ctx, buf, buf_len, JS_READ_OBJ_BYTECODE);
    if (JS_IsException(obj)) {
        store_exception();
        JS_FreeValue(ctx, obj);
        return -1;
    }

    JSValue val = JS_EvalFunction(ctx, obj);
    // JS_EvalFunction frees obj
    if (JS_IsException(val)) {
        store_exception();
        JS_FreeValue(ctx, val);
        return -1;
    }
    store_value(val);
    JS_FreeValue(ctx, val);
    return 0;
}

__attribute__((export_name("qjs_get_result_ptr")))
const char* qjs_get_result_ptr(void) { return result_buf; }

__attribute__((export_name("qjs_get_result_len")))
uint32_t qjs_get_result_len(void) { return result_len; }

__attribute__((export_name("qjs_destroy")))
void qjs_destroy(void)
{
    clear_result();
    if (ctx) { JS_FreeContext(ctx); ctx = NULL; }
    if (rt) { JS_FreeRuntime(rt); rt = NULL; }
}
