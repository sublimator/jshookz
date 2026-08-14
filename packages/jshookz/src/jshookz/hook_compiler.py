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

_ENTRY_INVOCATION_VALIDATOR = r"""
const ts = require(process.argv[1]);
const fs = require("fs");
const source = fs.readFileSync(0, "utf8");
const file = ts.createSourceFile(
  "<hook-module>.js",
  source,
  ts.ScriptTarget.Latest,
  true,
  ts.ScriptKind.JS,
);
const violations = [];

function unparenthesize(node) {
  while (ts.isParenthesizedExpression(node)) node = node.expression;
  return node;
}

function visit(node) {
  if (node !== file && (ts.isFunctionLike(node) || ts.isClassLike(node))) return;
  if (ts.isCallExpression(node)) {
    const target = unparenthesize(node.expression);
    let forbidden = null;
    if (
      ts.isIdentifier(target) &&
      ["hook", "cbak", "accept", "rollback"].includes(target.text)
    ) {
      forbidden = target.text;
    }
    if (forbidden !== null) {
      const position = file.getLineAndCharacterOfPosition(node.getStart(file));
      violations.push(`${forbidden} at ${position.line + 1}:${position.character + 1}`);
    }
  }
  ts.forEachChild(node, visit);
}

for (const statement of file.statements) visit(statement);
if (violations.length) {
  console.error(violations.join(", "));
  process.exit(2);
}
"""


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


def _validate_no_top_level_entry_invocation(
    javascript: str,
    *,
    tsc: str | None,
) -> None:
    """Reject the old fixture cheat where an ES module invokes itself."""
    executable = _typescript_executable(tsc)
    resolved_tsc = Path(executable).resolve()
    typescript = resolved_tsc.parent.parent / "lib" / "typescript.js"
    if not typescript.is_file():
        raise RuntimeError(
            "Cannot locate the TypeScript parser beside tsc: " f"{resolved_tsc}"
        )
    node = shutil.which("node")
    if not node:
        raise RuntimeError("Node.js not found; it is required beside tsc")

    completed = subprocess.run(
        [node, "-e", _ENTRY_INVOCATION_VALIDATOR, str(typescript)],
        input=javascript,
        capture_output=True,
        text=True,
    )
    if completed.returncode:
        detail = completed.stderr.strip() or "entry invocation found"
        raise RuntimeError(
            "Hook modules export hook/cbak and the provider invokes them. "
            f"Remove top-level entry or terminal invocation ({detail})."
        )


def _typescript_to_javascript(
    source: Path,
    *,
    declarations: Path,
    tsc: str | None = None,
) -> str:
    executable = _typescript_executable(tsc)
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
        completed = subprocess.run(
            [executable, "-p", str(config)],
            capture_output=True,
            text=True,
        )
        if completed.returncode:
            detail = "\n".join(
                part.strip()
                for part in (completed.stdout, completed.stderr)
                if part.strip()
            )
            raise RuntimeError(f"TypeScript compilation failed:\n{detail}")

        emitted = out_dir / source.with_suffix(".js").name
        if not emitted.is_file():
            raise RuntimeError(f"TypeScript emitted no JavaScript at {emitted}")
        return emitted.read_text()


def compile_hook(
    source: str | Path,
    *,
    wasm_path: str | Path | None = None,
    declarations: str | Path = DEFAULT_DECLARATIONS,
    tsc: str | None = None,
) -> CompiledHook:
    """Compile a .ts/.js Hook to provider-compatible QuickJS bytecode."""
    source_path = Path(source).resolve()
    suffix = source_path.suffix.lower()
    if suffix == ".ts":
        javascript = _typescript_to_javascript(
            source_path,
            declarations=Path(declarations),
            tsc=tsc,
        )
    elif suffix in {".js", ".mjs"}:
        javascript = source_path.read_text()
    else:
        raise ValueError(
            f"unsupported Hook source extension {suffix!r}; expected .ts or .js"
        )

    _validate_no_top_level_entry_invocation(javascript, tsc=tsc)

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

    host = WasmHost(wasm_path=provider)
    host.init()
    try:
        bytecode = host.compile_source(javascript, module=True)
    finally:
        host.destroy()
    return CompiledHook(bytecode=bytecode, javascript=javascript)


def package_hook(
    source: str | Path,
    *,
    hook_api_version: int,
    bytecode_abi_id: bytes,
    runtime_profile_id: bytes,
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
    validator = WasmHost(wasm_path=provider)
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
