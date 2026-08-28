"""TypeScript/JavaScript Hook compiler tied to the QuickJS provider build."""

from __future__ import annotations

import hashlib
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
from .xfl_profile import XFLArithmeticProfile

CANONICAL_DECLARATIONS = paths.CANONICAL_HOOKS_API_DECLARATIONS
DEFAULT_DECLARATIONS = paths.XAHAU_V1_HOOKS_API_DECLARATIONS

_FRONTEND_DIR = Path(__file__).resolve().parent
_FRONTEND_TSCONFIG = _FRONTEND_DIR / "tsconfig.frontend.json"
_FRONTEND_SOURCES = (
    _FRONTEND_DIR / "compiler_driver.ts",
    _FRONTEND_DIR / "entry_policy.ts",
    _FRONTEND_DIR / "result_validator.ts",
    _FRONTEND_DIR / "xfl_profile_ledger.ts",
    _FRONTEND_DIR / "xfl_profile_policy.ts",
    _FRONTEND_DIR / "node-shim.d.ts",
    _FRONTEND_TSCONFIG,
)


@dataclass(frozen=True)
class CompiledHook:
    bytecode: bytes
    javascript: str
    profile: XFLArithmeticProfile
    source_map: str | None = None


@dataclass(frozen=True)
class PackagedHook:
    artifact: bytes
    bytecode: bytes
    javascript: str
    profile: XFLArithmeticProfile
    source_map: str | None = None


@dataclass(frozen=True)
class _TypeScriptOutput:
    javascript: str
    source_map: str | None
    allow_malformed: bool
    profile: XFLArithmeticProfile


# The frontend declares its own TypeScript in package.json. Prefer that
# install: the version doing the type-checking is then stated where the code
# that depends on it lives, rather than inherited from whatever `tsc` happens
# to be on PATH. A global tsc of a different major silently changes what
# type-checks, and nothing would notice.
_FRONTEND_NODE_MODULES = _FRONTEND_DIR / "node_modules"
_PINNED_TYPESCRIPT_VERSION = "6.0.3"
_TYPESCRIPT_IDENTITY_CACHE: dict[tuple[str, int, int, str, int, int], str] = {}
_PINNED_ESBUILD_VERSION = "0.28.2"
_ESBUILD_VERSION_CACHE: dict[tuple[str, int, int], str] = {}


def _typescript_executable(tsc: str | None) -> str:
    if tsc:
        return tsc
    override = os.environ.get("TSC")
    if override:
        return override
    local = _FRONTEND_NODE_MODULES / ".bin" / "tsc"
    if local.is_file():
        return str(local)
    found = shutil.which("tsc")
    if not found:
        raise RuntimeError(
            "TypeScript compiler not found. Run `npm install` in "
            f"{_FRONTEND_DIR}, or set TSC."
        )
    return found


def _typescript_library(tsc: str | None) -> Path:
    """Locate typescript.js — by package resolution first, layout guess last."""
    local = _FRONTEND_NODE_MODULES / "typescript" / "lib" / "typescript.js"
    if not tsc and not os.environ.get("TSC") and local.is_file():
        return local
    executable = Path(_typescript_executable(tsc)).resolve()
    # Fallback for an explicitly supplied tsc: assume the npm layout
    # (<prefix>/bin/tsc alongside <prefix>/lib/typescript.js). This is a guess
    # and fails loudly rather than silently using a mismatched parser.
    typescript = executable.parent.parent / "lib" / "typescript.js"
    if not typescript.is_file():
        raise RuntimeError(
            "Cannot locate the TypeScript parser beside tsc: "
            f"{executable}. Run `npm install` in {_FRONTEND_DIR} to use the "
            "pinned frontend TypeScript instead."
        )
    return typescript


