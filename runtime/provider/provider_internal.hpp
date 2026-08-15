#pragma once

#include <cstdint>

extern "C" {
#include "../../engine/quickjs/quickjs.h"
}

extern "C" bool register_cpp_types(JSContext* ctx);
extern "C" bool register_uint_types(JSContext* ctx);

#ifdef CONFIG_PROTOCOL_XDATA
extern "C" void register_protocol_functions(JSContext* ctx);
#endif

namespace jshookz::provider {

JSValue makeSTBlob(
    JSContext* ctx,
    std::uint8_t const* bytes,
    std::uint32_t length);
JSValue makeHash256(
    JSContext* ctx,
    std::uint8_t const* bytes,
    std::uint32_t length);
JSValue makeAccountID(
    JSContext* ctx,
    std::uint8_t const* bytes,
    std::uint32_t length);

bool registerBindings(JSContext* ctx);
bool installDeterministicSandbox(JSContext* ctx);
void setRandomSeed(std::uint32_t seed) noexcept;
void setCoverageEnabled(JSRuntime* runtime, JSContext* context, bool enabled);

}  // namespace jshookz::provider
