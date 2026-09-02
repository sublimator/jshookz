"""Build and seal the Xahau QuickJS provider."""

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
    native_abi_path: Path = paths.XAHAU_HOOK_PROVIDER_NATIVE_ABI,
    source_path: Path = paths.XAHAU_RUNTIME_PROFILE_SOURCE,
    product: str = BASELINE_PRODUCT,
) -> Path:
    """Measure the sealed wasm and emit the runtime-profile lock and native ABI."""
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
    provider_imports = data["provider"]["imports"]
    native_abi = _validate_native_abi(provider_imports, product)
    native_abi_path.parent.mkdir(parents=True, exist_ok=True)
    native_abi_path.write_text(
        json.dumps(native_abi, sort_keys=True, separators=(",", ":")) + "\n"
    )
    print(f"✓ Sealed {manifest_path}")
    return manifest_path


def export_consumer_bundle(
    product: str = BASELINE_PRODUCT, destination: Path | None = None
) -> Path:
    """Write the directory xahaud consumes; see consumer_bundle.export."""
    from .consumer_bundle import export

    return export(product, destination)


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
