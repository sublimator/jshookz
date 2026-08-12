/*
 * Minimal Wasmtime fixture host for the XRPL/Xahau codec providers.
 *
 * This host loads a WASM module containing QuickJS + xahaud's protocol
 * serialization layer, then runs a JS script that can call ripple_decode().
 *
 * Host-provided functions:
 *   - host_log(ptr, len)                        — console output
 *   - host_digest_{sha256,sha512,ripemd160}     — crypto hash delegation
 *   - __cxa_allocate_exception / __cxa_throw     — C++ exception ABI stubs
 *
 * The crypto functions are needed because xahaud's digest.h wraps OpenSSL,
 * which can't compile to WASM. Instead, the WASM-side stubs buffer the
 * input and call these host functions on finalize. The host uses macOS
 * CommonCrypto on macOS and OpenSSL on other supported hosts.
 *
 * The C++ exception stubs exist because wasi-sdk generates __cxa_throw
 * imports for throw/catch, but wasmtime doesn't support the WASM exception
 * handling proposal. In our setup, any C++ exception is fatal (trap).
 * The xahaud code uses exceptions for malformed input — a production
 * version would need proper EH support or pre-validation.
 */

#include <wasmtime.h>
#include <wasi.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#define JSHOOKZ_SHA256 CC_SHA256
#define JSHOOKZ_SHA512 CC_SHA512
#else
#include <openssl/sha.h>
#define JSHOOKZ_SHA256 SHA256
#define JSHOOKZ_SHA512 SHA512
#endif

// --- File I/O ---

static std::string read_file(const char *path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) { fprintf(stderr, "error: cannot open %s\n", path); exit(1); }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// --- Error handling ---

static void check(wasmtime_error_t *err, wasm_trap_t *trap, const char *step) {
    if (err) {
        wasm_name_t msg;
        wasmtime_error_message(err, &msg);
        fprintf(stderr, "error at %s: %.*s\n", step, (int)msg.size, msg.data);
        wasm_byte_vec_delete(&msg);
        wasmtime_error_delete(err);
        exit(1);
    }
    if (trap) {
        wasm_name_t msg;
        wasm_trap_message(trap, &msg);
        fprintf(stderr, "trap at %s: %.*s\n", step, (int)msg.size, msg.data);
        wasm_byte_vec_delete(&msg);
        wasm_trap_delete(trap);
        exit(1);
    }
}

// --- WASM memory access ---
//
// Host functions need to read/write WASM linear memory. During _initialize
// (C++ static constructor phase), the instance isn't fully set up yet, so
// we can't use a cached memory reference. Instead, host functions use
// wasmtime_caller_export_get to find memory dynamically. After init
// completes, we cache the memory for efficiency.

static wasmtime_memory_t wasm_memory;
static bool wasm_memory_set = false;

static uint8_t *mem_ptr_caller(wasmtime_caller_t *caller, uint32_t offset, uint32_t len) {
    wasmtime_memory_t mem;
    if (wasm_memory_set) {
        mem = wasm_memory;
    } else {
        wasmtime_extern_t ext;
        if (!wasmtime_caller_export_get(caller, "memory", 6, &ext) ||
            ext.kind != WASMTIME_EXTERN_MEMORY) {
            fprintf(stderr, "error: cannot find memory export\n");
            exit(1);
        }
        mem = ext.of.memory;
    }
    auto *ctx = wasmtime_caller_context(caller);
    return wasmtime_memory_data(ctx, &mem) + offset;
}

static uint8_t *mem_ptr(wasmtime_context_t *ctx, uint32_t offset, uint32_t len) {
    return wasmtime_memory_data(ctx, &wasm_memory) + offset;
}

static std::string mem_read_str(wasmtime_context_t *ctx, uint32_t ptr, uint32_t len) {
    return std::string((char *)mem_ptr(ctx, ptr, len), len);
}

// --- Host function implementations ---

#define HOST_FN(name) static wasm_trap_t *name(void *env, wasmtime_caller_t *caller, \
    const wasmtime_val_t *args, size_t nargs, wasmtime_val_t *results, size_t nresults)

