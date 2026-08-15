/*
 * Host-facing QuickJS/Wasm lifecycle.
 *
 * JavaScript API bindings live in bindings/; deterministic runtime policy
 * lives in sandbox.cpp. This file owns only the exported provider ABI and the
 * state whose lifetime spans those calls.
 */

#include "provider_internal.hpp"
#include "bindings/hook_imports.hpp"
#include "quickjs.hpp"

#include <cstdlib>
#include <cstring>

#ifndef CONFIG_XAHAU_HOOK_PROVIDER
extern "C" {
__attribute__((import_module("env"), import_name("host_resolve_module")))
int32_t host_resolve_module(
    char const* namePtr,
    uint32_t nameLength,
    uint8_t* output,
    uint32_t outputLength);
}

static uint8_t module_loader_buf[65536];
#endif

/* ---- Exported WASM API ---- */

static JSRuntime *rt = NULL;
static JSContext *ctx = NULL;

static void
destroy_runtime(void)
{
    if (ctx) {
        JS_FreeContext(ctx);
        ctx = NULL;
    }
    if (rt) {
        JS_FreeRuntime(rt);
        rt = NULL;
    }
}

static JSValue
callbackInfo(JSContext *context, uint32_t rawFlags)
{
    using jshookz::provider::qjs::OwnedValue;

    OwnedValue info(context, JS_NewObject(context));
    if (info.isException())
        return info.release();
    if (JS_DefinePropertyValueStr(
            context,
            info.get(),
            "failed",
            JS_NewBool(context, (rawFlags & 1U) != 0),
            JS_PROP_ENUMERABLE) < 0)
        return JS_EXCEPTION;
    if (JS_DefinePropertyValueStr(
            context,
            info.get(),
            "rawFlags",
            JS_NewUint32(context, rawFlags),
            JS_PROP_ENUMERABLE) < 0)
        return JS_EXCEPTION;
    return info.release();
}

/* Buffer for returning result strings to the host */
/* RESULT_MAX — the consensus result cap (issue 0009, decided 2026-07-26).
 *
 * Host-visible ABI: qjs_get_result_ptr/len never expose more than this, and a
 * larger result fails loudly with a RangeError rather than being clamped. The
 * pre-M7 static slot clamped silently at 16KB, which handed the host
 * syntactically partial output with a success code; that is the bug this
 * replaced.
 *
 * Why 1 MiB, and why a cap at all:
 *   - This is NOT the primary memory bound. JS heap growth is already bounded
 *     by qjs_set_memory_limit, and isolation comes from one fresh Store per
 *     contract. The cap's job is narrower: never hand the host an unbounded
 *     read.
 *   - The result slot is the VALUE/STATUS channel, not the bulk channel. Bytes
 *     have a proper path (encode_object returns a Uint8Array the host reads
 *     from wasm memory directly). 1 MiB is far above any legitimate status,
 *     hash, error, or decoded-object JSON.
 *   - Deliberate consequence: hex of a maximum VL field (918744 bytes ->
 *     1,837,488 chars) exceeds this. That is the intended signal to use the
 *     bytes channel, not a bug to raise the cap for.
 *
 * FIXED, never host-configurable: a per-node knob would let two nodes disagree
 * about whether the same contract succeeded. Pinned on both sides of the
 * boundary by codec/xrpl/tests/test_provider_result_abi.py.
 */
enum { RESULT_MAX = 1048576 };

class ResultSlot
{
    char *owned_ = nullptr;
    char const *static_ = nullptr;
    std::uint32_t size_ = 0;

public:
    ~ResultSlot()
    {
        clear();
    }

    void
    clear() noexcept
    {
        if (owned_ != nullptr)
            free(owned_);
        owned_ = nullptr;
        static_ = nullptr;
        size_ = 0;
    }

    void
    storeStatic(char const *text) noexcept
    {
        clear();
        static_ = text;
        size_ = static_cast<std::uint32_t>(std::strlen(text));
    }

    [[nodiscard]] bool
    storeCopy(char const *text, std::size_t size) noexcept
    {
        auto *copy = static_cast<char *>(malloc(size + 1));
        if (copy == nullptr)
            return false;
        std::memcpy(copy, text, size);
        copy[size] = '\0';
        clear();
        owned_ = copy;
        size_ = static_cast<std::uint32_t>(size);
        return true;
    }

    [[nodiscard]] char const *
    data() const noexcept
    {
        if (owned_ != nullptr)
            return owned_;
        return static_ != nullptr ? static_ : "";
    }

