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
API_ARTIFACT_MANIFEST = PACKAGE_ROOT / "types" / "api-artifacts.json"
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
            (candidate / "runtime/provider/CMakeLists.txt").is_file()
            and (candidate / "engine/quickjs/quickjs.c").is_file()
        ):
            return candidate
    return None


SOURCE_CHECKOUT = _discover_repo_root()
# Product data works from an installed wheel. Build/profile defaults require a
# source checkout or explicit CLI paths/environment overrides.
REPO_ROOT = SOURCE_CHECKOUT or PACKAGE_ROOT / "__source_checkout_required__"


def require_source_checkout() -> Path:
    if SOURCE_CHECKOUT is None:
        raise RuntimeError(
            "this command requires a jshookz source checkout; set "
            "JSHOOKZ_REPO_ROOT to its root"
        )
    return SOURCE_CHECKOUT

# Provider source
PROVIDER_SRC = REPO_ROOT / "runtime" / "provider"

# Build outputs
BUILD_DIR = REPO_ROOT / "build"
XAHAU_PROVIDER_BUILD_DIR = BUILD_DIR / "xahau-provider"
XAHAU_HOOK_PROVIDER_WASM = Path(
    os.environ.get(
        "JSHOOKZ_PROVIDER_WASM",
        XAHAU_PROVIDER_BUILD_DIR / "jshookz_provider.wasm",
    )
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
XAHAU_RUNTIME_PROFILE_SOURCE = (
    REPO_ROOT / "integrations/xahau/profiles" / "xahau-quickjs-v1.source.json"
)
XAHAU_RUNTIME_PROFILE_LOCK = (
    REPO_ROOT / "integrations/xahau/profiles" / "xahau-quickjs-v1.lock.json"
)
XAHAU_RAW_HOOK_ABI = (
    REPO_ROOT / "integrations/xahau/generated" / "raw-hook-abi.json"
)

# wasi-sdk
WASI_SDK_PATH = Path(
    os.environ.get("WASI_SDK_PATH", "")
    or str(Path.home() / ".local/share/mise/installs/wasi-sdk/32/wasi-sdk")
)
WASI_TOOLCHAIN = WASI_SDK_PATH / "share" / "cmake" / "wasi-sdk.cmake"