def _typescript_identity(tsc: str | None) -> str:
    executable = Path(_typescript_executable(tsc)).resolve()
    library = _typescript_library(tsc).resolve()
    executable_stat = executable.stat()
    library_stat = library.stat()
    key = (
        str(executable),
        executable_stat.st_mtime_ns,
        executable_stat.st_size,
        str(library),
        library_stat.st_mtime_ns,
        library_stat.st_size,
    )
    cached = _TYPESCRIPT_IDENTITY_CACHE.get(key)
    if cached is not None:
        return cached

    executable_probe = subprocess.run(
        [str(executable), "--version"],
        capture_output=True,
        check=False,
        text=True,
    )
    executable_version = executable_probe.stdout.strip().removeprefix("Version ")
    if executable_probe.returncode != 0:
        detail = executable_probe.stderr.strip() or "version probe failed"
        raise RuntimeError(f"Cannot execute pinned TypeScript: {detail}")

    node = shutil.which("node")
    if not node:
        raise RuntimeError("Node.js not found; it is required beside TypeScript")
    library_probe = subprocess.run(
        [
            node,
            "-e",
            (
                "const ts=require(process.argv[1]);"
                "process.stdout.write(String(ts.version));"
            ),
            str(library),
        ],
        capture_output=True,
        check=False,
        text=True,
    )
    library_version = library_probe.stdout.strip()
    if library_probe.returncode != 0:
        detail = library_probe.stderr.strip() or "parser version probe failed"
        raise RuntimeError(f"Cannot load pinned TypeScript parser: {detail}")
    if (
        executable_version != _PINNED_TYPESCRIPT_VERSION
        or library_version != _PINNED_TYPESCRIPT_VERSION
    ):
        raise RuntimeError(
            "Hook TypeScript version mismatch: expected "
            f"{_PINNED_TYPESCRIPT_VERSION}, found executable "
            f"{executable_version or '<unknown>'} and parser "
            f"{library_version or '<unknown>'}"
        )

    identity = (
        f"{_PINNED_TYPESCRIPT_VERSION}:"
        f"{executable}:{executable_stat.st_mtime_ns}:{executable_stat.st_size}:"
        f"{library}:{library_stat.st_mtime_ns}:{library_stat.st_size}"
    )
    _TYPESCRIPT_IDENTITY_CACHE[key] = identity
    return identity


def _esbuild_executable(esbuild: str | None) -> Path:
    selected = esbuild or os.environ.get("ESBUILD")
    executable = (
        Path(selected).resolve()
        if selected
        else (_FRONTEND_NODE_MODULES / ".bin" / "esbuild").resolve()
    )
    if not executable.is_file():
        raise RuntimeError(
            "Pinned esbuild not found. Run `npm ci` in "
            f"{_FRONTEND_DIR}, or set ESBUILD to exact esbuild "
            f"{_PINNED_ESBUILD_VERSION}."
        )
    stat = executable.stat()
    key = (str(executable), stat.st_mtime_ns, stat.st_size)
    version = _ESBUILD_VERSION_CACHE.get(key)
    if version is None:
        completed = subprocess.run(
            [str(executable), "--version"],
            capture_output=True,
            check=False,
            text=True,
        )
        version = completed.stdout.strip()
        if completed.returncode != 0:
            detail = completed.stderr.strip() or "version probe failed"
            raise RuntimeError(f"Cannot execute pinned esbuild: {detail}")
        _ESBUILD_VERSION_CACHE[key] = version
    if version != _PINNED_ESBUILD_VERSION:
        raise RuntimeError(
            "Hook bundler version mismatch: expected esbuild "
            f"{_PINNED_ESBUILD_VERSION}, found {version or '<unknown>'}"
        )
    return executable


