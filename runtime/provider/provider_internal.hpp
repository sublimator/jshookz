#pragma once

#include <cstdint>

extern "C" {
#include "../../engine/quickjs/quickjs.h"
}

extern "C" void register_cpp_types(JSContext* ctx);

#ifdef CONFIG_PROTOCOL_XDATA
extern "C" void register_protocol_functions(JSContext* ctx);
#endif

namespace jshookz::provider {

void registerBindings(JSContext* ctx);
void installDeterministicSandbox(JSContext* ctx);
void setRandomSeed(std::uint32_t seed) noexcept;
void setCoverageEnabled(JSRuntime* runtime, JSContext* context, bool enabled);

}  // namespace jshookz::provider
