#pragma once

#include "../quickjs.hpp"

#include <cstdint>

namespace jshookz::provider::bindings {

JSValue host_success(JSContext* ctx, JSValue value);
JSValue host_failure(JSContext* ctx, std::int64_t code);
JSValue rich_from_bytes(
    JSContext* ctx,
    char const* typeName,
    std::uint8_t const* bytes,
    std::uint32_t length);

void registerControl(JSContext* ctx, JSValue global);
void registerHook(JSContext* ctx, JSValue global);
void registerLedger(JSContext* ctx, JSValue global);
void registerTrace(JSContext* ctx, JSValue global);
void registerState(JSContext* ctx, JSValue global);
void registerEmission(JSContext* ctx, JSValue global);
void registerLegacy(JSContext* ctx, JSValue global);

}  // namespace jshookz::provider::bindings
