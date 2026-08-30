#pragma once

#include <quickjs.h>

#include <cstdint>

namespace jshookz::provider::types {

// Register the private certified owner plus STObject/STArray/iterator classes
// and the one per-runtime generated field-atom accelerator. This does not
// publish a public constructor or byte-decoding utility.
[[nodiscard]] bool registerObjectTypes(JSContext* ctx);

// Publish the real, non-user-constructible STObject prototype hierarchy built
// by registerObjectTypes. The generated globals are committed as one group.
[[nodiscard]] bool publishObjectTypes(JSContext* ctx, JSValueConst global);

// Release registrar-owned atoms/storage before the owning runtime is freed.
void unregisterObjectTypes(JSRuntime* runtime) noexcept;

// Resolve a borrowed atom through the one persistent generated field-name
// accelerator. This is allocation-free and accepts serialized names only.
[[nodiscard]] bool registeredFieldCode(
    JSAtom atom, std::uint32_t& code) noexcept;

// Private groundwork entry used by the eventual object utilities and native
// tests. It copies the supplied contiguous bytes, certifies/indexes exactly
// once, and returns a provider-minted top-level STObject.
[[nodiscard]] JSValue makeCertifiedObjectCopy(
    JSContext* ctx, std::uint8_t const* bytes, std::uint32_t size);

// Maximum contiguous serialized object accepted by the exact recursive
// certifier. Host-backed acquisition checks this before allocating its root
// byte owner.
[[nodiscard]] std::uint32_t certifiedObjectMaxBytes() noexcept;

// Trusted-host ingress used by otxn.object(). This consumes `bytes` on every
// path, certifies/indexes it exactly once, requires a complete generated
// Transaction format, and adopts it directly as the sole immutable root owner
// on success. Malformed or non-Transaction trusted bytes are an internal
// provider invariant failure; allocation failure remains exact QuickJS OOM.
[[nodiscard]] JSValue makeCertifiedTransactionOwned(
    JSContext* ctx, std::uint8_t* bytes, std::uint32_t size);

// Trusted-host ingress used by the concrete AccountRoot ledger lookup. This
// consumes `bytes` on every path and succeeds only when the exact generated
// AccountRoot format (including required fields and leaf refinements) is
// proven. Any other trusted host payload is an internal provider invariant
// failure rather than a generic STObject success.
[[nodiscard]] JSValue makeCertifiedAccountRootOwned(
    JSContext* ctx, std::uint8_t* bytes, std::uint32_t size);

// Trusted-host ingress for the concrete URIToken ledger lookup. Ownership and
// invariant semantics match makeCertifiedAccountRootOwned; only the required
// generated leaf format differs.
[[nodiscard]] JSValue makeCertifiedURITokenOwned(
    JSContext* ctx, std::uint8_t* bytes, std::uint32_t size);

// Private adapters behind the eventual util.validateObject,
// util.safeDecodeObject, and util.decodeObject publication. They enforce the
// exact ObjectBytes provenance union without executing JavaScript.
[[nodiscard]] JSValue validateObjectBytes(
    JSContext* ctx, JSValueConst input);
[[nodiscard]] JSValue safeDecodeObjectBytes(
    JSContext* ctx, JSValueConst input);
[[nodiscard]] JSValue decodeObjectBytes(
    JSContext* ctx, JSValueConst input);

[[nodiscard]] bool isSTObject(JSValueConst value) noexcept;
[[nodiscard]] bool isSTArray(JSValueConst value) noexcept;

// Allocation-free/no-JavaScript provenance proof used by borrowed child
// facades such as PathSet. The owner must be the private certified root and
// the complete range must lie inside its immutable byte domain.
[[nodiscard]] bool isCertifiedObjectRange(
    JSContext* ctx, JSValueConst owner, std::uint8_t const* bytes,
    std::uint32_t length) noexcept;

}  // namespace jshookz::provider::types
