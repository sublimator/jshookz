#pragma once

#include <quickjs.h>

#include <cstdint>

namespace jshookz::provider::types {

// Publish the generated 325-member Field table. Descriptor objects have a
// private class and no public constructor/class value.
[[nodiscard]] bool registerFieldDescriptors(JSContext *ctx,
                                            JSValueConst global);

// Allocation-free exact-provenance classifier for method/replacement input.
[[nodiscard]] bool serializedFieldCode(JSValueConst value,
                                       std::uint32_t &code) noexcept;

} // namespace jshookz::provider::types