// host_log: JS calls host_log("message") → prints to stdout
HOST_FN(host_log_fn) {
    auto *src = mem_ptr_caller(caller, args[0].of.i32, args[1].of.i32);
    printf("[contract] %.*s\n", (int)args[1].of.i32, (char*)src);
    return nullptr;
}

// --- Crypto host functions ---
//
// The xahaud digest classes (openssl_sha256_hasher etc.) are stubbed in
// digest_stub.cpp to buffer input, then call these host functions on
// finalize. This avoids compiling OpenSSL to WASM — the host provides
// the real crypto implementation.
//
// Signature: (in_ptr, in_len, out_ptr, out_len) → bytes_written

HOST_FN(host_digest_sha256_fn) {
    auto *src = mem_ptr_caller(caller, args[0].of.i32, args[1].of.i32);
    auto *dst = mem_ptr_caller(caller, args[2].of.i32, args[3].of.i32);
    if (args[3].of.i32 >= 32) {
        JSHOOKZ_SHA256(src, args[1].of.i32, dst);
        results[0] = {.kind = WASMTIME_I32, .of = {.i32 = 32}};
    } else {
        results[0] = {.kind = WASMTIME_I32, .of = {.i32 = 0}};
    }
    return nullptr;
}

HOST_FN(host_digest_sha512_fn) {
    auto *src = mem_ptr_caller(caller, args[0].of.i32, args[1].of.i32);
    auto *dst = mem_ptr_caller(caller, args[2].of.i32, args[3].of.i32);
    if (args[3].of.i32 >= 64) {
        JSHOOKZ_SHA512(src, args[1].of.i32, dst);
        results[0] = {.kind = WASMTIME_I32, .of = {.i32 = 64}};
    } else {
        results[0] = {.kind = WASMTIME_I32, .of = {.i32 = 0}};
    }
    return nullptr;
}

HOST_FN(host_digest_ripemd160_fn) {
    // macOS CommonCrypto doesn't have RIPEMD-160.
    // Returns zeros — only needed for calcAccountID (pubkey → account),
    // not for parsing or base58 display of existing account IDs.
    auto *dst = mem_ptr_caller(caller, args[2].of.i32, args[3].of.i32);
    if (args[3].of.i32 >= 20) {
        memset(dst, 0, 20);
        results[0] = {.kind = WASMTIME_I32, .of = {.i32 = 20}};
    } else {
        results[0] = {.kind = WASMTIME_I32, .of = {.i32 = 0}};
    }
    return nullptr;
}

// --- Generic runtime functions used only by the combined codec fixture ---

HOST_FN(host_resolve_module_fn) {
    results[0] = {.kind = WASMTIME_I32, .of = {.i32 = -1}};
    return nullptr;
}

HOST_FN(host_coverage_hit_fn) {
    return nullptr;
}

HOST_FN(host_get_balance_fn) {
    results[0] = {.kind = WASMTIME_I64, .of = {.i64 = 0}};
    return nullptr;
}

HOST_FN(host_get_state_fn) {
    results[0] = {.kind = WASMTIME_I32, .of = {.i32 = 0}};
    return nullptr;
}

HOST_FN(host_get_tx_info_fn) {
    results[0] = {.kind = WASMTIME_I32, .of = {.i32 = 0}};
    return nullptr;
}

HOST_FN(raw_hook_unavailable_i64_fn) {
    results[0] = {.kind = WASMTIME_I64, .of = {.i64 = -1}};
    return nullptr;
}

HOST_FN(host_util_sha256_fn) {
    return host_digest_sha256_fn(env, caller, args, nargs, results, nresults);
}

HOST_FN(host_util_sha512h_fn) {
    auto *src = mem_ptr_caller(caller, args[0].of.i32, args[1].of.i32);
    auto *dst = mem_ptr_caller(caller, args[2].of.i32, args[3].of.i32);
    if (args[3].of.i32 >= 32) {
        uint8_t full[64];
        JSHOOKZ_SHA512(src, args[1].of.i32, full);
        memcpy(dst, full, 32);
        results[0] = {.kind = WASMTIME_I32, .of = {.i32 = 32}};
    } else {
        results[0] = {.kind = WASMTIME_I32, .of = {.i32 = 0}};
    }
    return nullptr;
}