    [[nodiscard]] std::uint32_t
    size() const noexcept
    {
        return size_;
    }
};

static ResultSlot resultSlot;

/* ---- On-demand module loader (resolves via host) ---- */

#ifndef CONFIG_XAHAU_HOOK_PROVIDER
static int is_hex_char(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* Normalize: validate "index:<64 hex>" format, strip prefix.
   Returns just the 64-char hex string as canonical module name. */
static char *module_normalize(JSContext *ctx, const char *base_name,
                               const char *name, void *opaque)
{
    /* Must start with "index:" */
    if (strncmp(name, "index:", 6) != 0) {
        JS_ThrowTypeError(ctx,
            "module specifier must be \"index:<64 hex chars>\", got \"%s\"", name);
        return NULL;
    }
    const char *hex = name + 6;
    if (strlen(hex) != 64) {
        JS_ThrowTypeError(ctx,
            "module index must be exactly 64 hex chars (32 bytes), got %zu", strlen(hex));
        return NULL;
    }
    for (int i = 0; i < 64; i++) {
        if (!is_hex_char(hex[i])) {
            JS_ThrowTypeError(ctx,
                "invalid hex char '%c' at position %d in module index", hex[i], i);
            return NULL;
        }
    }
    /* Return just the hex part — this is the cache key */
    return js_strdup(ctx, hex);
}

static JSModuleDef *wasm_module_loader(JSContext *ctx, const char *name,
                                        void *opaque)
{
    /* Ask the host for the module content.
       The host returns either:
       - JS source code (compiled here as module)
       - Precompiled bytecode (loaded directly)
       Detected by first byte: bytecode starts with 0x02 (QJS bytecode magic). */
    int32_t len = host_resolve_module(name, strlen(name), module_loader_buf,
                                      sizeof(module_loader_buf));
    if (len <= 0) {
        JS_ThrowReferenceError(ctx, "could not resolve module '%s'", name);
        return NULL;
    }

    JSValue func;
    if (len >= 1 && module_loader_buf[0] == 0x02) {
        /* Precompiled bytecode (starts with QJS bytecode header) */
        func = JS_ReadObject(ctx, module_loader_buf, (size_t)len,
                             JS_READ_OBJ_BYTECODE);
    } else {
        /* JS source — compile as module */
        func = JS_Eval(ctx, (const char *)module_loader_buf, (size_t)len, name,
                        JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    }

    if (JS_IsException(func))
        return NULL;

    JSModuleDef *m = static_cast<JSModuleDef *>(JS_VALUE_GET_PTR(func));
    return m;
}
#endif

__attribute__((export_name("qjs_init")))
void qjs_init(void)
{
    if (rt) return; /* already initialized (e.g. by Wizer snapshot) */
    rt = JS_NewRuntime();
    if (!rt)
        return;
#ifdef CONFIG_XAHAU_HOOK_PROVIDER
    ctx = JS_NewContextRaw(rt);
    if (!ctx ||
        JS_AddIntrinsicBaseObjects(ctx) ||
        JS_AddIntrinsicDate(ctx) ||
        /* The host compiler and ES-module evaluator use the Eval intrinsic.
           Dynamic code is deterministic and remains metered in profile v1;
           removing it is a separate language-policy choice. */
        JS_AddIntrinsicEval(ctx) ||
        JS_AddIntrinsicStringNormalize(ctx) ||
        JS_AddIntrinsicRegExp(ctx) ||
        JS_AddIntrinsicJSON(ctx) ||
        JS_AddIntrinsicProxy(ctx) ||
        JS_AddIntrinsicMapSet(ctx) ||
        JS_AddIntrinsicTypedArrays(ctx) ||
        /* QuickJS's ES-module evaluator uses the Promise machinery even for
           synchronous modules.  Hook entry points remain synchronous; the
           profile does not drain contract-scheduled jobs after termination. */
        JS_AddIntrinsicPromise(ctx)) {
        destroy_runtime();
        return;
    }
#else
    ctx = JS_NewContext(rt);
    if (!ctx) {
        destroy_runtime();
        return;
    }
#endif
#ifndef CONFIG_XAHAU_HOOK_PROVIDER
    JS_SetModuleLoaderFunc(rt, module_normalize, wasm_module_loader, NULL);
#endif
    bool initialized =
        jshookz::provider::registerBindings(ctx) &&
        register_cpp_types(ctx) &&
        register_uint_types(ctx);
#ifdef CONFIG_PROTOCOL_XDATA
    if (initialized) {
        register_protocol_functions(ctx);
        initialized = !JS_HasException(ctx);
    }
#endif
    if (initialized)
        initialized =
            jshookz::provider::installDeterministicSandbox(ctx);
    if (!initialized)
        destroy_runtime();
}

#ifdef CONFIG_XAHAU_HOOK_PROVIDER
/* JavaScript Date is Unix milliseconds.  ledger_last_time is the prior ledger
   close time in Ripple-epoch seconds (2000-01-01T00:00:00Z). */
extern "C" int64_t qjs_hook_date_now(void)
{
    static const int64_t ripple_epoch_unix_seconds = 946684800;
    int64_t const ledger_seconds = hook_ledger_last_time();
    if (ledger_seconds < 0 ||
        ledger_seconds > INT64_MAX / 1000 - ripple_epoch_unix_seconds)
        return 0;
    return (ledger_seconds + ripple_epoch_unix_seconds) * 1000;
}
#endif

/* Wizer pre-initialization entry point.
   Called at build time to snapshot the initialized QuickJS state. */
__attribute__((export_name("wizer.initialize")))
void wizer_initialize(void)
{
    qjs_init();
}

__attribute__((export_name("qjs_set_seed")))
void qjs_set_seed(uint32_t seed)
{
    /* Write the native seed cell. No re-eval: the sandbox patch is installed
       once at init, so seeding no longer re-wraps Date (issue 0030). */
    jshookz::provider::setRandomSeed(seed);
}

__attribute__((export_name("qjs_set_memory_limit")))
void qjs_set_memory_limit(uint32_t limit_bytes)
{
    JS_SetMemoryLimit(rt, limit_bytes);
}

__attribute__((export_name("qjs_set_max_stack_size")))
void qjs_set_max_stack_size(uint32_t stack_bytes)
{
    JS_SetMaxStackSize(rt, stack_bytes);
}

__attribute__((export_name("qjs_enable_coverage")))
void qjs_enable_coverage(int32_t enable)
{
    jshookz::provider::setCoverageEnabled(rt, ctx, enable != 0);
}

/* ---- Helpers ---- */

static void clear_result(void)
{
    resultSlot.clear();
}

static void store_static_result(const char *str)
{
    resultSlot.storeStatic(str);
}

static int store_result_str(const char *str)
{
    size_t len = strlen(str);

    if (len > RESULT_MAX) {
        store_static_result("RangeError: result exceeds 1048576 byte RESULT_MAX");
        return -1;
    }

    if (!resultSlot.storeCopy(str, len)) {
        store_static_result("Error: out of memory storing result");
        return -1;
    }
    return 0;
}

static int store_exception(void)
{
    jshookz::provider::qjs::OwnedValue exception(ctx, JS_GetException(ctx));
    const char *str = JS_ToCString(ctx, exception.get());
    int status = 0;
    if (str) {
        status = store_result_str(str);
        JS_FreeCString(ctx, str);
    }
    return status;
}

static int store_value(JSValue val)
{
    const char *str = JS_ToCString(ctx, val);
    if (str) {
        int status = store_result_str(str);
        JS_FreeCString(ctx, str);
        return status;
    }
    return 0;
}

/* ---- Eval / Compile / Load bytecode ---- */

__attribute__((export_name("qjs_eval")))
int32_t qjs_eval(const char *input, uint32_t input_len)
{
    clear_result();

    jshookz::provider::qjs::OwnedValue value(
        ctx,
        JS_Eval(ctx, input, input_len, "<contract>", JS_EVAL_TYPE_GLOBAL));
    if (value.isException()) {
        store_exception();
        return -1;
    }
    return store_value(value.get());
}

__attribute__((export_name("qjs_eval_module")))
int32_t qjs_eval_module(const char *input, uint32_t input_len)
{
    clear_result();

    jshookz::provider::qjs::OwnedValue value(
        ctx,
        JS_Eval(
            ctx, input, input_len, "<contract>", JS_EVAL_TYPE_MODULE));
    if (value.isException()) {
        store_exception();
        return -1;
    }
    return store_value(value.get());
}

/* Compile JS source to bytecode. Returns bytecode size, or -1 on error.
   The bytecode is stored in an internal buffer — retrieve with
   qjs_get_bytecode_ptr/len, then copy it out before next call. */
static uint8_t *bytecode_buf = NULL;
static uint32_t bytecode_len = 0;

static int32_t compile_source(
    const char *input, uint32_t input_len, int eval_type)
{
    clear_result();
    if (bytecode_buf) { js_free(ctx, bytecode_buf); bytecode_buf = NULL; }
    bytecode_len = 0;

    jshookz::provider::qjs::OwnedValue object(
        ctx,
        JS_Eval(
            ctx,
            input,
            input_len,
            "<contract>",
            eval_type | JS_EVAL_FLAG_COMPILE_ONLY));
    if (object.isException()) {
        store_exception();
        return -1;
    }

    size_t out_size;
    bytecode_buf = JS_WriteObject(
        ctx, &out_size, object.get(), JS_WRITE_OBJ_BYTECODE);

    if (!bytecode_buf) {
        store_exception();
        return -1;
    }

    bytecode_len = (uint32_t)out_size;
    return (int32_t)bytecode_len;
}

__attribute__((export_name("qjs_compile")))
int32_t qjs_compile(const char *input, uint32_t input_len)
{
    return compile_source(input, input_len, JS_EVAL_TYPE_GLOBAL);
}

__attribute__((export_name("qjs_compile_module")))
int32_t qjs_compile_module(const char *input, uint32_t input_len)
{
    return compile_source(input, input_len, JS_EVAL_TYPE_MODULE);
}

__attribute__((export_name("qjs_get_bytecode_ptr")))
const uint8_t *qjs_get_bytecode_ptr(void)
{
    return bytecode_buf;
}

__attribute__((export_name("qjs_get_bytecode_len")))
uint32_t qjs_get_bytecode_len(void)
{
    return bytecode_len;
}

/* Load and execute precompiled bytecode. */
__attribute__((export_name("qjs_eval_bytecode")))
int32_t qjs_eval_bytecode(const uint8_t *buf, uint32_t buf_len)
{
    clear_result();

    jshookz::provider::qjs::OwnedValue object(
        ctx, JS_ReadObject(ctx, buf, buf_len, JS_READ_OBJ_BYTECODE));
    if (object.isException()) {
        store_exception();
        return -1;
    }

    jshookz::provider::qjs::OwnedValue value(
        ctx, JS_EvalFunction(ctx, object.release()));
    if (value.isException()) {
        store_exception();
        return -1;
    }
    return store_value(value.get());
}

static int
finish_module_evaluation(jshookz::provider::qjs::OwnedValue evaluated)
{
    if (evaluated.isException()) {
        store_exception();
        return -1;
    }

    int promise_state = JS_PromiseState(ctx, evaluated.get());
    if (promise_state == JS_PROMISE_REJECTED) {
        jshookz::provider::qjs::OwnedValue reason(
            ctx, JS_PromiseResult(ctx, evaluated.get()));
        JS_Throw(ctx, reason.release());
        store_exception();
        return -1;
    }
    if (promise_state == JS_PROMISE_PENDING) {
        store_static_result(
            "TypeError: pending module initialization is not supported");
        return -1;
    }

    return 0;
}

static int32_t qjs_invoke_bytecode_export(
    const uint8_t *buf,
    uint32_t buf_len,
    const char *export_name,
    uint32_t reserved)
{
    clear_result();

    using jshookz::provider::qjs::OwnedValue;
    OwnedValue object(
        ctx, JS_ReadObject(ctx, buf, buf_len, JS_READ_OBJ_BYTECODE));
    if (object.isException()) {
        store_exception();
        return -1;
    }
    if (JS_VALUE_GET_TAG(object.get()) != JS_TAG_MODULE) {
        store_static_result("TypeError: Hook bytecode must contain an ES module");
        return -1;
    }

    JSModuleDef *module =
        static_cast<JSModuleDef *>(JS_VALUE_GET_PTR(object.get()));
    if (JS_ResolveModule(ctx, object.get()) < 0) {
        store_exception();
        return -1;
    }

    OwnedValue evaluated(ctx, JS_EvalFunction(ctx, object.release()));
    if (finish_module_evaluation(std::move(evaluated)) < 0)
        return -1;

    OwnedValue moduleNamespace(ctx, JS_GetModuleNamespace(ctx, module));
    if (moduleNamespace.isException()) {
        store_exception();
        return -1;
    }
    OwnedValue entry(
        ctx, JS_GetPropertyStr(ctx, moduleNamespace.get(), export_name));
    if (entry.isException()) {
        store_exception();
        return -1;
    }
    if (JS_IsUndefined(entry.get())) {
        if (std::strcmp(export_name, "callback") == 0)
            store_static_result(
                "TypeError: Hook module has no exported callback entry point");
        else
            store_static_result(
                "TypeError: Hook module has no exported main entry point");
        return -1;
    }
    if (!JS_IsFunction(ctx, entry.get())) {
        if (std::strcmp(export_name, "callback") == 0)
            store_static_result(
                "TypeError: exported callback entry point is not a function");
        else
            store_static_result(
                "TypeError: exported main entry point is not a function");
        return -1;
    }

    bool const is_callback = std::strcmp(export_name, "callback") == 0;
    OwnedValue argument(
        ctx, is_callback ? callbackInfo(ctx, reserved) : JS_UNDEFINED);
    if (argument.isException()) {
        store_exception();
        return -1;
    }
    JSValueConst argument_value = argument.get();
    JSValueConst *arguments = is_callback ? &argument_value : nullptr;
    OwnedValue result(
        ctx,
        JS_Call(
            ctx,
            entry.get(),
            JS_UNDEFINED,
            is_callback ? 1 : 0,
            arguments));
    if (result.isException()) {
        store_exception();
        return -1;
    }
    return store_value(result.get());
}

__attribute__((export_name("qjs_hook")))
int32_t qjs_hook(const uint8_t *buf, uint32_t buf_len, uint32_t reserved)
{
    return qjs_invoke_bytecode_export(buf, buf_len, "main", reserved);
}

__attribute__((export_name("qjs_cbak")))
int32_t qjs_cbak(const uint8_t *buf, uint32_t buf_len, uint32_t reserved)
{
    return qjs_invoke_bytecode_export(buf, buf_len, "callback", reserved);
}

/* Evaluate module initialization in the caller's disposable validation
   instance, but do not invoke either entry point. Result bits:
   1 = callable main export, 2 = callable callback export. */
__attribute__((export_name("qjs_validate_hook_module")))
int32_t qjs_validate_hook_module(const uint8_t *buf, uint32_t buf_len)
{
    clear_result();

    using jshookz::provider::qjs::OwnedValue;
    OwnedValue object(
        ctx, JS_ReadObject(ctx, buf, buf_len, JS_READ_OBJ_BYTECODE));
    if (object.isException()) {
        store_exception();
        return -1;
    }
    if (JS_VALUE_GET_TAG(object.get()) != JS_TAG_MODULE) {
        store_static_result("TypeError: Hook bytecode must contain an ES module");
        return -1;
    }

    JSModuleDef *module =
        static_cast<JSModuleDef *>(JS_VALUE_GET_PTR(object.get()));
    if (JS_ResolveModule(ctx, object.get()) < 0) {
        store_exception();
        return -1;
    }

    OwnedValue evaluated(ctx, JS_EvalFunction(ctx, object.release()));
    if (finish_module_evaluation(std::move(evaluated)) < 0)
        return -1;

    OwnedValue moduleNamespace(ctx, JS_GetModuleNamespace(ctx, module));
    if (moduleNamespace.isException()) {
        store_exception();
        return -1;
    }
    OwnedValue mainEntry(
        ctx, JS_GetPropertyStr(ctx, moduleNamespace.get(), "main"));
    OwnedValue callback(
        ctx, JS_GetPropertyStr(ctx, moduleNamespace.get(), "callback"));
    if (mainEntry.isException() || callback.isException()) {
        store_exception();
        return -1;
    }
    if (JS_IsUndefined(mainEntry.get())) {
        store_static_result(
            "TypeError: Hook module has no exported main entry point");
        return -1;
    }
    if (!JS_IsFunction(ctx, mainEntry.get())) {
        store_static_result(
            "TypeError: exported main entry point is not a function");
        return -1;
    }
    if (!JS_IsUndefined(callback.get()) &&
        !JS_IsFunction(ctx, callback.get())) {
        store_static_result(
            "TypeError: exported callback entry point is not a function");
        return -1;
    }
    return 1 | (!JS_IsUndefined(callback.get()) ? 2 : 0);
}

__attribute__((export_name("qjs_get_result_ptr")))
const char *qjs_get_result_ptr(void)
{
    return resultSlot.data();
}

__attribute__((export_name("qjs_get_result_len")))
uint32_t qjs_get_result_len(void)
{
    return resultSlot.size();
}

__attribute__((export_name("qjs_destroy")))
void qjs_destroy(void)
{
    clear_result();
    destroy_runtime();
}

/* wasi-sdk reactor CRT provides _initialize automatically */
