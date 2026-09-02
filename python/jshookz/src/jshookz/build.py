"""Build and seal the Xahau QuickJS provider."""

import hashlib
import json
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

from . import paths


BASELINE_PRODUCT = "provider"
CONSENSUS_ENTROPY_PRODUCT = "provider-consensus-entropy"


@dataclass(frozen=True)
class ProviderProduct:
    name: str
    build_dir: Path
    wasm: Path
    unwizered_wasm: Path
    manifest: Path
    cmake_manifest: Path
    native_abi: Path
    profile_source: Path
    profile_lock: Path
    bundle_dir: Path
    cmake_options: tuple[str, ...] = ()


PRODUCTS = {
    BASELINE_PRODUCT: ProviderProduct(
        name=BASELINE_PRODUCT,
        build_dir=paths.XAHAU_PROVIDER_BUILD_DIR,
        bundle_dir=paths.XAHAU_PROVIDER_CONSUMER_BUNDLE_DIR,
        wasm=paths.XAHAU_HOOK_PROVIDER_WASM,
        unwizered_wasm=paths.XAHAU_HOOK_PROVIDER_UNWIZERED_WASM,
        manifest=paths.XAHAU_HOOK_PROVIDER_MANIFEST,
        cmake_manifest=paths.XAHAU_HOOK_PROVIDER_CMAKE_MANIFEST,
        native_abi=paths.XAHAU_HOOK_PROVIDER_NATIVE_ABI,
        profile_source=paths.XAHAU_RUNTIME_PROFILE_SOURCE,
        profile_lock=paths.XAHAU_RUNTIME_PROFILE_LOCK,
    ),
    CONSENSUS_ENTROPY_PRODUCT: ProviderProduct(
        name=CONSENSUS_ENTROPY_PRODUCT,
        build_dir=paths.XAHAU_CONSENSUS_ENTROPY_PROVIDER_BUILD_DIR,
        bundle_dir=paths.XAHAU_CONSENSUS_ENTROPY_PROVIDER_CONSUMER_BUNDLE_DIR,
        wasm=paths.XAHAU_CONSENSUS_ENTROPY_HOOK_PROVIDER_WASM,
        unwizered_wasm=(paths.XAHAU_CONSENSUS_ENTROPY_HOOK_PROVIDER_UNWIZERED_WASM),
        manifest=paths.XAHAU_CONSENSUS_ENTROPY_HOOK_PROVIDER_MANIFEST,
        cmake_manifest=(paths.XAHAU_CONSENSUS_ENTROPY_HOOK_PROVIDER_CMAKE_MANIFEST),
        native_abi=paths.XAHAU_CONSENSUS_ENTROPY_HOOK_PROVIDER_NATIVE_ABI,
        profile_source=paths.XAHAU_CONSENSUS_ENTROPY_RUNTIME_PROFILE_SOURCE,
        profile_lock=paths.XAHAU_CONSENSUS_ENTROPY_RUNTIME_PROFILE_LOCK,
        cmake_options=("-DJSHOOKZ_CONSENSUS_ENTROPY_PROVIDER=ON",),
    ),
}


def _validate_native_abi(
    provider_imports: list[dict], product: str = BASELINE_PRODUCT
) -> dict:
    raw_abi = json.loads(paths.XAHAU_RAW_HOOK_ABI.read_text())
    try:
        selected = raw_abi["products"][product]
    except (KeyError, TypeError) as error:
        raise ValueError(f"raw Hook ABI has no product {product!r}") from error
    by_name = {item["name"]: item for item in selected}
    provider_names = {item["name"] for item in provider_imports}
    if (
        len(by_name) != len(selected)
        or len(provider_names) != len(provider_imports)
        or len(provider_imports) != len(selected)
        or provider_names != set(by_name)
    ):
        raise ValueError(
            "sealed provider imports differ from the pinned raw Hook ABI snapshot"
        )
    return raw_abi


def wizer_executable() -> Path:
    """Resolve the Wizer binary. Env `WIZER` wins, then PATH, then cargo bin."""
    configured = os.environ.get("WIZER")
    if configured:
        path = Path(configured).expanduser()
        if not path.is_file():
            raise RuntimeError(f"WIZER={configured} is not a file")
        return path
    found = shutil.which("wizer")
    if found:
        return Path(found)
    cargo = Path.home() / ".cargo" / "bin" / "wizer"
    if cargo.is_file():
        return cargo
    raise RuntimeError(
        "wizer not found. Install bytecodealliance/wizer 10 and put it on PATH "
        "or set WIZER=/path/to/wizer"
    )


