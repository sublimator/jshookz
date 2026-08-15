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


def test_result_moot_is_restricted_to_void_results(tmp_path: Path):
    source = tmp_path / "moot-value.hook.ts"
    source.write_text(
        "export function main(): never { "
        "state.get('meaningful').moot(); "
        "return accept(); }"
    )

    with pytest.raises(RuntimeError, match="Property 'moot' does not exist"):
        compile_hook(source)


@pytest.mark.parametrize(
    "statement",
    [
        "state.get('key');",
        "state.set('key', new Uint8Array([1]));",
        "void state.set('key', new Uint8Array([1]));",
    ],
)
def test_compiler_rejects_discarded_results(tmp_path: Path, statement: str):
    source = tmp_path / "discarded-result.hook.ts"
    source.write_text(
        "export function main(): never { "
        f"{statement} "
        "return accept(); }"
    )

    with pytest.raises(RuntimeError, match="six legal exits"):
        compile_hook(source)


def test_v1_example_compiles_and_packages(tmp_path: Path):
    source = tmp_path / "v1.hook.ts"
    source.write_text(
        "export function main(): never { "
        "trace('ledger', ledger.sequence); "
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
        export function main(): never {
          throw new Error("entered main");
        }
        export function callback(info: CallbackInfo): never {
          throw new Error(`entered callback:${info.failed}:${info.rawFlags}`);
        }
        """
    )

    result = compile_hook(source)

    assert result.bytecode
    assert "rawFlags" in result.javascript

    host = WasmHost(wasm_path=XAHAU_HOOK_PROVIDER_WASM)
    host.init()
    evaluated = host.run_hook_bytecode(
        result.bytecode,
        export="cbak",
        reserved=7,
    )
    assert evaluated.exit_code == -1
    assert evaluated.error == "Error: entered callback:true:7"


def test_typescript_hook_packages_payload_with_explicit_identities(tmp_path: Path):
    source = tmp_path / "packaged.hook.ts"
    source.write_text(
        """
        export function main(): never {
          throw new Error("entered main");
        }
        export function callback(info: CallbackInfo): never {
          throw new Error(`packaged:${info.failed}:${info.rawFlags}`);
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
    evaluated = host.run_hook_bytecode(
        parsed.payload,
        export="cbak",
        reserved=10,
    )
    assert evaluated.exit_code == -1
    assert evaluated.error == "Error: packaged:false:10"


def test_main_does_not_receive_the_raw_callback_word(tmp_path: Path):
    source = tmp_path / "main-arguments.hook.ts"
    source.write_text(
        """
        export function main(...args: unknown[]): never {
          throw new Error(`main arguments:${args.length}`);
        }
        """
    )
    compiled = compile_hook(source)

    host = WasmHost(wasm_path=XAHAU_HOOK_PROVIDER_WASM)
    host.init()
    evaluated = host.run_hook_bytecode(compiled.bytecode, reserved=9)

    assert evaluated.exit_code == -1
    assert evaluated.error == "Error: main arguments:0"


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
        export function main(): never {
          throw new Error("entry");
        }
        main();
        """
    )

    with pytest.raises(RuntimeError, match="provider invokes them"):
        compile_hook(source)


def test_compiler_rejects_top_level_terminal_invocation(tmp_path: Path):
    source = tmp_path / "top-level-terminal.hook.ts"
    source.write_text(
        """
        export function main(): never {
          throw new Error("entry");
        }
        accept("not an entry", 1);
        """
    )

    with pytest.raises(RuntimeError, match="terminal invocation"):
        compile_hook(source)


def test_compiler_refuses_missing_main_export(tmp_path: Path):
    source = tmp_path / "missing-entry.hook.ts"
    source.write_text(
        """
        const marker = "ordinary module initialization";
        export function other(_reserved: number): never {
          throw new Error(marker);
        }
        """
    )
    with pytest.raises(RuntimeError, match="missing exported main entry point"):
        compile_hook(source)


def test_provider_dispatches_callback_export(tmp_path: Path):
    source = tmp_path / "callback.hook.ts"
    source.write_text(
        """
        export function main(): never {
          throw new Error("entered main");
        }
        export function callback(info: CallbackInfo): never {
          throw new Error(`entered callback:${info.failed}:${info.rawFlags}`);
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
    assert evaluated.error == "Error: entered callback:true:9"
