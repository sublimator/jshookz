#pragma once

#include <quickjs.h>

#include <cstdint>

namespace jshookz::provider::types {

// Register the private certified owner plus STObject/STArray/iterator classes
// and the one per-runtime generated field-atom accelerator. This does not
// publish a public constructor or byte-decoding utility.
[[nodiscard]] bool registerObjectTypes(JSContext* ctx);

// Release registrar-owned atoms/storage before the owning runtime is freed.
void unregisterObjectTypes(JSRuntime* runtime) noexcept;

// Private groundwork entry used by the eventual object utilities and native
// tests. It copies the supplied contiguous bytes, certifies/indexes exactly
// once, and returns a provider-minted top-level STObject.
[[nodiscard]] JSValue makeCertifiedObjectCopy(
    JSContext* ctx, std::uint8_t const* bytes, std::uint32_t size);

[[nodiscard]] bool isSTObject(JSValueConst value) noexcept;
[[nodiscard]] bool isSTArray(JSValueConst value) noexcept;

}  // namespace jshookz::provider::types