def _frontend_driver_js(tsc: str | None) -> Path:
    """Emit the TypeScript frontend to a mtime-keyed cache, then run that JS.

    Publish into a stamp-keyed directory after tsc finishes. Parallel
    compile-hook workers (hookz uses one process per fixture) used to
    share /tmp/jshookz-frontend and exec a half-written entry_policy.js.
    """
    missing = [path for path in _FRONTEND_SOURCES if not path.is_file()]
    if missing:
        raise RuntimeError(f"compiler frontend source missing: {missing[0]}")
    stamp = ":".join(
        [
            *(f"{path.name}={path.stat().st_mtime_ns}" for path in _FRONTEND_SOURCES),
            f"typescript={_typescript_identity(tsc)}",
        ]
    )
    cache_root = Path(tempfile.gettempdir()) / "jshookz-frontend"
    cache_root.mkdir(parents=True, exist_ok=True)
    key = hashlib.sha256(stamp.encode()).hexdigest()[:16]
    outdir = cache_root / key
    driver = outdir / "compiler_driver.js"
    policy = outdir / "entry_policy.js"
    xfl_policy = outdir / "xfl_profile_policy.js"
    xfl_ledger = outdir / "xfl_profile_ledger.js"
    if all(path.is_file() for path in (driver, policy, xfl_policy, xfl_ledger)):
        return driver

    staging = Path(tempfile.mkdtemp(prefix=f"{key}-", dir=cache_root))
    try:
        compiled = subprocess.run(
            [
                _typescript_executable(tsc),
                "-p",
                str(_FRONTEND_TSCONFIG),
                "--noEmit",
                "false",
                "--outDir",
                str(staging),
                "--declaration",
                "false",
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        emitted = staging / "compiler_driver.js"
        if compiled.returncode != 0 or not emitted.is_file():
            detail = (
                "\n".join(
                    part.strip()
                    for part in (compiled.stdout, compiled.stderr)
                    if part.strip()
                )
                or "frontend tsc failed"
            )
            raise RuntimeError(f"compiler frontend failed to emit:\n{detail}")
        if not (staging / "entry_policy.js").is_file():
            raise RuntimeError("compiler frontend emit missed entry_policy.js")
        if not (staging / "xfl_profile_policy.js").is_file():
            raise RuntimeError("compiler frontend emit missed xfl_profile_policy.js")
        if not (staging / "xfl_profile_ledger.js").is_file():
            raise RuntimeError("compiler frontend emit missed xfl_profile_ledger.js")
        (staging / "stamp").write_text(stamp)
        try:
            staging.rename(outdir)
        except OSError:
            shutil.rmtree(staging, ignore_errors=True)
            if not all(
                path.is_file() for path in (driver, policy, xfl_policy, xfl_ledger)
            ):
                raise RuntimeError(
                    "compiler frontend cache vanished after a parallel emit"
                )
        return driver
    except Exception:
        shutil.rmtree(staging, ignore_errors=True)
        raise


def _parse_driver_meta(stderr: str) -> tuple[str, bool, XFLArithmeticProfile]:
    kind = "typescript"
    allow_malformed = False
    profile = XFLArithmeticProfile.NONE
    for line in stderr.splitlines():
        if line.startswith("kind="):
            kind = line.split("=", 1)[1].strip()
        elif line.startswith("allowMalformed="):
            allow_malformed = line.split("=", 1)[1].strip() == "1"
        elif line.startswith("xflProfile="):
            raw_profile = line.split("=", 1)[1].strip()
            try:
                profile = XFLArithmeticProfile(raw_profile)
            except ValueError as error:
                raise RuntimeError(
                    f"compiler driver returned unknown XFL profile: {raw_profile!r}"
                ) from error
    return kind, allow_malformed, profile


def _run_compiler_driver(
    source: Path,
    config: Path,
    declarations: Path,
    *,
    tsc: str | None,
    failure_label: str,
) -> tuple[bool, XFLArithmeticProfile]:
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
            str(declarations),
        ],
        capture_output=True,
        check=False,
        text=True,
    )
    kind, allow_malformed, profile = _parse_driver_meta(completed.stderr)
    if completed.returncode == 0:
        return allow_malformed, profile
    body_lines = [
        line
        for line in completed.stderr.splitlines()
        if not line.startswith(
            ("kind=", "allowMalformed=", "xflProfile=", "createProgram=")
        )
    ]
    detail = (
        "\n".join(
            part for part in (*body_lines, completed.stdout.strip()) if part.strip()
        )
        or "compiler driver failed"
    )
    if kind == "xfl":
        raise RuntimeError(f"Hook XFL profile policy failed:\n{detail}")
    if kind == "entry":
        raise RuntimeError(f"Hook entry signature is invalid:\n{detail}")
    if kind == "result":
        raise RuntimeError(
            f"Hook Result must use one of its six legal exits:\n{detail}"
        )
    raise RuntimeError(f"{failure_label}:\n{detail}")