def wizer_provider(src: Path, dest: Path) -> Path:
    """Cold wasm → sealed wasm. `src` is left alone."""
    if src.resolve() == dest.resolve():
        raise RuntimeError("wizer src and dest must differ; do not snapshot in place")
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_name(dest.name + ".wizer-tmp")
    run(
        [
            wizer_executable(),
            "--keep-init-func",
            "true",
            "--rename-func",
            "_initialize=wizer.initialize",
            "-o",
            tmp,
            src,
        ]
    )
    if not tmp.is_file():
        raise RuntimeError(f"wizer produced no output at {tmp}")
    if tmp.stat().st_size <= src.stat().st_size:
        tmp.unlink(missing_ok=True)
        raise RuntimeError(
            f"wizer output is not larger than the input "
            f"({tmp.stat().st_size} <= {src.stat().st_size}); snapshot looks vacuous"
        )
    tmp.replace(dest)
    return dest


def run(cmd: list[str | Path], **kwargs) -> subprocess.CompletedProcess:
    """Run a command, printing it first."""
    print(f"$ {' '.join(str(c) for c in cmd)}")
    return subprocess.run([str(c) for c in cmd], check=True, **kwargs)


def seal_xahau_hook_provider_bundle(
    wasm_path: Path = paths.XAHAU_HOOK_PROVIDER_WASM,
    lock_path: Path = paths.XAHAU_RUNTIME_PROFILE_LOCK,
    manifest_path: Path = paths.XAHAU_HOOK_PROVIDER_MANIFEST,
    cmake_manifest_path: Path = paths.XAHAU_HOOK_PROVIDER_CMAKE_MANIFEST,
    native_abi_path: Path = paths.XAHAU_HOOK_PROVIDER_NATIVE_ABI,
    source_path: Path = paths.XAHAU_RUNTIME_PROFILE_SOURCE,
    product: str = BASELINE_PRODUCT,
) -> Path:
    """Measure the sealed wasm and emit the lock/manifest xahaud pins."""
    from .runtime_profile import build_runtime_profile_lock

    data = build_runtime_profile_lock(source_path, wasm_path)
    if data["source"].get("product") != product:
        raise ValueError(
            f"runtime-profile product {data['source'].get('product')!r} "
            f"does not match requested product {product!r}"
        )
    lock_text = json.dumps(data, indent=2, sort_keys=True) + "\n"
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    lock_path.write_text(lock_text)
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    if lock_path.resolve() != manifest_path.resolve():
        manifest_path.write_text(lock_text)
    source = data["source"]
    provider = data["provider"]
    provider_imports = provider["imports"]
    native_abi = _validate_native_abi(provider_imports, product)
    native_abi_path.parent.mkdir(parents=True, exist_ok=True)
    native_abi_path.write_text(
        json.dumps(native_abi, sort_keys=True, separators=(",", ":")) + "\n"
    )
    manifest_sha256 = hashlib.sha256(manifest_path.read_bytes()).hexdigest()
    native_abi_sha256 = hashlib.sha256(native_abi_path.read_bytes()).hexdigest()
    cmake_manifest_path.write_text(
        "# Generated by jshookz build provider; do not edit.\n"
        f'set(XAHAU_QUICKJS_PRODUCT "{product}")\n'
        f'set(XAHAU_QUICKJS_MANIFEST_SCHEMA "{data["schema"]}")\n'
        f'set(XAHAU_QUICKJS_MANIFEST_SHA256 "{manifest_sha256}")\n'
        f'set(XAHAU_QUICKJS_PROVIDER_FILE "{wasm_path.name}")\n'
        f'set(XAHAU_QUICKJS_PROVIDER_SHA256 "{provider["sha256"]}")\n'
        f'set(XAHAU_QUICKJS_PROVIDER_SIZE "{provider["size"]}")\n'
        f'set(XAHAU_QUICKJS_PROVIDER_IMPORT_COUNT "{len(provider_imports)}")\n'
        f'set(XAHAU_QUICKJS_PROVIDER_EXPORT_COUNT "{len(provider["exports"])}")\n'
        f'set(XAHAU_QUICKJS_NATIVE_ABI_FILE "{native_abi_path.name}")\n'
        f'set(XAHAU_QUICKJS_NATIVE_ABI_SHA256 "{native_abi_sha256}")\n'
        f'set(XAHAU_QUICKJS_BYTECODE_ABI_ID "{data["bytecode_abi_id"]}")\n'
        f'set(XAHAU_QUICKJS_RUNTIME_PROFILE_ID "{data["runtime_profile_id"]}")\n'
        f'set(XAHAU_QUICKJS_HOOK_API_VERSION "{source["artifact"]["hook_api_version"]}")\n'
        f'set(XAHAU_QUICKJS_WASMTIME_VERSION "{source["engine"]["version"]}")\n'
        f'set(XAHAU_QUICKJS_INITIALIZATION_FUEL "{source["limits"]["wasmtime_fuel_per_initialization"]}")\n'
        f'set(XAHAU_QUICKJS_INVOCATION_FUEL "{source["limits"]["wasmtime_fuel_per_invocation"]}")\n'
        f'set(XAHAU_QUICKJS_HOST_WORK_METER "{source["limits"]["host_work_meter"]}")\n'
        f'set(XAHAU_QUICKJS_HOST_WORK_BUDGET "{source["limits"]["host_work_budget"]}")\n'
        f'set(XAHAU_QUICKJS_HOST_WORK_BASE_PER_CALL "{source["limits"]["host_work_base_per_call"]}")\n'
        f'set(XAHAU_QUICKJS_HOST_WORK_PER_ADDRESSED_BYTE "{source["limits"]["host_work_per_addressed_byte"]}")\n'
        f'set(XAHAU_QUICKJS_HOST_ADAPTER_POLICY "{source["execution"]["host_adapter_policy"]}")\n'
        f'set(XAHAU_QUICKJS_HEAP_BYTES "{source["limits"]["quickjs_heap_bytes"]}")\n'
        f'set(XAHAU_QUICKJS_STACK_BYTES "{source["limits"]["quickjs_stack_bytes"]}")\n'
        f'set(XAHAU_QUICKJS_SERIALIZED_OBJECT_MAX_BYTES "{source["limits"]["serialized_object_max_bytes"]}")\n'
        f'set(XAHAU_QUICKJS_SERIALIZED_OBJECT_MAX_FIELDS "{source["limits"]["serialized_object_max_fields"]}")\n'
        f'set(XAHAU_QUICKJS_SERIALIZED_OBJECT_MAX_SCOPES "{source["limits"]["serialized_object_max_scopes"]}")\n'
        f'set(XAHAU_QUICKJS_SERIALIZED_OBJECT_MAX_DEPTH "{source["limits"]["serialized_object_max_depth"]}")\n'
    )
    print(f"✓ Sealed {manifest_path}")
    print(f"✓ Projected {cmake_manifest_path}")
    return manifest_path


