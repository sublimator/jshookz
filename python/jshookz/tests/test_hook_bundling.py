import json
import shutil
import subprocess
from argparse import Namespace
from pathlib import Path

import pytest
from jshookz.cli import cmd_compile_hook
from jshookz.hook_compiler import compile_hook, package_hook
from jshookz.host import WasmHost
from jshookz.paths import XAHAU_HOOK_PROVIDER_WASM


def _evaluate(bytecode: bytes) -> str | None:
    host = WasmHost(wasm_path=XAHAU_HOOK_PROVIDER_WASM)
    host.init()
    try:
        return host.run_hook_bytecode(bytecode).error
    finally:
        host.destroy()


def _write_nested_graph(root: Path) -> Path:
    root.mkdir(parents=True, exist_ok=True)
    shared = root / "shared"
    shared.mkdir()
    (shared / "constants.ts").write_text(
        "export const LIMIT = 32 as const;\n"
        "export interface Policy { readonly limit: number }\n"
        "export const POLICY = { limit: LIMIT } satisfies Policy;\n"
    )
    (shared / "policy.ts").write_text(
        'import { POLICY } from "./constants";\n'
        "let initialized = 0;\n"
        "initialized += 1;\n"
        "export function message(): string {\n"
        "  return `bundle:${POLICY.limit}:init:${initialized}`;\n"
        "}\n"
    )
    entry = root / "nested.hook.ts"
    entry.write_text(
        'import { message } from "./shared/policy";\n'
        "export function main(): never { throw new Error(message()); }\n"
    )
    return entry


def test_nested_types_constants_and_pure_initialization_bundle_import_free(
    tmp_path: Path,
):
    entry = _write_nested_graph(tmp_path)

    compiled = compile_hook(entry)

    assert 'from "./' not in compiled.javascript
    assert "import(" not in compiled.javascript
    assert _evaluate(compiled.bytecode) == "Error: bundle:32:init:1"


def test_private_name_collisions_are_isolated_by_bundling(tmp_path: Path):
    (tmp_path / "left.ts").write_text(
        'const privateName = "left";\n'
        "export function left(): string { return privateName; }\n"
    )
    (tmp_path / "right.ts").write_text(
        'const privateName = "right";\n'
        "export function right(): string { return privateName; }\n"
    )
    entry = tmp_path / "collisions.hook.ts"
    entry.write_text(
        'import { left } from "./left";\n'
        'import { right } from "./right";\n'
        "export function main(): never {\n"
        "  throw new Error(`${left()}:${right()}`);\n"
        "}\n"
    )

    assert _evaluate(compile_hook(entry).bytecode) == "Error: left:right"


def test_missing_relative_dependency_fails_closed(tmp_path: Path):
    entry = tmp_path / "missing.hook.ts"
    entry.write_text(
        'import { missing } from "./missing";\n'
        "export function main(): never { throw new Error(String(missing)); }\n"
    )

    with pytest.raises(RuntimeError, match="Cannot find module './missing'"):
        compile_hook(entry)


def test_resolvable_bare_dependency_is_still_rejected(tmp_path: Path):
    package = tmp_path / "node_modules" / "fixture-package"
    package.mkdir(parents=True)
    (package / "package.json").write_text(
        json.dumps({"name": "fixture-package", "types": "index.d.ts"})
    )
    (package / "index.d.ts").write_text("export const VALUE: 7;\n")
    entry = tmp_path / "bare.hook.ts"
    entry.write_text(
        'import { VALUE } from "fixture-package";\n'
        "export function main(): never { throw new Error(String(VALUE)); }\n"
    )

    with pytest.raises(RuntimeError, match="bare module specifiers are forbidden"):
        compile_hook(entry)


@pytest.mark.parametrize(
    ("helper_body", "message"),
    [
        (
            (
                "export async function forbidden(): Promise<unknown> { "
                'return import("./later"); }\n'
            ),
            r"dynamic import\(\)",
        ),
        (
            "export function forbidden(): ImportMeta { return import.meta; }\n",
            "import.meta",
        ),
    ],
)
def test_dynamic_linkage_is_rejected_anywhere_in_graph(
    tmp_path: Path, helper_body: str, message: str
):
    (tmp_path / "later.ts").write_text("export const later = 1;\n")
    (tmp_path / "helper.ts").write_text(helper_body)
    entry = tmp_path / "dynamic.hook.ts"
    entry.write_text(
        'import { forbidden } from "./helper";\n'
        "export function main(): never { throw new Error(String(forbidden)); }\n"
    )

    with pytest.raises(RuntimeError, match=message):
        compile_hook(entry)


def test_result_ownership_policy_applies_to_imported_helpers(tmp_path: Path):
    (tmp_path / "helper.ts").write_text(
        'export function leak(): void { state.get("unchecked"); }\n'
    )
    entry = tmp_path / "result.hook.ts"
    entry.write_text(
        'import { leak } from "./helper";\n'
        "export function main(): never { leak(); accept(); }\n"
    )

    with pytest.raises(RuntimeError, match="six legal exits"):
        compile_hook(entry)


