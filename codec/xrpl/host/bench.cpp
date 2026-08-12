/*
 * bench.cpp — Production-realistic benchmark for xahaud protocol WASM.
 *
 * Measures cold and snapshotted codec-provider instantiation:
 * Each iteration = fresh Store → instantiate (COW) → eval bytecode → destroy.
 *
 * Compares normal vs Wizered module. Reports:
 *   - ms/contract, contracts/sec
 *   - Total bytes decoded (from bytecode metadata)
 *   - Memory per instance (RSS delta)
 *
 * The host functions (host_log, host_digest_*) use real implementations
 * so the crypto actually works during _initialize (Feature hashing).
 */

#include <wasmtime.h>
#include <wasi.h>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <fstream>
#include <string>
#include <sys/resource.h>
#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#define JSHOOKZ_SHA256 CC_SHA256
#define JSHOOKZ_SHA512 CC_SHA512
#else
#include <openssl/sha.h>
#define JSHOOKZ_SHA256 SHA256
#define JSHOOKZ_SHA512 SHA512
#endif

static std::string read_file(const char *p) {
    std::ifstream f(p, std::ios::binary);
    if (!f.is_open()) { fprintf(stderr, "cannot open %s\n", p); exit(1); }
    return {std::istreambuf_iterator<char>(f), {}};
}

// --- Host function helpers ---

using HFn = wasm_trap_t *(*)(void *, wasmtime_caller_t *, const wasmtime_val_t *, size_t,
                              wasmtime_val_t *, size_t);

static uint8_t *caller_mem(wasmtime_caller_t *c, uint32_t off) {
    wasmtime_extern_t ext;
    wasmtime_caller_export_get(c, "memory", 6, &ext);
    return wasmtime_memory_data(wasmtime_caller_context(c), &ext.of.memory) + off;
}

// host_log: silent in benchmark mode
static wasm_trap_t *noop_void(void *e, wasmtime_caller_t *c, const wasmtime_val_t *a, size_t n,
                               wasmtime_val_t *r, size_t nr) { return nullptr; }

// host_digest_sha256: real implementation (needed for Feature init + base58)
static wasm_trap_t *sha256_fn(void *e, wasmtime_caller_t *c, const wasmtime_val_t *a, size_t n,
                               wasmtime_val_t *r, size_t nr) {
    auto *src = caller_mem(c, a[0].of.i32);
    auto *dst = caller_mem(c, a[2].of.i32);
    if (a[3].of.i32 >= 32) {
        JSHOOKZ_SHA256(src, a[1].of.i32, dst);
        r[0] = {.kind = WASMTIME_I32, .of = {.i32 = 32}};
    } else {
        r[0] = {.kind = WASMTIME_I32, .of = {.i32 = 0}};
    }
    return nullptr;
}

// host_digest_sha512: real implementation (needed for Feature init)
static wasm_trap_t *sha512_fn(void *e, wasmtime_caller_t *c, const wasmtime_val_t *a, size_t n,
                               wasmtime_val_t *r, size_t nr) {
    auto *src = caller_mem(c, a[0].of.i32);
    auto *dst = caller_mem(c, a[2].of.i32);
    if (a[3].of.i32 >= 64) {
        JSHOOKZ_SHA512(src, a[1].of.i32, dst);
        r[0] = {.kind = WASMTIME_I32, .of = {.i32 = 64}};
    } else {
        r[0] = {.kind = WASMTIME_I32, .of = {.i32 = 0}};
    }
    return nullptr;
}

// ripemd160: stub (not needed for decode/encode path)
static wasm_trap_t *ripemd_fn(void *e, wasmtime_caller_t *c, const wasmtime_val_t *a, size_t n,
                               wasmtime_val_t *r, size_t nr) {
    auto *dst = caller_mem(c, a[2].of.i32);
    if (a[3].of.i32 >= 20) { memset(dst, 0, 20); r[0] = {.kind = WASMTIME_I32, .of = {.i32 = 20}}; }
    else r[0] = {.kind = WASMTIME_I32, .of = {.i32 = 0}};
    return nullptr;
}

