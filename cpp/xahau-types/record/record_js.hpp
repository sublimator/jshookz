#pragma once

#include <quickjs.h>

#include <cstdint>

namespace jshookz::provider::types {

[[nodiscard]] bool
registerRecordSchemas(JSContext* context, JSValueConst global);

// Exact nominal schema seam for host-backed fixed-capacity reads. Reading the
// byte length performs no JavaScript operation. Parsing returns the same flat
// ParseResult as BinarySchema.safeParse without first wrapping host bytes.
[[nodiscard]] bool readBinarySchemaByteLength(
    JSContext* context,
    JSValueConst schema,
    std::uint32_t* byteLength) noexcept;
[[nodiscard]] JSValue safeParseBinarySchemaBytes(
    JSContext* context,
    JSValueConst schema,
    std::uint8_t const* bytes,
    std::uint32_t length);

}  // namespace jshookz::provider::types