def _normalize_source_map(map_path: Path, source_root: Path) -> str:
    """Make an off-ledger source map stable and prove its sources are honest."""
    try:
        document = json.loads(map_path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(
            f"Hook bundler emitted an invalid source map: {error}"
        ) from error

    if document.get("version") != 3:
        raise RuntimeError("Hook bundler source map is not version 3")
    sources = document.get("sources")
    sources_content = document.get("sourcesContent")
    if not isinstance(sources, list) or not isinstance(sources_content, list):
        raise TypeError("Hook bundler source map must embed every source")
    if not sources or len(sources) != len(sources_content):
        raise RuntimeError("Hook bundler source map sources/content disagree")
    if not isinstance(document.get("mappings"), str) or not document["mappings"]:
        raise RuntimeError("Hook bundler source map has no decoded mappings")

    root = source_root.resolve()
    normalized: list[str] = []
    for raw_source, embedded_content in zip(sources, sources_content, strict=True):
        if not isinstance(raw_source, str) or not isinstance(embedded_content, str):
            raise TypeError("Hook bundler source map contains a malformed source")
        resolved = (map_path.parent / raw_source).resolve()
        try:
            relative = resolved.relative_to(root)
        except ValueError:
            # TypeScript can realpath macOS's /var -> /private/var while
            # retaining /Users for authoring sources. Match the complete,
            # canonical source-root identity in the map path; never guess a
            # same-content suffix or launder an escaped symlink.
            raw_parts = tuple(
                part
                for part in raw_source.replace("\\", "/").split("/")
                if part not in {"", ".", ".."}
            )
            root_parts = tuple(
                part for part in root.parts if part not in {root.anchor, ""}
            )
            candidates: list[tuple[Path, Path]] = []
            for index in range(len(raw_parts) - len(root_parts) + 1):
                if raw_parts[index : index + len(root_parts)] != root_parts:
                    continue
                remainder = raw_parts[index + len(root_parts) :]
                candidate = root.joinpath(*remainder).resolve()
                try:
                    candidate_relative = candidate.relative_to(root)
                except ValueError:
                    continue
                candidates.append((candidate, candidate_relative))
            unique_candidates = {
                (candidate, candidate_relative)
                for candidate, candidate_relative in candidates
            }
            if len(unique_candidates) != 1:
                raise RuntimeError(
                    "Hook bundler source map escaped the Hook source root: "
                    f"{raw_source}"
                )
            resolved, relative = unique_candidates.pop()
        source_text = (
            resolved.read_bytes().decode("utf-8") if resolved.is_file() else None
        )
        if source_text != embedded_content:
            raise RuntimeError(
                f"Hook bundler source map content disagrees with {relative.as_posix()}"
            )
        normalized.append(relative.as_posix())

    if len(set(normalized)) != len(normalized):
        raise RuntimeError("Hook bundler source map contains duplicate source paths")
    document["sources"] = normalized
    document.pop("sourceRoot", None)
    return json.dumps(document, indent=2, sort_keys=True) + "\n"


def _bundle_emitted_javascript(
    emitted: Path,
    *,
    output_dir: Path,
    source_root: Path,
    esbuild: str | None,
    source_map: bool,
) -> tuple[str, str | None]:
    """Bundle checked TS output into exactly one import-free ESM module."""
    bundle_dir = output_dir.parent / "bundle"
    bundle_dir.mkdir()
    bundled = bundle_dir / "hook.js"
    metafile = bundle_dir / "meta.json"
    command = [
        str(_esbuild_executable(esbuild)),
        str(emitted.relative_to(output_dir)),
        "--bundle",
        "--format=esm",
        "--platform=neutral",
        "--target=es2023",
        "--legal-comments=none",
        "--log-level=warning",
        f"--metafile={metafile}",
        f"--outfile={bundled}",
    ]
    if source_map:
        command.extend(("--sourcemap=external", "--sources-content=true"))
    completed = subprocess.run(
        command,
        cwd=output_dir,
        capture_output=True,
        check=False,
        text=True,
    )
    if completed.returncode != 0:
        detail = (
            "\n".join(
                part.strip()
                for part in (completed.stdout, completed.stderr)
                if part.strip()
            )
            or "esbuild failed"
        )
        raise RuntimeError(f"Hook bundling failed:\n{detail}")
    if not bundled.is_file() or not metafile.is_file():
        raise RuntimeError("Hook bundler did not emit its declared module and metadata")

    try:
        metadata = json.loads(metafile.read_text())
    except json.JSONDecodeError as error:
        raise RuntimeError("Hook bundler emitted malformed metadata") from error
    outputs = metadata.get("outputs")
    if not isinstance(outputs, dict):
        raise TypeError("Hook bundler metadata has no outputs")
    javascript_outputs = [
        value
        for name, value in outputs.items()
        if Path(name).resolve() == bundled.resolve()
    ]
    if len(javascript_outputs) != 1:
        raise RuntimeError(
            "Hook bundler did not describe exactly one JavaScript output"
        )
    imports = javascript_outputs[0].get("imports")
    if imports != []:
        raise RuntimeError(
            "Hook bundler left a runtime import in deployable JavaScript"
        )

    expected_files = {"hook.js", "meta.json"}
    map_path = bundled.with_suffix(".js.map")
    if source_map:
        expected_files.add(map_path.name)
    actual_files = {path.name for path in bundle_dir.iterdir() if path.is_file()}
    if actual_files != expected_files:
        raise RuntimeError(
            "Hook bundler output set disagrees with the single-module contract: "
            f"{sorted(actual_files)}"
        )
    normalized_map = (
        _normalize_source_map(map_path, source_root) if source_map else None
    )
    return bundled.read_text(), normalized_map


def _compile_typescript_graph(
    source: Path,
    *,
    declarations: Path,
    tsc: str | None = None,
    esbuild: str | None = None,
    source_map: bool = False,
) -> _TypeScriptOutput:
    if not declarations.is_file():
        raise FileNotFoundError(f"Hook API declarations not found: {declarations}")

    source = source.resolve()
    declarations = declarations.resolve()
    with tempfile.TemporaryDirectory(prefix="qjs-hook-ts-") as temp:
        temp_path = Path(temp)
        out_dir = temp_path / "out"
        config = temp_path / "tsconfig.json"
        compiler_options: dict[str, object] = {
            "lib": ["ES2023"],
            "module": "ESNext",
            "moduleResolution": "Bundler",
            "outDir": str(out_dir),
            "rootDir": str(source.parent),
            "skipLibCheck": False,
            "strict": True,
            "target": "ES2023",
        }
        if source_map:
            compiler_options.update(
                {
                    "sourceMap": True,
                    "sourceRoot": str(source.parent),
                    "inlineSources": True,
                }
            )
        config.write_text(
            json.dumps(
                {
                    "compilerOptions": compiler_options,
                    "files": [str(declarations), str(source)],
                },
                indent=2,
            )
        )
        source_allows, profile = _run_compiler_driver(
            source,
            config,
            declarations,
            tsc=tsc,
            failure_label="TypeScript compilation failed",
        )

        emitted = out_dir / source.with_suffix(".js").name
        if not emitted.is_file():
            raise RuntimeError(f"TypeScript emitted no JavaScript at {emitted}")
        javascript, composed_map = _bundle_emitted_javascript(
            emitted,
            output_dir=out_dir,
            source_root=source.parent,
            esbuild=esbuild,
            source_map=source_map,
        )
        return _TypeScriptOutput(
            javascript=javascript,
            source_map=composed_map,
            allow_malformed=source_allows,
            profile=profile,
        )


def _typescript_to_javascript(
    source: Path,
    *,
    declarations: Path,
    tsc: str | None = None,
    esbuild: str | None = None,
) -> tuple[str, bool, XFLArithmeticProfile]:
    """Compatibility wrapper for callers that do not request source maps."""
    output = _compile_typescript_graph(
        source,
        declarations=declarations,
        tsc=tsc,
        esbuild=esbuild,
    )
    return output.javascript, output.allow_malformed, output.profile


def _check_javascript(
    source: Path,
    *,
    declarations: Path,
    tsc: str | None = None,
) -> tuple[str, bool, XFLArithmeticProfile]:
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
        source_allows, profile = _run_compiler_driver(
            source,
            config,
            declarations,
            tsc=tsc,
            failure_label="JavaScript checking failed",
        )
    return source.read_text(), source_allows, profile


def _compile_already_checked_javascript(
    javascript: str,
    *,
    provider: Path,
    profile: XFLArithmeticProfile,
    emit_malformed: bool,
) -> bytes:
    """Narrow TS-bundle/checked-JS seam into the unchanged qjsc compiler."""
    host = WasmHost(wasm_path=provider)
    host.init()
    try:
        bytecode = host.compile_source(javascript, module=True)
        validation = None if emit_malformed else host.validate_hook_bytecode(bytecode)
    finally:
        host.destroy()
    if validation is not None and not validation.valid:
        raise RuntimeError(
            "QuickJS Hook is not deployable: "
            f"{validation.error or 'provider validation failed'}"
        )
    if validation is not None and validation.profile is not profile:
        raise RuntimeError(
            "QuickJS Hook profile disagreement: compiler selected "
            f"{profile.value}, provider observed {validation.profile.value}"
        )
    return bytecode


def compile_hook(
    source: str | Path,
    *,
    wasm_path: str | Path | None = None,
    declarations: str | Path = DEFAULT_DECLARATIONS,
    tsc: str | None = None,
    esbuild: str | None = None,
    source_map: bool = False,
    allow_malformed: bool = False,
) -> CompiledHook:
    """Compile a .ts/.js Hook to provider-compatible QuickJS bytecode."""
    source_path = Path(source).resolve()
    suffix = source_path.suffix.lower()
    source_allows = False
    composed_map: str | None = None
    profile = XFLArithmeticProfile.NONE
    if suffix == ".ts":
        output = _compile_typescript_graph(
            source_path,
            declarations=Path(declarations),
            tsc=tsc,
            esbuild=esbuild,
            source_map=source_map,
        )
        javascript = output.javascript
        composed_map = output.source_map
        source_allows = output.allow_malformed
        profile = output.profile
    elif suffix in {".js", ".mjs"}:
        if source_map:
            raise ValueError("source maps are available only for TypeScript Hooks")
        javascript, source_allows, profile = _check_javascript(
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
            f"Xahau QuickJS provider not found: {provider}\n{guidance}"
        )

    emit_malformed = allow_malformed or source_allows
    bytecode = _compile_already_checked_javascript(
        javascript,
        provider=provider,
        profile=profile,
        emit_malformed=emit_malformed,
    )
    return CompiledHook(
        bytecode=bytecode,
        javascript=javascript,
        source_map=composed_map,
        profile=profile,
    )


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
    esbuild: str | None = None,
    source_map: bool = False,
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
        esbuild=esbuild,
        source_map=source_map,
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
    if validation.profile is not compiled.profile:
        raise RuntimeError(
            "QuickJS Hook profile disagreement: compiler selected "
            f"{compiled.profile.value}, provider observed {validation.profile.value}"
        )
    return PackagedHook(
        artifact=build_hook_artifact(
            compiled.bytecode,
            hook_api_version=hook_api_version,
            bytecode_abi_id=bytecode_abi_id,
            runtime_profile_id=runtime_profile_id,
            profile=compiled.profile,
        ),
        bytecode=compiled.bytecode,
        javascript=compiled.javascript,
        source_map=compiled.source_map,
        profile=compiled.profile,
    )
