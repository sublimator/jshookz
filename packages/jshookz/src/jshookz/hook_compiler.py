"""TypeScript/JavaScript Hook compiler tied to the QuickJS provider build."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path

from . import paths
from .hook_artifact import build_hook_artifact
from .host import WasmHost


CANONICAL_DECLARATIONS = paths.CANONICAL_HOOKS_API_DECLARATIONS
DEFAULT_DECLARATIONS = paths.XAHAU_V1_HOOKS_API_DECLARATIONS

_FRONTEND_DIR = Path(__file__).resolve().parent
_FRONTEND_TSCONFIG = _FRONTEND_DIR / "tsconfig.frontend.json"
_FRONTEND_SOURCES = (
    _FRONTEND_DIR / "compiler_driver.ts",
    _FRONTEND_DIR / "entry_policy.ts",
    _FRONTEND_DIR / "result_validator.ts",
    _FRONTEND_DIR / "node-shim.d.ts",
    _FRONTEND_TSCONFIG,
)


@dataclass(frozen=True)
class CompiledHook:
    bytecode: bytes
    javascript: str


@dataclass(frozen=True)
class PackagedHook:
    artifact: bytes
    bytecode: bytes
    javascript: str


def _typescript_executable(tsc: str | None) -> str:
    executable = tsc or os.environ.get("TSC") or shutil.which("tsc")
    if not executable:
        raise RuntimeError("TypeScript compiler not found; install or set TSC")
    return executable


def _typescript_library(tsc: str | None) -> Path:
    executable = Path(_typescript_executable(tsc)).resolve()
    typescript = executable.parent.parent / "lib" / "typescript.js"
    if not typescript.is_file():
        raise RuntimeError(
            "Cannot locate the TypeScript parser beside tsc: " f"{executable}"
        )
    return typescript


def _frontend_driver_js(tsc: str | None) -> Path:
    """Emit the TypeScript frontend to a mtime-keyed cache, then run that JS."""
    missing = [path for path in _FRONTEND_SOURCES if not path.is_file()]
    if missing:
        raise RuntimeError(f"compiler frontend source missing: {missing[0]}")
    stamp = ":".join(
        f"{path.name}={path.stat().st_mtime_ns}" for path in _FRONTEND_SOURCES
    )
    cache = Path(tempfile.gettempdir()) / "jshookz-frontend"
    cache.mkdir(parents=True, exist_ok=True)
    marker = cache / "stamp"
    driver = cache / "compiler_driver.js"
    if driver.is_file() and marker.is_file() and marker.read_text() == stamp:
        return driver
    compiled = subprocess.run(
        [
            _typescript_executable(tsc),
            "-p",
            str(_FRONTEND_TSCONFIG),
            "--noEmit",
            "false",
            "--outDir",
            str(cache),
            "--declaration",
            "false",
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    if compiled.returncode != 0 or not driver.is_file():
        detail = "\n".join(
            part.strip()
            for part in (compiled.stdout, compiled.stderr)
            if part.strip()
        ) or "frontend tsc failed"
        raise RuntimeError(f"compiler frontend failed to emit:\n{detail}")
    marker.write_text(stamp)
    return driver


def _parse_driver_meta(stderr: str) -> tuple[str, bool]:
    kind = "typescript"
    allow_malformed = False
    for line in stderr.splitlines():
        if line.startswith("kind="):
            kind = line.split("=", 1)[1].strip()
        elif line.startswith("allowMalformed="):
            allow_malformed = line.split("=", 1)[1].strip() == "1"
    return kind, allow_malformed


def _run_compiler_driver(
    source: Path,
    config: Path,
    *,
    tsc: str | None,
    failure_label: str,
) -> bool:
    """One TypeScript Program: diagnostics, Result policy, entry types, emit."""
    node = shutil.which("node")
    if not node:
        raise RuntimeError("Node.js not found; it is required beside tsc")

    completed = subprocess.run(
        [
            node,
            str(_frontend_driver_js(tsc)),
            str(_typescript_library(tsc)),
            str(config),
            str(source),
        ],
        capture_output=True,
        check=False,
        text=True,
    )
    kind, allow_malformed = _parse_driver_meta(completed.stderr)
    if completed.returncode == 0:
        return allow_malformed
    body_lines = [
        line
        for line in completed.stderr.splitlines()
        if not line.startswith(("kind=", "allowMalformed=", "createProgram="))
    ]
    detail = "\n".join(
        part
        for part in (*body_lines, completed.stdout.strip())
        if part.strip()
    ) or "compiler driver failed"
    if kind == "entry":
        raise RuntimeError(f"Hook entry signature is invalid:\n{detail}")
    if kind == "result":
        raise RuntimeError(
            "Hook Result must use one of its six legal exits:\n" f"{detail}"
        )
    raise RuntimeError(f"{failure_label}:\n{detail}")


def _typescript_to_javascript(
    source: Path,
    *,
    declarations: Path,
    tsc: str | None = None,
) -> tuple[str, bool]:
    if not declarations.is_file():
        raise FileNotFoundError(f"Hook API declarations not found: {declarations}")

    source = source.resolve()
    declarations = declarations.resolve()
    with tempfile.TemporaryDirectory(prefix="qjs-hook-ts-") as temp:
        temp_path = Path(temp)
        out_dir = temp_path / "out"
        config = temp_path / "tsconfig.json"
        config.write_text(
            json.dumps(
                {
                    "compilerOptions": {
                        "lib": ["ES2023"],
                        "module": "ESNext",
                        "outDir": str(out_dir),
                        "rootDir": str(source.parent),
                        "skipLibCheck": False,
                        "strict": True,
                        "target": "ES2023",
                    },
                    "files": [str(declarations), str(source)],
                },
                indent=2,
            )
        )
        source_allows = _run_compiler_driver(
            source,
            config,
            tsc=tsc,
            failure_label="TypeScript compilation failed",
        )

        emitted = out_dir / source.with_suffix(".js").name
        if not emitted.is_file():
            raise RuntimeError(f"TypeScript emitted no JavaScript at {emitted}")
        return emitted.read_text(), source_allows


def _check_javascript(
    source: Path,
    *,
    declarations: Path,
    tsc: str | None = None,
) -> tuple[str, bool]:
    """Type-check raw JavaScript and apply the same Result ownership law."""
    declarations = declarations.resolve()
    if not declarations.is_file():
        raise FileNotFoundError(f"Hook API declarations not found: {declarations}")

    with tempfile.TemporaryDirectory(prefix="qjs-hook-js-") as temp:
        config = Path(temp) / "tsconfig.json"
        config.write_text(
            json.dumps(
                {
                    "compilerOptions": {
                        "allowJs": True,
                        "checkJs": True,
                        "lib": ["ES2023"],
                        "module": "ESNext",
                        "noEmit": True,
                        "skipLibCheck": False,
                        "strict": True,
                        "target": "ES2023",
                    },
                    "files": [str(declarations), str(source)],
                },
                indent=2,
            )
        )
        source_allows = _run_compiler_driver(
            source,
            config,
            tsc=tsc,
            failure_label="JavaScript checking failed",
        )
    return source.read_text(), source_allows


def compile_hook(
    source: str | Path,
    *,
    wasm_path: str | Path | None = None,
    declarations: str | Path = DEFAULT_DECLARATIONS,
    tsc: str | None = None,
    allow_malformed: bool = False,
) -> CompiledHook:
    """Compile a .ts/.js Hook to provider-compatible QuickJS bytecode."""
    source_path = Path(source).resolve()
    suffix = source_path.suffix.lower()
    source_allows = False
    if suffix == ".ts":
        javascript, source_allows = _typescript_to_javascript(
            source_path,
            declarations=Path(declarations),
            tsc=tsc,
        )
    elif suffix in {".js", ".mjs"}:
        javascript, source_allows = _check_javascript(
            source_path,
            declarations=Path(declarations),
            tsc=tsc,
        )
    else:
        raise ValueError(
            f"unsupported Hook source extension {suffix!r}; expected .ts or .js"
        )

    provider = Path(wasm_path or paths.XAHAU_HOOK_PROVIDER_WASM).resolve()
    if not provider.is_file():
        guidance = (
            "Build it with `jshookz build provider`."
            if paths.SOURCE_CHECKOUT is not None
            else "Set JSHOOKZ_PROVIDER_WASM or pass --wasm."
        )
        raise FileNotFoundError(
            f"Xahau QuickJS provider not found: {provider}\n"
            f"{guidance}"
        )

    emit_malformed = allow_malformed or source_allows
    host = WasmHost(wasm_path=provider)
    host.init()
    try:
        bytecode = host.compile_source(javascript, module=True)
        validation = (
            None
            if emit_malformed
            else host.validate_hook_bytecode(bytecode)
        )
    finally:
        host.destroy()
    if validation is not None and not validation.valid:
        raise RuntimeError(
            "QuickJS Hook is not deployable: "
            f"{validation.error or 'provider validation failed'}"
        )
    return CompiledHook(bytecode=bytecode, javascript=javascript)


def package_hook(
    source: str | Path,
    *,
    hook_api_version: int,
    bytecode_abi_id: bytes,
    runtime_profile_id: bytes,
    profile_path: str | Path | None = None,
    wasm_path: str | Path | None = None,
    declarations: str | Path = DEFAULT_DECLARATIONS,
    tsc: str | None = None,
) -> PackagedHook:
    """Compile source and bind its bytecode to an explicit deployment profile.

    The identities are mandatory because silently packaging against whichever
    provider happens to be on disk would produce replay-ambiguous ledger data.
    A profile registry can supply them later; this boundary does not invent a
    default profile.
    """
    compiled = compile_hook(
        source,
        wasm_path=wasm_path,
        declarations=declarations,
        tsc=tsc,
    )
    provider = Path(wasm_path or paths.XAHAU_HOOK_PROVIDER_WASM).resolve()
    validator = (
        WasmHost.profiled(wasm_path=provider, profile_path=profile_path)
        if profile_path is not None
        else WasmHost(wasm_path=provider)
    )
    validator.init()
    try:
        validation = validator.validate_hook_bytecode(compiled.bytecode)
    finally:
        validator.destroy()
    if not validation.valid:
        raise RuntimeError(
            "QuickJS Hook is not deployable: "
            f"{validation.error or 'provider validation failed'}"
        )
    return PackagedHook(
        artifact=build_hook_artifact(
            compiled.bytecode,
            hook_api_version=hook_api_version,
            bytecode_abi_id=bytecode_abi_id,
            runtime_profile_id=runtime_profile_id,
        ),
        bytecode=compiled.bytecode,
        javascript=compiled.javascript,
    )
