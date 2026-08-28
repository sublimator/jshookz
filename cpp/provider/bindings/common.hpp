#pragma once

#include "result.hpp"

#include <cstdint>

namespace jshookz::provider::bindings {

JSValue host_success(JSContext* ctx, JSValue value);
JSValue host_failure(JSContext* ctx, std::int64_t code);
JSValue host_effect_success(JSContext* ctx);
JSValue host_effect_failure(JSContext* ctx, std::int64_t code);
[[nodiscard]] bool registerControl(JSContext* ctx, JSValue global);
[[nodiscard]] bool registerHook(JSContext* ctx, JSValue global);
[[nodiscard]] bool registerLedger(JSContext* ctx, JSValue global);
[[nodiscard]] bool registerTrace(JSContext* ctx, JSValue global);
[[nodiscard]] bool registerState(JSContext* ctx, JSValue global);
[[nodiscard]] bool registerEmission(JSContext* ctx, JSValue global);
[[nodiscard]] bool registerLegacy(JSContext* ctx, JSValue global);
void resetOriginatingTransactionCache(JSContext* ctx) noexcept;

}  // namespace jshookz::provider::bindings
