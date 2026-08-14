#pragma once

#include <cstdint>

extern "C" {
#include "../../../engine/quickjs/quickjs.h"
}

namespace jshookz::provider::bindings {

struct BytesInput
{
    std::uint8_t const* ptr = nullptr;
    std::uint32_t len = 0;
    std::uint8_t* owned = nullptr;
    char const* cstring = nullptr;
    JSValue ab_ref = JS_UNDEFINED;
};

int get_bytes_input(JSContext* ctx, JSValueConst value, BytesInput* out);
int get_hook_input(JSContext* ctx, JSValueConst value, BytesInput* out);
void free_bytes_input(JSContext* ctx, BytesInput* input);

JSValue make_uint8array(
    JSContext* ctx,
    std::uint8_t const* data,
    std::uint32_t length);
JSValue host_success(JSContext* ctx, JSValue value);
JSValue host_failure(JSContext* ctx, std::int64_t code);
JSValue rich_from_bytes(
    JSContext* ctx,
    char const* typeName,
    std::uint8_t const* bytes,
    std::uint32_t length);

void registerControl(JSContext* ctx, JSValue global);
void registerLifecycle(JSContext* ctx, JSValue global);
void registerLedger(JSContext* ctx, JSValue global);
void registerTrace(JSContext* ctx, JSValue global);
void registerState(JSContext* ctx, JSValue global);
void registerEmission(JSContext* ctx, JSValue global);
void registerLegacy(JSContext* ctx, JSValue global);

}  // namespace jshookz::provider::bindings
