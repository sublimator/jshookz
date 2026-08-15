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
const entries = new Map();

function unparenthesize(node) {
  while (ts.isParenthesizedExpression(node)) node = node.expression;
  return node;
}

function isExported(node) {
  return (ts.getCombinedModifierFlags(node) & ts.ModifierFlags.Export) !== 0;
}

for (const statement of file.statements) {
  if (ts.isFunctionDeclaration(statement) && statement.name && isExported(statement)) {
    entries.set(statement.name.text, true);
  } else if (ts.isVariableStatement(statement) && isExported(statement)) {
    for (const declaration of statement.declarationList.declarations) {
      if (!ts.isIdentifier(declaration.name)) continue;
      const value = declaration.initializer && unparenthesize(declaration.initializer);
      entries.set(
        declaration.name.text,
        !!value && (ts.isArrowFunction(value) || ts.isFunctionExpression(value)),
      );
    }
  } else if (isExported(statement) && statement.name && ts.isIdentifier(statement.name)) {
    entries.set(statement.name.text, false);
  }
}

if (!entries.has("main")) {
  violations.push("missing exported main entry point");
} else if (!entries.get("main")) {
  violations.push("exported main entry point is not callable");
}
if (entries.has("callback") && !entries.get("callback")) {
  violations.push("exported callback entry point is not callable");
}

function visit(node) {
  if (node !== file && (ts.isFunctionLike(node) || ts.isClassLike(node))) return;
  if (ts.isCallExpression(node)) {
    const target = unparenthesize(node.expression);
    let forbidden = null;
    if (
      ts.isIdentifier(target) &&
      ["main", "callback", "accept", "rollback"].includes(target.text)
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

_RESULT_CONSUMPTION_VALIDATOR = r"""
const ts = require(process.argv[1]);
const configPath = process.argv[2];
const sourcePath = process.argv[3];
const configFile = ts.readConfigFile(configPath, ts.sys.readFile);
if (configFile.error) {
  console.error(ts.flattenDiagnosticMessageText(configFile.error.messageText, "\n"));
  process.exit(2);
}
const parsed = ts.parseJsonConfigFileContent(
  configFile.config,
  ts.sys,
  require("path").dirname(configPath),
);
const program = ts.createProgram(parsed.fileNames, parsed.options);
const checker = program.getTypeChecker();
const source = program.getSourceFile(sourcePath);
if (!source) {
  console.error(`source file is absent from TypeScript program: ${sourcePath}`);
  process.exit(2);
}
const resultMembers = ["ok", "okOr", "okOrHandle", "okMapOr"];
const violations = [];

function isResult(type) {
  if (type.flags & (ts.TypeFlags.Any | ts.TypeFlags.Unknown)) return false;
  const apparent = checker.getApparentType(type);
  return resultMembers.every(name => checker.getPropertyOfType(apparent, name));
}

function reject(node, detail) {
  const position = source.getLineAndCharacterOfPosition(node.getStart(source));
  violations.push(`${position.line + 1}:${position.character + 1}: ${detail}`);
}

function visit(node) {
  if (
    ts.isVoidExpression(node) &&
    isResult(checker.getTypeAtLocation(node.expression))
  ) {
    reject(node, "void cannot discard a Result");
  } else if (
    ts.isExpressionStatement(node) &&
    isResult(checker.getTypeAtLocation(node.expression))
  ) {
    reject(node, "Result reaches the end of a statement without a legal exit");
  }
  ts.forEachChild(node, visit);
}

visit(source);
if (violations.length) {
  console.error(violations.join("\n"));
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
            "Hook modules export main/callback and the provider invokes them. "
            f"Remove top-level entry or terminal invocation ({detail})."
        )


def _validate_result_consumption(
    source: Path,
    config: Path,
    *,
    tsc: str | None,
) -> None:
    """Reject direct or ``void``-laundered Result expression statements."""
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
        [
            node,
            "-e",
            _RESULT_CONSUMPTION_VALIDATOR,
            str(typescript),
            str(config),
            str(source),
        ],
        capture_output=True,
        text=True,
    )
    if completed.returncode:
        detail = completed.stderr.strip() or "unconsumed Result"
        raise RuntimeError(
            "Hook Result must use one of its six legal exits:\n" f"{detail}"
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

        _validate_result_consumption(source, config, tsc=tsc)

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
