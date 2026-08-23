#pragma once

#include "quickjs.hpp"

#include <cstdint>

namespace jshookz::provider::types {

// Register the constructorless rich leaves used by recursive STObject
// materialization.  No JavaScript factory/global is published: only the
// provider can mint these exact nominal classes.
[[nodiscard]] bool registerRichLeafTypes(JSContext *ctx);
void unregisterRichLeafTypes(JSRuntime *runtime) noexcept;

// Provider-only factories.  Inputs are canonical value payloads from the
// recursive certifier: no field header and no VL prefix.
[[nodiscard]] JSValue makeHash128Bytes(JSContext *ctx,
                                       std::uint8_t const *bytes,
                                       std::uint32_t length);
[[nodiscard]] JSValue makeHash160Bytes(JSContext *ctx,
                                       std::uint8_t const *bytes,
                                       std::uint32_t length);
[[nodiscard]] JSValue makeHash192Bytes(JSContext *ctx,
                                       std::uint8_t const *bytes,
                                       std::uint32_t length);
[[nodiscard]] bool readHash192Bytes(JSContext *ctx, JSValueConst value,
                                    std::uint8_t *output) noexcept;
[[nodiscard]] JSValue makeCurrencyBytes(JSContext *ctx,
                                        std::uint8_t const *bytes,
                                        std::uint32_t length);
[[nodiscard]] bool readCurrencyBytes(JSContext *ctx, JSValueConst value,
                                     std::uint8_t *output) noexcept;
[[nodiscard]] JSValue makeIssueBytes(JSContext *ctx, std::uint8_t const *bytes,
                                     std::uint32_t length);
[[nodiscard]] JSValue makeVector256Bytes(JSContext *ctx,
                                         std::uint8_t const *bytes,
                                         std::uint32_t length);
[[nodiscard]] JSValue makeXChainBridgeBytes(JSContext *ctx,
                                            std::uint8_t const *bytes,
                                            std::uint32_t length);

} // namespace jshookz::provider::types
