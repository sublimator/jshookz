#pragma once

#include <quickjs.h>

#include <cstdint>

namespace jshookz::provider::types {

enum class LedgerKeyletKind : std::uint8_t {
  accountRoot,
  uriToken,
};

// Register the private immutable keylet value class. The public runtime noun
// and util.keylet factory are installed separately after all classified value
// types exist.
[[nodiscard]] bool registerLedgerKeylet(JSContext *ctx);
[[nodiscard]] bool installLedgerKeyletNamespace(JSContext *ctx,
                                                JSValueConst util);

[[nodiscard]] bool isLedgerKeylet(JSValueConst value) noexcept;

// Allocation-free exact nominal reader used by host-backed ledger lookup.
// It invokes no JavaScript and initializes every output on success only.
[[nodiscard]] bool readLedgerKeylet(JSValueConst value,
                                    std::uint8_t bytes[34],
                                    LedgerKeyletKind *kind) noexcept;

} // namespace jshookz::provider::types
