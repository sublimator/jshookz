#include "quickjs.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

extern const void *js_wasm_allocator_raw_pointer(const void *ptr);
extern size_t js_wasm_allocator_physical_usable_size(const void *ptr);

#define CHECK(condition, code) \
    do {                       \
        if (!(condition))      \
            return (code);     \
    } while (0)

static int same_allocator_state(const JSMemoryUsage *left,
                                const JSMemoryUsage *right)
{
    return left->malloc_size == right->malloc_size &&
           left->malloc_count == right->malloc_count;
}

/*
 * Directly exercises the CONFIG_WASM_STANDALONE default allocator.  Native
 * QuickJS tests install their own JSMallocFunctions and therefore cannot
 * cover the provider's compact tail metadata or pinned wasi-libc behavior.
 */
__attribute__((export_name("quickjs_wasm_allocator_probe")))
int quickjs_wasm_allocator_probe(void)
{
    static const size_t logical_overhead = 16;
    static const size_t small_size = 37;
    JSRuntime *runtime = JS_NewRuntime();
    JSMemoryUsage baseline, current, before;
    void *pointer, *guard, *replacement;
    size_t index;

    CHECK(runtime != NULL, 1);
    JS_ComputeMemoryUsage(runtime, &baseline);

    /* Exact N+16 charge and malloc-grade alignment. */
    JS_SetMemoryLimit(runtime,
                      (size_t)baseline.malloc_size + small_size +
                          logical_overhead);
    pointer = js_malloc_rt(runtime, small_size);
    CHECK(pointer != NULL, 2);
    CHECK((uintptr_t)pointer % 16 == 0, 3);
    CHECK(js_malloc_usable_size_rt(runtime, pointer) == small_size, 4);
    CHECK(js_wasm_allocator_raw_pointer(pointer) == pointer, 29);
    CHECK(js_wasm_allocator_physical_usable_size(pointer) >=
              small_size + sizeof(size_t),
          30);
    CHECK(js_wasm_allocator_physical_usable_size(pointer) <
              small_size + logical_overhead,
          31);
    JS_ComputeMemoryUsage(runtime, &current);
    CHECK(current.malloc_size ==
              baseline.malloc_size + small_size + logical_overhead,
          5);
    CHECK(current.malloc_count == baseline.malloc_count + 1, 6);
    memset(pointer, 0xA5, small_size);
    js_free_rt(runtime, pointer);
    JS_ComputeMemoryUsage(runtime, &current);
    CHECK(same_allocator_state(&current, &baseline), 7);

    /* One byte below the exact logical limit rejects without publication. */
    JS_SetMemoryLimit(runtime,
                      (size_t)baseline.malloc_size + small_size +
                          logical_overhead - 1);
    pointer = js_malloc_rt(runtime, small_size);
    CHECK(pointer == NULL, 8);
    JS_ComputeMemoryUsage(runtime, &current);
    CHECK(same_allocator_state(&current, &baseline), 9);

    /* Both logical and physical addition overflow before allocation. */
    pointer = js_malloc_rt(runtime, SIZE_MAX);
    CHECK(pointer == NULL, 32);
    JS_ComputeMemoryUsage(runtime, &current);
    CHECK(same_allocator_state(&current, &baseline), 33);

    /* realloc(NULL, n) is exactly one charged allocation. */
    JS_SetMemoryLimit(runtime, SIZE_MAX);
    pointer = js_realloc_rt(runtime, NULL, small_size);
    CHECK(pointer != NULL, 34);
    CHECK((uintptr_t)pointer % 16 == 0, 35);
    CHECK(js_malloc_usable_size_rt(runtime, pointer) == small_size, 36);
    JS_ComputeMemoryUsage(runtime, &current);
    CHECK(current.malloc_size ==
              baseline.malloc_size + small_size + logical_overhead,
          37);
    CHECK(current.malloc_count == baseline.malloc_count + 1, 38);

    /* realloc(ptr, 0) frees and restores the exact baseline. */
    replacement = js_realloc_rt(runtime, pointer, 0);
    CHECK(replacement == NULL, 39);
    JS_ComputeMemoryUsage(runtime, &current);
    CHECK(same_allocator_state(&current, &baseline), 40);

    JS_SetMemoryLimit(runtime, SIZE_MAX);
    pointer = js_malloc_rt(runtime, 96);
    guard = js_malloc_rt(runtime, 96);
    CHECK(pointer != NULL && guard != NULL, 10);
    CHECK(js_malloc_usable_size_rt(runtime, pointer) == 96, 27);
    for (index = 0; index < 96; ++index)
        ((uint8_t *)pointer)[index] = (uint8_t)(index ^ 0x5A);
    CHECK(js_malloc_usable_size_rt(runtime, pointer) == 96, 28);

    /* The adjacent guard forces this growth to move across size classes. */
    JS_ComputeMemoryUsage(runtime, &before);
    replacement = js_realloc_rt(runtime, pointer, 8192);
    CHECK(replacement != NULL, 11);
    CHECK(replacement != pointer, 12);
    pointer = replacement;
    CHECK((uintptr_t)pointer % 16 == 0, 41);
    for (index = 0; index < 96; ++index)
        CHECK(((uint8_t *)pointer)[index] == (uint8_t)(index ^ 0x5A), 13);
    CHECK(js_malloc_usable_size_rt(runtime, pointer) == 8192, 14);
    JS_ComputeMemoryUsage(runtime, &current);
    CHECK(current.malloc_size == before.malloc_size - 96 + 8192, 15);
    CHECK(current.malloc_count == before.malloc_count, 16);

    /* Shrinking refunds the exact requested-size delta. */
    before = current;
    replacement = js_realloc_rt(runtime, pointer, 24);
    CHECK(replacement != NULL, 17);
    pointer = replacement;
    for (index = 0; index < 24; ++index)
        CHECK(((uint8_t *)pointer)[index] == (uint8_t)(index ^ 0x5A), 18);
    CHECK(js_malloc_usable_size_rt(runtime, pointer) == 24, 19);
    JS_ComputeMemoryUsage(runtime, &current);
    CHECK(current.malloc_size == before.malloc_size - 8192 + 24, 20);
    CHECK(current.malloc_count == before.malloc_count, 21);

    /* Logical limit rejection retains the old allocation exactly. */
    before = current;
    JS_SetMemoryLimit(runtime,
                      (size_t)before.malloc_size - 24 - logical_overhead +
                          128 + logical_overhead - 1);
    replacement = js_realloc_rt(runtime, pointer, 128);
    CHECK(replacement == NULL, 42);
    for (index = 0; index < 24; ++index)
        CHECK(((uint8_t *)pointer)[index] == (uint8_t)(index ^ 0x5A), 43);
    JS_ComputeMemoryUsage(runtime, &current);
    CHECK(same_allocator_state(&current, &before), 44);

    /* SIZE_MAX also rejects before touching the retained allocation. */
    JS_SetMemoryLimit(runtime, SIZE_MAX);
    replacement = js_realloc_rt(runtime, pointer, SIZE_MAX);
    CHECK(replacement == NULL, 45);
    CHECK(js_malloc_usable_size_rt(runtime, pointer) == 24, 46);
    JS_ComputeMemoryUsage(runtime, &current);
    CHECK(same_allocator_state(&current, &before), 47);

    /*
     * The module maximum is 32 MiB, so this reaches the underlying allocator
     * and forces its realloc to fail.  QuickJS must retain the old pointer,
     * bytes, logical accounting, and freeability.
     */
    before = current;
    replacement = js_realloc_rt(runtime, pointer, 64U * 1024U * 1024U);
    CHECK(replacement == NULL, 22);
    for (index = 0; index < 24; ++index)
        CHECK(((uint8_t *)pointer)[index] == (uint8_t)(index ^ 0x5A), 23);
    CHECK(js_malloc_usable_size_rt(runtime, pointer) == 24, 24);
    JS_ComputeMemoryUsage(runtime, &current);
    CHECK(same_allocator_state(&current, &before), 25);

    js_free_rt(runtime, pointer);
    js_free_rt(runtime, guard);
    JS_ComputeMemoryUsage(runtime, &current);
    CHECK(same_allocator_state(&current, &baseline), 26);
    JS_FreeRuntime(runtime);
    return 0;
}
