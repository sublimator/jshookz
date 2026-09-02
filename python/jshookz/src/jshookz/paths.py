"""Canonical paths for the project — single source of truth."""

import os
from pathlib import Path

PACKAGE_ROOT = Path(__file__).resolve().parent
CANONICAL_HOOKS_API_DECLARATIONS = PACKAGE_ROOT / "types" / "hooks-api.d.ts"
XAHAU_V1_HOOKS_API_DECLARATIONS = (
    PACKAGE_ROOT / "types" / "xahau-quickjs-v1.d.ts"
)
XAHAU_V1_JAVASCRIPT_SURFACE = (
    PACKAGE_ROOT / "types" / "xahau-quickjs-v1.surface.json"
)
XAHAU_V1_CONSENSUS_ENTROPY_HOOKS_API_DECLARATIONS = (
    PACKAGE_ROOT / "types" / "xahau-quickjs-v1-consensus-entropy.d.ts"
)
XAHAU_V1_CONSENSUS_ENTROPY_JAVASCRIPT_SURFACE = (
    PACKAGE_ROOT / "types" / "xahau-quickjs-v1-consensus-entropy.surface.json"
)
API_ARTIFACT_MANIFEST = PACKAGE_ROOT / "types" / "api-artifacts.json"
XAHAU_XFL_PROFILE_LEDGER = PACKAGE_ROOT / "xfl_profile_ledger.ts"
# Backwards-compatible name for consumers asking for the broad public spec.
HOOKS_API_DECLARATIONS = CANONICAL_HOOKS_API_DECLARATIONS


def _discover_repo_root() -> Path | None:
    configured = os.environ.get("JSHOOKZ_REPO_ROOT")
    candidates = (
        [Path(configured).expanduser().resolve()]
        if configured
        else list(PACKAGE_ROOT.parents)
    )
    for candidate in candidates:
        if (
            (candidate / "cpp/provider/CMakeLists.txt").is_file()
            and (candidate / "cpp/quickjs/quickjs.c").is_file()
        ):
            return candidate
    return None


SOURCE_CHECKOUT = _discover_repo_root()
# Build/profile defaults require a source checkout or explicit CLI
# paths/environment overrides.
REPO_ROOT = SOURCE_CHECKOUT or PACKAGE_ROOT / "__source_checkout_required__"


def require_source_checkout() -> Path:
    if SOURCE_CHECKOUT is None:
        raise RuntimeError(
            "this command requires a jshookz source checkout; set "
            "JSHOOKZ_REPO_ROOT to its root"
        )
    return SOURCE_CHECKOUT

# Provider source
PROVIDER_SRC = REPO_ROOT / "cpp" / "provider"

# Build outputs
BUILD_DIR = REPO_ROOT / "build"
XAHAU_PROVIDER_BUILD_DIR = BUILD_DIR / "xahau-provider"
XAHAU_HOOK_PROVIDER_WASM = Path(
    os.environ.get(
        "JSHOOKZ_PROVIDER_WASM",
        XAHAU_PROVIDER_BUILD_DIR / "jshookz_provider.wasm",
    )
)
# CMake output. Not locked. Wizer reads this every build so a no-op cmake
# cannot feed an already-snapshotted wasm back into wizer.
XAHAU_HOOK_PROVIDER_UNWIZERED_WASM = (
    XAHAU_PROVIDER_BUILD_DIR / "jshookz_provider.unwizered.wasm"
)
XAHAU_HOOK_PROVIDER_MANIFEST = (
    XAHAU_PROVIDER_BUILD_DIR / "jshookz_provider.manifest.json"
)
XAHAU_HOOK_PROVIDER_CMAKE_MANIFEST = (
    XAHAU_PROVIDER_BUILD_DIR / "jshookz_provider.manifest.cmake"
)
XAHAU_HOOK_PROVIDER_NATIVE_ABI = (
    XAHAU_PROVIDER_BUILD_DIR / "jshookz_provider.native-abi.json"
)
# Complete consumer bundles: the exact file set xahaud's cmake requires plus
# the consumer lock, exported by `jshookz build <product>`. Point xahaud's
# XAHAU_QUICKJS_PROVIDER_BUNDLE_DIR here; never share roots between products.
XAHAU_PROVIDER_CONSUMER_BUNDLE_DIR = BUILD_DIR / "xahau-provider-bundle"
XAHAU_CONSENSUS_ENTROPY_PROVIDER_BUILD_DIR = (
    BUILD_DIR / "xahau-provider-consensus-entropy"
)
XAHAU_CONSENSUS_ENTROPY_PROVIDER_CONSUMER_BUNDLE_DIR = (
    BUILD_DIR / "xahau-provider-consensus-entropy-bundle"
)
XAHAU_CONSENSUS_ENTROPY_HOOK_PROVIDER_WASM = Path(
    os.environ.get(
        "JSHOOKZ_CONSENSUS_ENTROPY_PROVIDER_WASM",
        XAHAU_CONSENSUS_ENTROPY_PROVIDER_BUILD_DIR / "jshookz_provider.wasm",
    )
)
XAHAU_CONSENSUS_ENTROPY_HOOK_PROVIDER_UNWIZERED_WASM = (
    XAHAU_CONSENSUS_ENTROPY_PROVIDER_BUILD_DIR
    / "jshookz_provider.unwizered.wasm"
)
XAHAU_CONSENSUS_ENTROPY_HOOK_PROVIDER_MANIFEST = (
    XAHAU_CONSENSUS_ENTROPY_PROVIDER_BUILD_DIR
    / "jshookz_provider.manifest.json"
)
XAHAU_CONSENSUS_ENTROPY_HOOK_PROVIDER_CMAKE_MANIFEST = (
    XAHAU_CONSENSUS_ENTROPY_PROVIDER_BUILD_DIR
    / "jshookz_provider.manifest.cmake"
)
XAHAU_CONSENSUS_ENTROPY_HOOK_PROVIDER_NATIVE_ABI = (
    XAHAU_CONSENSUS_ENTROPY_PROVIDER_BUILD_DIR
    / "jshookz_provider.native-abi.json"
)
XAHAU_RUNTIME_PROFILE_SOURCE = (
    REPO_ROOT / "xahau/profiles" / "xahau-quickjs-v1.source.json"
)
# Emitted by `jshookz build provider`. Not committed; xahaud pins a copy.
XAHAU_RUNTIME_PROFILE_LOCK = (
    REPO_ROOT / "xahau/profiles" / "xahau-quickjs-v1.lock.json"
)
XAHAU_CONSENSUS_ENTROPY_RUNTIME_PROFILE_SOURCE = (
    REPO_ROOT
    / "xahau/profiles"
    / "xahau-quickjs-v1-consensus-entropy.delta.json"
)
XAHAU_CONSENSUS_ENTROPY_RUNTIME_PROFILE_LOCK = (
    REPO_ROOT
    / "xahau/profiles"
    / "xahau-quickjs-v1-consensus-entropy.lock.json"
)
XAHAU_RAW_HOOK_ABI = (
    REPO_ROOT / "xahau/generated" / "raw-hook-abi.json"
)

# wasi-sdk
WASI_SDK_PATH = Path(
    os.environ.get("WASI_SDK_PATH", "")
    or str(Path.home() / ".local/share/mise/installs/wasi-sdk/32/wasi-sdk")
)
WASI_TOOLCHAIN = WASI_SDK_PATH / "share" / "cmake" / "wasi-sdk.cmake"
