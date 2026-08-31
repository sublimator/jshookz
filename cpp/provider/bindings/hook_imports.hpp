#pragma once

#include <cstdint>

extern "C" {
#include "../generated/hook_raw_imports.inc"
#ifdef CONFIG_XAHAU_CONSENSUS_ENTROPY_PROVIDER
#include "../generated/hook_raw_imports_consensus_entropy.inc"
#endif
}
