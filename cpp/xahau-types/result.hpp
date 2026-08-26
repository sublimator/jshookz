#pragma once

#include "quickjs.hpp"

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
JSValue xfl_failure(JSContext* ctx, char const* issue);
JSValue effect_success(JSContext* ctx);
JSValue result_error(JSContext* ctx, char const* domain);
JSValue result_failure(JSContext* ctx, JSValue error, bool effect = false);
bool result_finish(JSContext* ctx, JSValueConst result);

}  // namespace jshookz::provider::bindings