// __cxa_allocate_exception / __cxa_throw: fatal stubs
static wasm_trap_t *cxa_alloc(void *e, wasmtime_caller_t *c, const wasmtime_val_t *a, size_t n,
                               wasmtime_val_t *r, size_t nr) {
    r[0] = {.kind = WASMTIME_I32, .of = {.i32 = 0}};
    return nullptr;
}
static wasm_trap_t *cxa_throw(void *e, wasmtime_caller_t *c, const wasmtime_val_t *a, size_t n,
                               wasmtime_val_t *r, size_t nr) {
    return wasmtime_trap_new("C++ exception", 13);
}

static wasm_trap_t *raw_hook_unavailable_i64_fn(
    void *, wasmtime_caller_t *, const wasmtime_val_t *, size_t,
    wasmtime_val_t *results, size_t) {
    results[0] = {.kind = WASMTIME_I64, .of = {.i64 = -1}};
    return nullptr;
}

// --- Registration ---

static void reg(wasmtime_linker_t *l, const char *name, HFn fn,
                std::initializer_list<wasm_valkind_t> p,
                std::initializer_list<wasm_valkind_t> r) {
    wasm_valtype_vec_t pv, rv;
    wasm_valtype_vec_new_uninitialized(&pv, p.size());
    int i = 0; for (auto k : p) pv.data[i++] = wasm_valtype_new(k);
    wasm_valtype_vec_new_uninitialized(&rv, r.size());
    i = 0; for (auto k : r) rv.data[i++] = wasm_valtype_new(k);
    auto *ft = wasm_functype_new(&pv, &rv);
    wasmtime_linker_define_func(l, "env", 3, name, strlen(name), ft, fn, nullptr, nullptr);
    wasm_functype_delete(ft);
}

static void setup_linker(wasmtime_linker_t *linker) {
    wasmtime_linker_define_wasi(linker);
    reg(linker, "host_log", noop_void, {WASM_I32, WASM_I32}, {});
    reg(linker, "host_digest_sha256", sha256_fn, {WASM_I32, WASM_I32, WASM_I32, WASM_I32}, {WASM_I32});
    reg(linker, "host_digest_sha512", sha512_fn, {WASM_I32, WASM_I32, WASM_I32, WASM_I32}, {WASM_I32});
    reg(linker, "host_digest_ripemd160", ripemd_fn, {WASM_I32, WASM_I32, WASM_I32, WASM_I32}, {WASM_I32});
    reg(linker, "__cxa_allocate_exception", cxa_alloc, {WASM_I32}, {WASM_I32});
    reg(linker, "__cxa_throw", cxa_throw, {WASM_I32, WASM_I32, WASM_I32}, {});
    #include "generated/hook_raw_stubs.inc"
}

// --- Benchmark loop ---

using Clock = std::chrono::high_resolution_clock;

static long get_rss_kb() {
    struct rusage u; getrusage(RUSAGE_SELF, &u);
    return u.ru_maxrss / 1024;
}