def test_composed_source_map_is_stable_embedded_and_decodable(tmp_path: Path):
    entry = _write_nested_graph(tmp_path)
    compiled = compile_hook(entry, source_map=True)
    assert compiled.source_map is not None
    source_map = json.loads(compiled.source_map)

    assert set(source_map["sources"]) == {
        "nested.hook.ts",
        "shared/constants.ts",
        "shared/policy.ts",
    }
    for source, content in zip(
        source_map["sources"], source_map["sourcesContent"], strict=True
    ):
        assert content == (tmp_path / source).read_text()
    assert str(tmp_path) not in compiled.source_map
    assert "qjs-hook-ts-" not in compiled.source_map

    node = shutil.which("node")
    assert node is not None
    generated_line = next(
        index
        for index, line in enumerate(compiled.javascript.splitlines())
        if "bundle:" in line
    )
    generated_column = compiled.javascript.splitlines()[generated_line].index("bundle:")
    map_path = tmp_path / "composed.map"
    map_path.write_text(compiled.source_map)
    decoded = subprocess.run(
        [
            node,
            "-e",
            (
                "const fs=require('node:fs');"
                "const {SourceMap}=require('node:module');"
                "const map=new SourceMap(JSON.parse(fs.readFileSync(process.argv[1])));"
                "console.log(JSON.stringify(map.findEntry("
                "Number(process.argv[2]),Number(process.argv[3]))));"
            ),
            str(map_path),
            str(generated_line),
            str(generated_column),
        ],
        capture_output=True,
        check=True,
        text=True,
    )
    entry_mapping = json.loads(decoded.stdout)
    assert entry_mapping["originalSource"] == "shared/policy.ts"
    assert entry_mapping["originalLine"] > 0


def test_bundled_output_source_map_and_qjsc_are_deterministic(tmp_path: Path):
    entry = _write_nested_graph(tmp_path)

    compiled = [compile_hook(entry, source_map=True) for _ in range(3)]

    assert len({item.javascript for item in compiled}) == 1
    assert len({item.source_map for item in compiled}) == 1
    assert len({item.bytecode for item in compiled}) == 1


def test_multi_file_and_handwritten_single_file_have_behavioral_parity(
    tmp_path: Path,
):
    nested = _write_nested_graph(tmp_path)
    single = tmp_path / "single.hook.ts"
    single.write_text(
        "const LIMIT = 32 as const;\n"
        "let initialized = 0;\n"
        "initialized += 1;\n"
        "export function main(): never {\n"
        "  throw new Error(`bundle:${LIMIT}:init:${initialized}`);\n"
        "}\n"
    )

    assert _evaluate(compile_hook(nested).bytecode) == _evaluate(
        compile_hook(single).bytecode
    )


def test_source_map_is_off_ledger_and_raw_javascript_stays_strict(tmp_path: Path):
    entry = _write_nested_graph(tmp_path)
    with_map = package_hook(
        entry,
        hook_api_version=1,
        bytecode_abi_id=b"a" * 32,
        runtime_profile_id=b"b" * 32,
        source_map=True,
    )
    without_map = package_hook(
        entry,
        hook_api_version=1,
        bytecode_abi_id=b"a" * 32,
        runtime_profile_id=b"b" * 32,
    )
    assert with_map.source_map is not None
    assert with_map.artifact == without_map.artifact

    raw = tmp_path / "unchecked.js"
    raw.write_text(
        "function identity(value) { return value; }\n"
        "export function main() { throw new Error(identity('raw')); }\n"
    )
    with pytest.raises(RuntimeError, match="implicitly has an 'any' type"):
        compile_hook(raw)
    with pytest.raises(ValueError, match="only for TypeScript"):
        compile_hook(raw, source_map=True)

    helper = tmp_path / "raw-helper.js"
    helper.write_text("export const VALUE = 1;\n")
    importing_raw = tmp_path / "importing.js"
    importing_raw.write_text(
        'import { VALUE } from "./raw-helper.js";\n'
        "export function main() { throw new Error(String(VALUE)); }\n"
    )
    with pytest.raises(RuntimeError, match="JavaScript Hooks must not import"):
        compile_hook(importing_raw)


def test_compile_cli_can_write_bundle_and_off_ledger_source_map(
    tmp_path: Path,
):
    entry = _write_nested_graph(tmp_path / "source")
    bytecode = tmp_path / "hook.qjsc"
    javascript = tmp_path / "hook.js"
    source_map = tmp_path / "hook.js.map"

    assert (
        cmd_compile_hook(
            Namespace(
                source=str(entry),
                output=str(bytecode),
                wasm=None,
                declarations=None,
                tsc=None,
                esbuild=None,
                emit_js=str(javascript),
                emit_source_map=str(source_map),
                allow_malformed=False,
            )
        )
        == 0
    )
    assert bytecode.read_bytes()
    assert javascript.read_text()
    assert json.loads(source_map.read_text())["sources"]


def test_esbuild_dependency_and_runtime_are_exactly_pinned(tmp_path: Path):
    package_root = Path(__file__).parents[1] / "src" / "jshookz"
    package = json.loads((package_root / "package.json").read_text())
    lock = json.loads((package_root / "package-lock.json").read_text())
    assert package["devDependencies"]["esbuild"] == "0.28.2"
    assert lock["packages"]["node_modules/esbuild"]["version"] == "0.28.2"

    fake = tmp_path / "esbuild"
    fake.write_text("#!/bin/sh\necho 0.28.1\n")
    fake.chmod(0o755)
    entry = _write_nested_graph(tmp_path / "hook")
    with pytest.raises(RuntimeError, match="expected esbuild 0.28.2, found 0.28.1"):
        compile_hook(entry, esbuild=str(fake))
