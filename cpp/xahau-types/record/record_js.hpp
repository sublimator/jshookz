#pragma once

#include <quickjs.h>

#include <cstdint>

namespace jshookz::provider::types {

[[nodiscard]] bool
registerRecordSchemas(JSContext* context, JSValueConst global);

// Exact nominal codec seam for host-backed fixed-capacity reads. A codec is a
// provider-minted BinarySchema or one non-padding RecordField. Reading its
// byte length performs no JavaScript operation. Parsing returns the same flat
// ParseResult as BinarySchema.safeParse without first wrapping host bytes.
[[nodiscard]] bool readBinaryCodecByteLength(
    JSContext* context,
    JSValueConst codec,
    std::uint32_t* byteLength) noexcept;
[[nodiscard]] JSValue safeParseBinaryCodecBytes(
    JSContext* context,
    JSValueConst codec,
    std::uint8_t const* bytes,
    std::uint32_t length);

}  // namespace jshookz::provider::types