# The consumer bundle is the exact file set xahaud's cmake/QuickJSProvider.cmake
# requires at configure, plus the consumer lock its generator parses. Keep the
# basenames and the lock shape identical to that contract; xahaud verifies
# every digest before projecting a single constant.
CONSUMER_LOCK_SCHEMA = "xahau.quickjs.provider-consumer-lock.v1"
CONSUMER_LOCK_FILE = "jshookz_provider.lock.json"
SEALED_BUNDLE_FILES = (
    "jshookz_provider.wasm",
    "jshookz_provider.manifest.json",
    "jshookz_provider.native-abi.json",
)
CONSUMER_BUNDLE_API_ARTIFACTS = {
    "api-artifacts.json": paths.API_ARTIFACT_MANIFEST,
    "hooks-api.d.ts": paths.CANONICAL_HOOKS_API_DECLARATIONS,
    "xahau-quickjs-v1-consensus-entropy.d.ts": (
        paths.XAHAU_V1_CONSENSUS_ENTROPY_HOOKS_API_DECLARATIONS
    ),
    "xahau-quickjs-v1-consensus-entropy.surface.json": (
        paths.XAHAU_V1_CONSENSUS_ENTROPY_JAVASCRIPT_SURFACE
    ),
    "xahau-quickjs-v1.d.ts": paths.XAHAU_V1_HOOKS_API_DECLARATIONS,
    "xahau-quickjs-v1.surface.json": paths.XAHAU_V1_JAVASCRIPT_SURFACE,
    "xfl-profile-ledger.ts": paths.XAHAU_XFL_PROFILE_LEDGER,
}


def _sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def consumer_lock(manifest: dict, bundle: Path) -> dict:
    """Derive the consumer lock from a sealed manifest and the bundle bytes."""
    return {
        "api_artifacts": {
            "file": "api-artifacts.json",
            "sha256": _sha256_file(bundle / "api-artifacts.json"),
        },
        "bytecode_abi_id": manifest["bytecode_abi_id"],
        "manifest": {
            "file": "jshookz_provider.manifest.json",
            "schema": manifest["schema"],
            "sha256": _sha256_file(bundle / "jshookz_provider.manifest.json"),
        },
        "native_abi": {
            "file": "jshookz_provider.native-abi.json",
            "sha256": _sha256_file(bundle / "jshookz_provider.native-abi.json"),
        },
        "product": manifest["source"]["product"],
        "provider": {
            "file": "jshookz_provider.wasm",
            "sha256": manifest["provider"]["sha256"],
            "size": manifest["provider"]["size"],
        },
        "runtime_profile_id": manifest["runtime_profile_id"],
        "schema": CONSUMER_LOCK_SCHEMA,
        "wasmtime_version": manifest["source"]["engine"]["version"],
    }