static void bench(wasm_engine_t *engine, wasmtime_module_t *module,
                  const std::string &bc, int N, const char *label) {
    long rss_before = get_rss_kb();
    auto t0 = Clock::now();

    for (int i = 0; i < N; i++) {
        // Fresh store per contract — production pattern
        wasmtime_store_t *store = wasmtime_store_new(engine, nullptr, nullptr);
        wasmtime_context_t *ctx = wasmtime_store_context(store);
        wasi_config_t *wc = wasi_config_new();
        wasmtime_context_set_wasi(ctx, wc);

        wasmtime_linker_t *linker = wasmtime_linker_new(engine);
        setup_linker(linker);

        wasmtime_instance_t inst;
        wasm_trap_t *trap = nullptr;
        wasmtime_linker_instantiate(linker, ctx, module, &inst, &trap);

        // _initialize (runs C++ static constructors; no-op if Wizered)
        wasmtime_extern_t ext;
        if (wasmtime_instance_export_get(ctx, &inst, "_initialize", 11, &ext))
            wasmtime_func_call(ctx, &ext.of.func, nullptr, 0, nullptr, 0, &trap);

        // qjs_init (full init if normal, no-op if Wizered)
        wasmtime_instance_export_get(ctx, &inst, "qjs_init", 8, &ext);
        wasmtime_func_call(ctx, &ext.of.func, nullptr, 0, nullptr, 0, &trap);

        // malloc + write bytecode into WASM memory
        wasmtime_instance_export_get(ctx, &inst, "malloc", 6, &ext);
        wasmtime_val_t ma[] = {{.kind = WASMTIME_I32, .of = {.i32 = (int32_t)bc.size()}}};
        wasmtime_val_t mr[1];
        wasmtime_func_call(ctx, &ext.of.func, ma, 1, mr, 1, &trap);
        uint32_t ptr = mr[0].of.i32;

        wasmtime_extern_t mem_ext;
        wasmtime_instance_export_get(ctx, &inst, "memory", 6, &mem_ext);
        memcpy(wasmtime_memory_data(ctx, &mem_ext.of.memory) + ptr, bc.data(), bc.size());

        // eval bytecode
        wasmtime_instance_export_get(ctx, &inst, "qjs_eval_bytecode", 17, &ext);
        wasmtime_val_t ea[] = {
            {.kind = WASMTIME_I32, .of = {.i32 = (int32_t)ptr}},
            {.kind = WASMTIME_I32, .of = {.i32 = (int32_t)bc.size()}},
        };
        wasmtime_val_t er[1];
        wasmtime_func_call(ctx, &ext.of.func, ea, 2, er, 1, &trap);

        // destroy
        wasmtime_instance_export_get(ctx, &inst, "qjs_destroy", 11, &ext);
        wasmtime_func_call(ctx, &ext.of.func, nullptr, 0, nullptr, 0, &trap);

        wasmtime_linker_delete(linker);
        wasmtime_store_delete(store);
    }

    double total = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    long rss_after = get_rss_kb();
    printf("  %-20s N=%-4d  %.0fms total  %.3fms/contract  %.0f/sec  RSS: +%.1fMB\n",
           label, N, total, total / N, N / (total / 1000),
           (rss_after - rss_before) / 1024.0);
}

// --- AOT compilation helper ---

static wasmtime_module_t *compile_module(wasm_engine_t *engine, const char *path) {
    auto d = read_file(path);
    wasmtime_module_t *mod;

    // Cranelift AOT compilation: WASM → native code
    auto t0 = Clock::now();
    auto err = wasmtime_module_new(engine, (uint8_t *)d.data(), d.size(), &mod);
    if (err) {
        wasm_name_t msg; wasmtime_error_message(err, &msg);
        fprintf(stderr, "compile %s: %.*s\n", path, (int)msg.size, msg.data);
        wasm_byte_vec_delete(&msg); wasmtime_error_delete(err); exit(1);
    }
    double compile_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();

    // Serialize → deserialize (production path: compile once, cache to disk)
    wasm_byte_vec_t serialized;
    wasmtime_module_serialize(mod, &serialized);

    t0 = Clock::now();
    wasmtime_module_t *cached;
    wasmtime_module_deserialize(engine, (uint8_t *)serialized.data, serialized.size, &cached);
    double deser_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();

    printf("    Compile: %.0fms, Serialized: %zuKB, Deserialize: %.1fms\n",
           compile_ms, serialized.size/1024, deser_ms);

    wasmtime_module_delete(mod);
    wasm_byte_vec_delete(&serialized);
    return cached;  // Use the deserialized (production) module
}

