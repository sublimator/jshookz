#pragma once

#include "quickjs.hpp"

#include <cstdint>

namespace jshookz::provider::types {

enum class RichLeafKind : std::uint8_t {
  hash128,
  hash160,
  hash192,
  currency,
  issue,
  vector256,
  xChainBridge,
};

// Register the constructorless rich leaves used by recursive STObject
// materialization.  No JavaScript factory/global is published: only the
// provider can mint these exact nominal classes.
[[nodiscard]] bool registerRichLeafTypes(JSContext *ctx);
void unregisterRichLeafTypes(JSRuntime *runtime) noexcept;

[[nodiscard]] bool isRichLeaf(JSValueConst value, RichLeafKind kind) noexcept;

// Provider-only factories.  Inputs are canonical value payloads from the
// recursive certifier: no field header and no VL prefix.
[[nodiscard]] JSValue makeHash128Bytes(JSContext *ctx,
                                       std::uint8_t const *bytes,
                                       std::uint32_t length);
[[nodiscard]] JSValue makeHash128View(JSContext *ctx, JSValueConst owner,
                                      std::uint8_t const *bytes,
                                      std::uint32_t length);
[[nodiscard]] JSValue makeHash160Bytes(JSContext *ctx,
                                       std::uint8_t const *bytes,
                                       std::uint32_t length);
[[nodiscard]] JSValue makeHash160View(JSContext *ctx, JSValueConst owner,
                                      std::uint8_t const *bytes,
                                      std::uint32_t length);
[[nodiscard]] JSValue makeHash192Bytes(JSContext *ctx,
                                       std::uint8_t const *bytes,
                                       std::uint32_t length);
[[nodiscard]] JSValue makeHash192View(JSContext *ctx, JSValueConst owner,
                                      std::uint8_t const *bytes,
                                      std::uint32_t length);
[[nodiscard]] JSValue makeHash192DerivedBytes(JSContext *ctx,
                                              JSValueConst owner,
                                              std::uint8_t const *bytes,
                                              std::uint32_t length);
[[nodiscard]] bool readHash192Bytes(JSContext *ctx, JSValueConst value,
                                    std::uint8_t *output) noexcept;
[[nodiscard]] JSValue makeCurrencyBytes(JSContext *ctx,
                                        std::uint8_t const *bytes,
                                        std::uint32_t length);
[[nodiscard]] JSValue makeCurrencyView(JSContext *ctx, JSValueConst owner,
                                       std::uint8_t const *bytes,
                                       std::uint32_t length);
[[nodiscard]] JSValue makeCurrencyDerivedBytes(JSContext *ctx,
                                               JSValueConst owner,
                                               std::uint8_t const *bytes,
                                               std::uint32_t length);
[[nodiscard]] bool readCurrencyBytes(JSContext *ctx, JSValueConst value,
                                     std::uint8_t *output) noexcept;
[[nodiscard]] JSValue makeIssueBytes(JSContext *ctx, std::uint8_t const *bytes,
                                     std::uint32_t length);
[[nodiscard]] JSValue makeIssueView(JSContext *ctx, JSValueConst owner,
                                    std::uint8_t const *bytes,
                                    std::uint32_t length);
// Composite leaves sometimes expose a canonical value whose bytes are a
// deterministic transform rather than one contiguous owner slice. Retain the
// certified owner directly while copying only that bounded derived value.
[[nodiscard]] JSValue makeIssueDerivedBytes(JSContext *ctx, JSValueConst owner,
                                            std::uint8_t const *bytes,
                                            std::uint32_t length);
[[nodiscard]] JSValue makeVector256Bytes(JSContext *ctx,
                                         std::uint8_t const *bytes,
                                         std::uint32_t length);
[[nodiscard]] JSValue makeVector256View(JSContext *ctx, JSValueConst owner,
                                        std::uint8_t const *bytes,
                                        std::uint32_t length);
[[nodiscard]] JSValue makeXChainBridgeBytes(JSContext *ctx,
                                            std::uint8_t const *bytes,
                                            std::uint32_t length);
[[nodiscard]] JSValue makeXChainBridgeView(JSContext *ctx, JSValueConst owner,
                                           std::uint8_t const *bytes,
                                           std::uint32_t length);

} // namespace jshookz::provider::types
