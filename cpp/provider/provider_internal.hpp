#pragma once

#include <cstdint>

#include <jshookz/quickjs.h>

extern "C" {
[[nodiscard]] bool register_cpp_types(JSContext* ctx);
[[nodiscard]] bool register_uint_types(JSContext* ctx);
}

namespace jshookz::provider {

[[nodiscard]] JSValue makeSTBlob(
    JSContext* ctx,
    std::uint8_t const* bytes,
    std::uint32_t length);
[[nodiscard]] JSValue makeHash256(
    JSContext* ctx,
    std::uint8_t const* bytes,
    std::uint32_t length);
[[nodiscard]] JSValue makeAccountID(
    JSContext* ctx,
    std::uint8_t const* bytes,
    std::uint32_t length);

[[nodiscard]] bool registerBindings(JSContext* ctx);
[[nodiscard]] bool installDeterministicSandbox(JSContext* ctx);
void setRandomSeed(std::uint32_t seed) noexcept;
void setCoverageEnabled(JSRuntime* runtime, JSContext* context, bool enabled);

}  // namespace jshookz::provider
