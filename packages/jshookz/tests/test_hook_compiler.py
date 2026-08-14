from pathlib import Path

import pytest

from jshookz.hook_artifact import HEADER_SIZE, parse_hook_artifact
from jshookz.hook_compiler import DEFAULT_DECLARATIONS, compile_hook, package_hook
from jshookz.host import WasmHost
from jshookz.paths import (
    CANONICAL_HOOKS_API_DECLARATIONS,
    XAHAU_HOOK_PROVIDER_WASM,
    XAHAU_V1_HOOKS_API_DECLARATIONS,
)


def test_public_declarations_are_package_data_and_v1_is_default():
    assert CANONICAL_HOOKS_API_DECLARATIONS.is_file()
    assert XAHAU_V1_HOOKS_API_DECLARATIONS.is_file()
    assert DEFAULT_DECLARATIONS == XAHAU_V1_HOOKS_API_DECLARATIONS
    assert ".ai-docs" not in str(CANONICAL_HOOKS_API_DECLARATIONS)
    declarations = CANONICAL_HOOKS_API_DECLARATIONS.read_text()
    assert "declare interface STObject" in declarations
    assert "declare namespace ledger" in declarations
    v1 = XAHAU_V1_HOOKS_API_DECLARATIONS.read_text()
    assert "declare class Hash256" in v1
    assert "declare function record" not in v1


def test_default_v1_declarations_reject_unimplemented_rich_api(tmp_path: Path):
    source = tmp_path / "future-api.hook.ts"
    source.write_text(
        "export function main(): never { "
        "record('future', 1, { value: record.u8(0) }); "
        "return accept(); }"
    )

    with pytest.raises(RuntimeError, match="Cannot find name 'record'"):
        compile_hook(source)


def test_v1_example_compiles_and_packages(tmp_path: Path):
    source = tmp_path / "v1.hook.ts"
    source.write_text(
        "export function main(_reserved: number): never { "
        "void _reserved; trace('ledger', ledger.sequence); "
        "return accept('ok', 0); }"
    )

    compiled = compile_hook(source)
    packaged = package_hook(
        source,
        hook_api_version=1,
        bytecode_abi_id=bytes(range(32)),
        runtime_profile_id=bytes(range(32, 64)),
    )

    assert compiled.bytecode
    assert packaged.artifact


def test_typescript_hook_compiles_to_provider_bytecode(tmp_path: Path):
    source = tmp_path / "accept.hook.ts"
    source.write_text(
        """
        export function main(_reserved: number): never {
          throw new Error(`entered main:${_reserved}`);
        }
        """
    )

    result = compile_hook(source)

    assert result.bytecode
    assert "_reserved" in result.javascript

    host = WasmHost(wasm_path=XAHAU_HOOK_PROVIDER_WASM)
    host.init()
    evaluated = host.run_hook_bytecode(result.bytecode, reserved=7)
    assert evaluated.exit_code == -1
    assert evaluated.error == "Error: entered main:7"


def test_typescript_hook_packages_payload_with_explicit_identities(tmp_path: Path):
    source = tmp_path / "packaged.hook.ts"
    source.write_text(
        """
        export function main(_reserved: number): never {
          throw new Error(`packaged:${_reserved}`);
        }
        """
    )
    abi_id = bytes(range(32))
    profile_id = bytes(range(32, 64))

    packaged = package_hook(
        source,
        hook_api_version=1,
        bytecode_abi_id=abi_id,
        runtime_profile_id=profile_id,
    )
    parsed = parse_hook_artifact(packaged.artifact)

    assert len(packaged.artifact) == HEADER_SIZE + len(packaged.bytecode)
    assert parsed.hook_api_version == 1
    assert parsed.bytecode_abi_id == abi_id
    assert parsed.runtime_profile_id == profile_id
    assert parsed.payload == packaged.bytecode

    host = WasmHost(wasm_path=XAHAU_HOOK_PROVIDER_WASM)
    host.init()
    evaluated = host.run_hook_bytecode(parsed.payload, reserved=11)
    assert evaluated.exit_code == -1
    assert evaluated.error == "Error: packaged:11"


def test_packager_rejects_host_calls_during_module_initialization(
    tmp_path: Path,
):
    source = tmp_path / "host-init.hook.js"
    source.write_text("export function main() {}\nvoid ledger.sequence;")

    with pytest.raises(RuntimeError, match="not deployable"):
        package_hook(
            source,
            hook_api_version=1,
            bytecode_abi_id=bytes(range(32)),
            runtime_profile_id=bytes(range(32, 64)),
        )


def test_compiler_rejects_top_level_self_invocation(tmp_path: Path):
    source = tmp_path / "self-invoking.hook.ts"
    source.write_text(
        """
        export function main(_reserved: number): never {
          throw new Error("entry");
        }
        main(0);
        """
    )

    with pytest.raises(RuntimeError, match="provider invokes them"):
        compile_hook(source)


def test_compiler_rejects_top_level_terminal_invocation(tmp_path: Path):
    source = tmp_path / "top-level-terminal.hook.ts"
    source.write_text(
        """
        export function main(_reserved: number): never {
          throw new Error("entry");
        }
        accept("not an entry", 1);
        """
    )

    with pytest.raises(RuntimeError, match="terminal invocation"):
        compile_hook(source)


def test_provider_refuses_missing_main_export(tmp_path: Path):
    source = tmp_path / "missing-entry.hook.ts"
    source.write_text(
        """
        const marker = "ordinary module initialization";
        export function other(_reserved: number): never {
          throw new Error(marker);
        }
        """
    )
    compiled = compile_hook(source)

    host = WasmHost(wasm_path=XAHAU_HOOK_PROVIDER_WASM)
    host.init()
    evaluated = host.run_hook_bytecode(compiled.bytecode)

    assert evaluated.exit_code == -1
    assert evaluated.error == "TypeError: Hook module has no exported main entry point"


def test_provider_dispatches_callback_export(tmp_path: Path):
    source = tmp_path / "callback.hook.ts"
    source.write_text(
        """
        export function callback(_reserved: number): never {
          throw new Error(`entered callback:${_reserved}`);
        }
        """
    )
    compiled = compile_hook(source)

    host = WasmHost(wasm_path=XAHAU_HOOK_PROVIDER_WASM)
    host.init()
    evaluated = host.run_hook_bytecode(
        compiled.bytecode,
        export="cbak",
        reserved=9,
    )

    assert evaluated.exit_code == -1
    assert evaluated.error == "Error: entered callback:9"
