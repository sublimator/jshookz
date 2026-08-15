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
    assert "declare interface Hash256" in v1
    assert "declare const Hash256: Hash256Factory" in v1
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


def test_rollback_require_strips_nullish_result_values(tmp_path: Path):
    source = tmp_path / "require-nullish.hook.ts"
    source.write_text(
        """
        declare const nullable: Result<string | null | undefined, HostError>;
        declare const zeroResult: Result<0, HostError>;

        export function main(): never {
          const present: string = rollback.require(nullable, "required");
          const zero: 0 = rollback.require(zeroResult, "required");
          trace(present, zero);
          return accept();
        }
        """
    )

    assert compile_hook(source).bytecode


def test_rich_roots_are_typed_as_factories_not_constructors(tmp_path: Path):
    source = tmp_path / "factory-roots.hook.ts"
    source.write_text(
        """
        export function main(): never {
          // @ts-expect-error STBlob is a factory object, not a constructor.
          const blob = new STBlob();
          // @ts-expect-error Hash256 is not a base class.
          class DerivedHash extends Hash256 {}
          // @ts-expect-error AccountID has no instanceof contract.
          const account = {} instanceof AccountID;
          // @ts-expect-error XFL exposes no public prototype.
          const prototype = XFL.prototype;
          void blob;
          void DerivedHash;
          void account;
          void prototype;
          return accept();
        }
        """
    )

    assert compile_hook(source).bytecode


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


@pytest.mark.parametrize(
    "body",
    [
        "const result = state.get('key');",
        "const result = state.get('key'); if (result.ok) {}",
        "(state.get('key'), 1);",
        "true ? state.get('key') : 1;",
        "state.get('key').ok;",
        "const { ok } = state.get('key'); void ok;",
        "for (const result of [state.get('key')]) { void result.ok; }",
        (
            "function ignore(result: HostResult<STBlob | undefined>): void {} "
            "ignore(state.get('key'));"
        ),
        "trace('result', state.get('key'));",
        (
            "const result: HostResult<STBlob | undefined> | string = "
            "state.get('key'); void result;"
        ),
        (
            "const result = state.set('key', [1]); "
            "if (ledger.sequence > 0) result.moot();"
        ),
    ],
)
def test_result_dataflow_rejects_unconsumed_shapes(
    tmp_path: Path,
    body: str,
):
    source = tmp_path / "unconsumed-shape.hook.ts"
    source.write_text(
        "export function main(): never { "
        f"{body} "
        "return accept(); }"
    )

    with pytest.raises(RuntimeError, match="six legal exits"):
        compile_hook(source)


@pytest.mark.parametrize(
    "body",
    [
        (
            "let result: HostResult<STBlob | undefined>; "
            "result = state.get('key'); "
            "if (!result.ok) rollback('read failed', result.error.code);"
        ),
        (
            "const first = state.get('key'); const second = first; "
            "rollback.onFail(second);"
        ),
        (
            "function consume(result: HostResult<STBlob | undefined>): void { "
            "rollback.onFail(result); } consume(state.get('key'));"
        ),
        (
            "const result = state.set('key', [1]); "
            "if (ledger.sequence > 0) result.moot(); else result.moot();"
        ),
    ],
)
def test_result_dataflow_accepts_checked_transfers_and_branches(
    tmp_path: Path,
    body: str,
):
    source = tmp_path / "consumed-shape.hook.ts"
    source.write_text(
        "export function main(): never { "
        f"{body} "
        "return accept(); }"
    )

    assert compile_hook(source).bytecode


def test_raw_javascript_uses_the_same_result_dataflow_gate(tmp_path: Path):
    source = tmp_path / "discarded-result.hook.js"
    source.write_text(
        "export function main() { state.get('key'); return accept(); }"
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


def test_compiler_rejects_aliased_host_call_during_module_initialization(
    tmp_path: Path,
):
    source = tmp_path / "aliased-terminal.hook.js"
    source.write_text(
        "const terminal = accept; terminal('not an entry', 1); "
        "export function main() {}"
    )

    with pytest.raises(RuntimeError, match="unavailable during module initialization"):
        compile_hook(source)


@pytest.mark.parametrize(
    "module",
    [
        (
            "function hookMain(): never { return accept(); } "
            "export { hookMain as main };"
        ),
        (
            "function hookMain(): never { return accept(); } "
            "export const main = hookMain;"
        ),
    ],
)
def test_compiler_accepts_live_callable_main_exports(
    tmp_path: Path,
    module: str,
):
    source = tmp_path / "live-main.hook.ts"
    source.write_text(module)

    assert compile_hook(source).bytecode


@pytest.mark.parametrize(
    "module,error",
    [
        (
            "export default function main(): never { return accept(); }",
            "no exported main entry point",
        ),
        (
            "export let main: (() => never) | number = () => accept(); main = 1;",
            "main entry point is not callable",
        ),
        ("export class main {}", "main entry point is not callable"),
    ],
)
def test_compiler_uses_provider_live_export_validation(
    tmp_path: Path,
    module: str,
    error: str,
):
    source = tmp_path / "invalid-live-main.hook.ts"
    source.write_text(module)

    with pytest.raises(RuntimeError, match=error):
        compile_hook(source)


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

    with pytest.raises(RuntimeError, match="not deployable: Error: entry"):
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

    with pytest.raises(RuntimeError, match="unavailable during module initialization"):
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
    with pytest.raises(RuntimeError, match="no exported main entry point"):
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