HOST_FN(host_util_ripemd160_fn) {
    return host_digest_ripemd160_fn(env, caller, args, nargs, results, nresults);
}

// --- C++ exception ABI stubs ---
//
// wasi-sdk compiles C++ throw/catch to __cxa_allocate_exception + __cxa_throw
// imports. wasmtime doesn't support the WASM exception handling proposal, so
// these end up as unresolved imports. We provide minimal stubs:
//   - allocate: returns null (exception object is never used)
//   - throw: traps the WASM module (unhandled exception = fatal)

HOST_FN(cxa_allocate_exception_fn) {
    results[0] = {.kind = WASMTIME_I32, .of = {.i32 = 0}};
    return nullptr;
}

HOST_FN(cxa_throw_fn) {
    fprintf(stderr, "[host] unhandled C++ exception in WASM module\n");
    return wasmtime_trap_new("unhandled C++ exception", strlen("unhandled C++ exception"));
}

// --- Registration helper ---

using HostFnPtr = wasm_trap_t *(*)(void *, wasmtime_caller_t *, const wasmtime_val_t *, size_t,
                                    wasmtime_val_t *, size_t);

static void reg(wasmtime_linker_t *linker, const char *name, HostFnPtr fn,
                std::initializer_list<wasm_valkind_t> params,
                std::initializer_list<wasm_valkind_t> rets) {
    wasm_valtype_vec_t pv, rv;
    wasm_valtype_vec_new_uninitialized(&pv, params.size());
    int i = 0; for (auto k : params) pv.data[i++] = wasm_valtype_new(k);
    wasm_valtype_vec_new_uninitialized(&rv, rets.size());
    i = 0; for (auto k : rets) rv.data[i++] = wasm_valtype_new(k);
    wasm_functype_t *ft = wasm_functype_new(&pv, &rv);
    check(wasmtime_linker_define_func(linker, "env", 3, name, strlen(name), ft, fn, nullptr, nullptr),
          nullptr, name);
    wasm_functype_delete(ft);
}

// --- Main ---