def export_consumer_bundle(
    product: str = BASELINE_PRODUCT, destination: Path | None = None
) -> Path:
    """Copy one sealed product plus its API artifacts into a consumer bundle.

    The result is what xahaud consumes directly, either through
    `-DXAHAU_QUICKJS_PROVIDER_BUNDLE_DIR=<destination>` for a local build or
    by copying into `external/quickjs-provider` for a pin. Nothing is
    rebuilt or resealed here; a stale or mismatched seal fails closed.
    """
    try:
        selected = PRODUCTS[product]
    except KeyError as error:
        raise ValueError(f"unknown provider product: {product!r}") from error
    target = Path(destination).resolve() if destination else selected.bundle_dir
    sources: dict[str, Path] = {
        name: selected.build_dir / name for name in SEALED_BUNDLE_FILES
    }
    sources.update(CONSUMER_BUNDLE_API_ARTIFACTS)
    missing = [str(path) for path in sources.values() if not path.is_file()]
    if missing:
        raise RuntimeError(
            f"incomplete {product} build; run `jshookz build {product}` first. "
            f"Missing: {', '.join(missing)}"
        )
    manifest = json.loads(sources["jshookz_provider.manifest.json"].read_text())
    if manifest.get("source", {}).get("product") != product:
        raise RuntimeError(
            f"sealed manifest names product "
            f"{manifest.get('source', {}).get('product')!r}, not {product!r}"
        )
    wasm = sources["jshookz_provider.wasm"].read_bytes()
    if (
        hashlib.sha256(wasm).hexdigest() != manifest["provider"]["sha256"]
        or len(wasm) != manifest["provider"]["size"]
    ):
        raise RuntimeError(
            "sealed provider does not match its manifest; rebuild before exporting"
        )
    target.mkdir(parents=True, exist_ok=True)
    for name, source in sources.items():
        shutil.copyfile(source, target / name)
    lock = consumer_lock(manifest, target)
    (target / CONSUMER_LOCK_FILE).write_text(
        json.dumps(lock, indent=2, sort_keys=True) + "\n"
    )
    print(f"✓ Exported consumer bundle {target}")
    return target


def build_xahau_hook_provider(
    *, product: str = BASELINE_PRODUCT, wizer: bool = True
) -> Path:
    """Build and seal the deterministic Xahau QuickJS provider bundle."""
    try:
        selected = PRODUCTS[product]
    except KeyError as error:
        raise ValueError(f"unknown provider product: {product!r}") from error
    paths.require_source_checkout()
    if not paths.WASI_SDK_PATH.exists():
        print(f"error: wasi-sdk not found at {paths.WASI_SDK_PATH}", file=sys.stderr)
        print("install via: mise install wasi-sdk@32", file=sys.stderr)
        sys.exit(1)

    build_dir = selected.build_dir
    build_dir.mkdir(parents=True, exist_ok=True)
    run(
        [
            "cmake",
            "-B",
            build_dir,
            "-S",
            paths.PROVIDER_SRC,
            f"-DCMAKE_TOOLCHAIN_FILE={paths.WASI_TOOLCHAIN}",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DXAHAU_HOOK_PROVIDER=ON",
            *selected.cmake_options,
        ]
    )
    run(["cmake", "--build", build_dir])

    cold = selected.unwizered_wasm
    wasm = selected.wasm
    if not cold.exists():
        raise RuntimeError(f"Xahau Hook provider not found at {cold}")
    if wizer:
        wizer_provider(cold, wasm)
        print(f"✓ Wizered {wasm} ({wasm.stat().st_size / (1024 * 1024):.1f} MB)")
        seal_xahau_hook_provider_bundle(
            wasm_path=wasm,
            lock_path=selected.profile_lock,
            manifest_path=selected.manifest,
            cmake_manifest_path=selected.cmake_manifest,
            native_abi_path=selected.native_abi,
            source_path=selected.profile_source,
            product=selected.name,
        )
        export_consumer_bundle(product=selected.name)
    else:
        shutil.copyfile(cold, wasm)
        print(f"✓ Skipped Wizer (--no-wizer); copied {cold.name}")
    print(f"\n✓ Built {wasm} ({wasm.stat().st_size / (1024 * 1024):.1f} MB)")
    return wasm
