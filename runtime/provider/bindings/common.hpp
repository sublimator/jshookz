#pragma once

#include "../quickjs.hpp"

#include <cstdint>

namespace jshookz::provider::bindings {

[[nodiscard]] bool registerResult(JSContext* ctx);
bool isResult(JSValueConst value) noexcept;
bool isEffectResult(JSValueConst value) noexcept;
JSValue result_success(JSContext* ctx, JSValue value);
JSValue uint_failure(
    JSContext* ctx,
    char const* issue,
    std::uint32_t bits);
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

}  // namespace jshookz::provider::bindings