int main(int argc, char **argv) {
    const char *normal_path = nullptr;
    const char *wizer_path = nullptr;
    const char *bc_path = nullptr;
    int N = 200;
    int hex_bytes = 0;  // total hex bytes per contract (for throughput calc)

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) N = atoi(argv[++i]);
        else if (strcmp(argv[i], "--normal") == 0 && i + 1 < argc) normal_path = argv[++i];
        else if (strcmp(argv[i], "--wizer") == 0 && i + 1 < argc) wizer_path = argv[++i];
        else if (strcmp(argv[i], "--bytecode") == 0 && i + 1 < argc) bc_path = argv[++i];
        else if (strcmp(argv[i], "--hex-bytes") == 0 && i + 1 < argc) hex_bytes = atoi(argv[++i]);
    }

    if (!bc_path) {
        fprintf(stderr, "usage: %s --bytecode <.qjsc> [--normal <.wasm>] [--wizer <.wasm>] [-n N] [--hex-bytes B]\n", argv[0]);
        return 1;
    }

    auto bc = read_file(bc_path);

    // Engine with AOT compilation (wasmtime compiles to native on module_new)
    wasm_engine_t *engine = wasm_engine_new();

    printf("═══ Xahaud Protocol WASM Benchmark ═══\n");
    printf("  Bytecode: %s (%zu bytes)\n", bc_path, bc.size());
    if (hex_bytes > 0)
        printf("  Hex payload: %d bytes/contract\n", hex_bytes);
    printf("  N=%d iterations per config\n\n", N);

    if (normal_path) {
        printf("Normal module: %s\n", normal_path);
        auto *mod = compile_module(engine, normal_path);
        bench(engine, mod, bc, N, "Normal:");
        wasmtime_module_delete(mod);
    }

    if (wizer_path) {
        printf("Wizered module: %s\n", wizer_path);
        auto *mod = compile_module(engine, wizer_path);
        bench(engine, mod, bc, N, "Wizered:");

        // Throughput summary
        if (hex_bytes > 0) {
            // Re-run to get clean timing
            auto t0 = Clock::now();
            for (int i = 0; i < N; i++) {
                wasmtime_store_t *store = wasmtime_store_new(engine, nullptr, nullptr);
                wasmtime_context_t *ctx = wasmtime_store_context(store);
                wasi_config_t *wc = wasi_config_new();
                wasmtime_context_set_wasi(ctx, wc);
                wasmtime_linker_t *linker = wasmtime_linker_new(engine);
                setup_linker(linker);
                wasmtime_instance_t inst;
                wasm_trap_t *trap = nullptr;
                wasmtime_linker_instantiate(linker, ctx, mod, &inst, &trap);
                wasmtime_extern_t ext;
                if (wasmtime_instance_export_get(ctx, &inst, "_initialize", 11, &ext))
                    wasmtime_func_call(ctx, &ext.of.func, nullptr, 0, nullptr, 0, &trap);
                wasmtime_instance_export_get(ctx, &inst, "qjs_init", 8, &ext);
                wasmtime_func_call(ctx, &ext.of.func, nullptr, 0, nullptr, 0, &trap);
                wasmtime_instance_export_get(ctx, &inst, "malloc", 6, &ext);
                wasmtime_val_t ma[] = {{.kind = WASMTIME_I32, .of = {.i32 = (int32_t)bc.size()}}};
                wasmtime_val_t mr[1];
                wasmtime_func_call(ctx, &ext.of.func, ma, 1, mr, 1, &trap);
                uint32_t ptr = mr[0].of.i32;
                wasmtime_extern_t mem_ext;
                wasmtime_instance_export_get(ctx, &inst, "memory", 6, &mem_ext);
                memcpy(wasmtime_memory_data(ctx, &mem_ext.of.memory) + ptr, bc.data(), bc.size());
                wasmtime_instance_export_get(ctx, &inst, "qjs_eval_bytecode", 17, &ext);
                wasmtime_val_t ea[] = {
                    {.kind = WASMTIME_I32, .of = {.i32 = (int32_t)ptr}},
                    {.kind = WASMTIME_I32, .of = {.i32 = (int32_t)bc.size()}},
                };
                wasmtime_val_t er[1];
                wasmtime_func_call(ctx, &ext.of.func, ea, 2, er, 1, &trap);
                wasmtime_instance_export_get(ctx, &inst, "qjs_destroy", 11, &ext);
                wasmtime_func_call(ctx, &ext.of.func, nullptr, 0, nullptr, 0, &trap);
                wasmtime_linker_delete(linker);
                wasmtime_store_delete(store);
            }
            double total_sec = std::chrono::duration<double>(Clock::now() - t0).count();
            double bytes_per_sec = (double)hex_bytes * N / total_sec;
            printf("\n  Throughput: %.0f bytes/sec (%.1f KB/sec) of serialized data\n",
                   bytes_per_sec, bytes_per_sec / 1024);
        }

        wasmtime_module_delete(mod);
    }

    wasm_engine_delete(engine);
    return 0;
}
