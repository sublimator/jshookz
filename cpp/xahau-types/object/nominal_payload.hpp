#pragma once

#include <catl/xdata/static_protocol.h>
#include <quickjs.h>

#include <cstdint>

namespace jshookz::provider::types {

// Borrowed canonical field-value bytes. `data` remains valid only while the
// input nominal value remains alive and no JavaScript executes. UInt payloads
// point into the caller-owned eight-byte scratch supplied to the reader.
struct NominalPayloadView {
  std::uint8_t const *data = nullptr;
  std::uint32_t size = 0;
};

// Read the canonical value payload from an exact provider-minted rich value.
// The expected materializer is part of the nominal check: even wire-compatible
// or structurally similar rich values are rejected. The path allocates
// nothing, executes no JavaScript, preserves any pending exception, and
// initializes scratch/output on every result.
[[nodiscard]] bool readNominalPayload(JSContext *ctx, JSValueConst input,
                                      catl::xdata::MaterializerKind expected,
                                      std::uint8_t (&integerScratch)[8],
                                      NominalPayloadView &output) noexcept;

namespace detail {

[[nodiscard]] bool readUIntNominalPayload(JSValueConst input,
                                          std::uint8_t expectedBits,
                                          std::uint64_t &value) noexcept;
[[nodiscard]] bool
readSTBlobNominalPayload(JSValueConst input,
                         NominalPayloadView &output) noexcept;
[[nodiscard]] bool
readHash256NominalPayload(JSValueConst input,
                          NominalPayloadView &output) noexcept;
[[nodiscard]] bool
readAccountIDNominalPayload(JSValueConst input,
                            NominalPayloadView &output) noexcept;
[[nodiscard]] bool
readAmountNominalPayload(JSValueConst input,
                         NominalPayloadView &output) noexcept;
[[nodiscard]] bool
readRichLeafNominalPayload(JSContext *ctx, JSValueConst input,
                           catl::xdata::MaterializerKind expected,
                           NominalPayloadView &output) noexcept;
[[nodiscard]] bool
readPathSetNominalPayload(JSContext *ctx, JSValueConst input,
                          NominalPayloadView &output) noexcept;

} // namespace detail
} // namespace jshookz::provider::types