int main(int argc, char **argv) {
    const char *wasm_path = nullptr;
    const char *script_path = nullptr;
    uint64_t gas_limit = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--wasm") == 0 && i + 1 < argc) wasm_path = argv[++i];
        else if (strcmp(argv[i], "--script") == 0 && i + 1 < argc) script_path = argv[++i];
        else if (strcmp(argv[i], "--gas") == 0 && i + 1 < argc) gas_limit = strtoull(argv[++i], nullptr, 10);
    }

    if (!wasm_path || !script_path) {
        fprintf(stderr, "usage: %s --wasm <path.wasm> --script <path.js> [--gas fuel]\n", argv[0]);
        return 1;
    }

    auto wasm_bytes = read_file(wasm_path);
    auto script = read_file(script_path);

    // --- Engine + store ---
    wasm_engine_t *engine = nullptr;
    if (gas_limit > 0) {
        wasm_config_t *config = wasm_config_new();
        wasmtime_config_consume_fuel_set(config, true);
        engine = wasm_engine_new_with_config(config);
    } else {
        engine = wasm_engine_new();
    }
    wasmtime_store_t *store = wasmtime_store_new(engine, nullptr, nullptr);
    wasmtime_context_t *ctx = wasmtime_store_context(store);
    if (gas_limit > 0) {
        check(wasmtime_context_set_fuel(ctx, gas_limit), nullptr, "set_fuel");
    }

    // --- Linker: WASI + host functions ---
    wasmtime_linker_t *linker = wasmtime_linker_new(engine);
    check(wasmtime_linker_define_wasi(linker), nullptr, "wasi");

    wasi_config_t *wasi_config = wasi_config_new();
    check(wasmtime_context_set_wasi(ctx, wasi_config), nullptr, "set_wasi");

    reg(linker, "host_log", host_log_fn,
        {WASM_I32, WASM_I32}, {});
    reg(linker, "host_digest_sha256", host_digest_sha256_fn,
        {WASM_I32, WASM_I32, WASM_I32, WASM_I32}, {WASM_I32});
    reg(linker, "host_digest_sha512", host_digest_sha512_fn,
        {WASM_I32, WASM_I32, WASM_I32, WASM_I32}, {WASM_I32});
    reg(linker, "host_digest_ripemd160", host_digest_ripemd160_fn,
        {WASM_I32, WASM_I32, WASM_I32, WASM_I32}, {WASM_I32});
    reg(linker, "host_resolve_module", host_resolve_module_fn,
        {WASM_I32, WASM_I32, WASM_I32, WASM_I32}, {WASM_I32});
    reg(linker, "host_coverage_hit", host_coverage_hit_fn,
        {WASM_I32, WASM_I32, WASM_I32}, {});
    reg(linker, "host_get_balance", host_get_balance_fn,
        {WASM_I32, WASM_I32}, {WASM_I64});
    reg(linker, "host_get_state", host_get_state_fn,
        {WASM_I32, WASM_I32, WASM_I32, WASM_I32}, {WASM_I32});
    reg(linker, "host_get_tx_info", host_get_tx_info_fn,
        {WASM_I32, WASM_I32}, {WASM_I32});
    reg(linker, "host_util_sha256", host_util_sha256_fn,
        {WASM_I32, WASM_I32, WASM_I32, WASM_I32}, {WASM_I32});
    reg(linker, "host_util_sha512h", host_util_sha512h_fn,
        {WASM_I32, WASM_I32, WASM_I32, WASM_I32}, {WASM_I32});
    reg(linker, "host_util_ripemd160", host_util_ripemd160_fn,
        {WASM_I32, WASM_I32, WASM_I32, WASM_I32}, {WASM_I32});
    reg(linker, "__cxa_allocate_exception", cxa_allocate_exception_fn,
        {WASM_I32}, {WASM_I32});
    reg(linker, "__cxa_throw", cxa_throw_fn,
        {WASM_I32, WASM_I32, WASM_I32}, {});
    #include "generated/hook_raw_stubs.inc"

    // --- Compile + instantiate ---
    wasmtime_module_t *module = nullptr;
    check(wasmtime_module_new(engine, (uint8_t *)wasm_bytes.data(), wasm_bytes.size(), &module),
          nullptr, "compile");

    wasmtime_instance_t instance;
    wasm_trap_t *trap = nullptr;
    check(wasmtime_linker_instantiate(linker, ctx, module, &instance, &trap), trap, "instantiate");

    // --- _initialize: run C++ static constructors ---
    //
    // CRITICAL: In WASI reactor mode, _initialize runs all static constructors.
    // Without this, the SField registry (knownCodeToField) is empty and
    // STObject deserialization fails — every field comes back as "unknown".
    //
    // During _initialize, Feature.cpp registers ~100 amendment features by
    // computing SHA-512-Half hashes of their names — which calls our
    // host_digest_sha512 function. This is why host functions must work
    // before the instance is fully set up (using caller-based memory lookup).
    {
        wasmtime_extern_t ext;
        if (wasmtime_instance_export_get(ctx, &instance, "_initialize", 11, &ext) &&
            ext.kind == WASMTIME_EXTERN_FUNC) {
            check(wasmtime_func_call(ctx, &ext.of.func, nullptr, 0, nullptr, 0, &trap),
                  trap, "_initialize");
        } else {
            fprintf(stderr, "warning: _initialize not found\n");
        }
    }

    // --- Find exports ---
    auto find_func = [&](const char *name, wasmtime_func_t *out) {
        wasmtime_extern_t ext;
        if (!wasmtime_instance_export_get(ctx, &instance, name, strlen(name), &ext) ||
            ext.kind != WASMTIME_EXTERN_FUNC) {
            fprintf(stderr, "export not found: %s\n", name);
            exit(1);
        }
        *out = ext.of.func;
    };

    wasmtime_func_t fn_init, fn_eval, fn_result_ptr, fn_result_len, fn_destroy, fn_malloc;
    find_func("qjs_init", &fn_init);
    find_func("qjs_eval", &fn_eval);
    find_func("qjs_get_result_ptr", &fn_result_ptr);
    find_func("qjs_get_result_len", &fn_result_len);
    find_func("qjs_destroy", &fn_destroy);
    find_func("malloc", &fn_malloc);

    // Cache memory reference for post-init use
    {
        wasmtime_extern_t ext;
        if (!wasmtime_instance_export_get(ctx, &instance, "memory", 6, &ext) ||
            ext.kind != WASMTIME_EXTERN_MEMORY) {
            fprintf(stderr, "memory export not found\n"); exit(1);
        }
        wasm_memory = ext.of.memory;
        wasm_memory_set = true;
    }

    // --- Initialize QuickJS + register ripple_decode ---
    check(wasmtime_func_call(ctx, &fn_init, nullptr, 0, nullptr, 0, &trap), trap, "qjs_init");

    // --- Copy JS script into WASM memory and evaluate ---
    // Allocate size+1 and null-terminate: QuickJS's lexer can read one byte
    // past buf_end in certain code paths (e.g. checking for multi-char tokens).
    // Without the null terminator, whatever byte follows the script in WASM
    // memory gets interpreted as source, causing erratic SyntaxErrors.
    wasmtime_val_t args[2], results[1];
    args[0] = {.kind = WASMTIME_I32, .of = {.i32 = (int32_t)(script.size() + 1)}};
    check(wasmtime_func_call(ctx, &fn_malloc, args, 1, results, 1, &trap), trap, "malloc");
    uint32_t code_ptr = results[0].of.i32;
    memcpy(wasmtime_memory_data(ctx, &wasm_memory) + code_ptr, script.data(), script.size());
    *(wasmtime_memory_data(ctx, &wasm_memory) + code_ptr + script.size()) = '\0';

    args[0] = {.kind = WASMTIME_I32, .of = {.i32 = (int32_t)code_ptr}};
    args[1] = {.kind = WASMTIME_I32, .of = {.i32 = (int32_t)script.size()}};
    uint64_t fuel_before = 0;
    uint64_t fuel_after = 0;
    if (gas_limit > 0) {
        check(wasmtime_context_get_fuel(ctx, &fuel_before), nullptr, "fuel_before");
    }
    check(wasmtime_func_call(ctx, &fn_eval, args, 2, results, 1, &trap), trap, "qjs_eval");
    if (gas_limit > 0) {
        check(wasmtime_context_get_fuel(ctx, &fuel_after), nullptr, "fuel_after");
        printf("[gas] %llu\n", (unsigned long long)(fuel_before - fuel_after));
    }
    int32_t eval_status = results[0].of.i32;

    // --- Read result ---
    check(wasmtime_func_call(ctx, &fn_result_ptr, nullptr, 0, results, 1, &trap), trap, "result_ptr");
    uint32_t rptr = results[0].of.i32;
    check(wasmtime_func_call(ctx, &fn_result_len, nullptr, 0, results, 1, &trap), trap, "result_len");
    uint32_t rlen = results[0].of.i32;

    if (rlen > 0) {
        auto result_str = mem_read_str(ctx, rptr, rlen);
        if (eval_status != 0)
            fprintf(stderr, "[error] %s\n", result_str.c_str());
        else
            printf("[result] %s\n", result_str.c_str());
    }

    // --- Cleanup ---
    check(wasmtime_func_call(ctx, &fn_destroy, nullptr, 0, nullptr, 0, &trap), trap, "destroy");
    wasmtime_module_delete(module);
    wasmtime_linker_delete(linker);
    wasmtime_store_delete(store);
    wasm_engine_delete(engine);

    return eval_status != 0 ? 1 : 0;
}
