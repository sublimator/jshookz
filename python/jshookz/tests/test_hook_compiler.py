import json
import os
import subprocess
from pathlib import Path

import pytest
from jshookz.hook_artifact import HEADER_SIZE, parse_hook_artifact
from jshookz.hook_compiler import (
    DEFAULT_DECLARATIONS,
    XFLArithmeticProfile,
    _check_javascript,
    _typescript_to_javascript,
    compile_hook,
    package_hook,
)
from jshookz.host import HookModuleValidation, WasmHost
from jshookz.paths import (
    CANONICAL_HOOKS_API_DECLARATIONS,
    XAHAU_HOOK_PROVIDER_WASM,
    XAHAU_V1_HOOKS_API_DECLARATIONS,
)


_XFL_IMPLEMENTED_METHODS = ("add", "divide", "multiply", "subtract")


def _xfl_test_declaration(suffix: str) -> tuple[str, str]:
    if suffix == ".ts":
        return "declare const decimal: XFLDecimal;\n", ": never"
    return (
        "/** @type {XFLDecimal} */ const decimal = "
        "/** @type {*} */ (globalThis).decimal;\n",
        "",
    )


def test_public_declarations_are_package_data_and_v1_is_default():
    assert CANONICAL_HOOKS_API_DECLARATIONS.is_file()
    assert XAHAU_V1_HOOKS_API_DECLARATIONS.is_file()
    assert DEFAULT_DECLARATIONS == XAHAU_V1_HOOKS_API_DECLARATIONS
    assert ".ai-docs" not in str(CANONICAL_HOOKS_API_DECLARATIONS)
    declarations = CANONICAL_HOOKS_API_DECLARATIONS.read_text()
    assert declarations.startswith("export {};")
    assert "declare global {" in declarations
    assert "class STObject" in declarations
    assert "namespace ledger" in declarations
    v1 = XAHAU_V1_HOOKS_API_DECLARATIONS.read_text()
    assert "interface Hash256" in v1
    assert "const Hash256: Hash256Factory" in v1
    assert "declare function record" not in v1


@pytest.mark.parametrize("suffix", [".ts", ".js"])
@pytest.mark.parametrize(
    ("expression", "expected"),
    [
        ("TransactionType.Invoke", "99"),
        ("TransactionResult.telLOCAL_ERROR", "-399"),
        ("HookReturnCode.INVALID_FLOAT", "-10024"),
    ],
)
def test_checked_hook_languages_share_runtime_enum_namespace_values(
    tmp_path: Path, suffix: str, expression: str, expected: str
):
    source = tmp_path / f"runtime-enum.hook{suffix}"
    annotation = ": never" if suffix == ".ts" else ""
    source.write_text(
        f"export function main(){annotation} {{ "
        f"throw new Error(`enum:${{{expression}}}`); }}"
    )
    compiled = compile_hook(source)

    host = WasmHost(wasm_path=XAHAU_HOOK_PROVIDER_WASM)
    host.init()
    try:
        evaluated = host.run_hook_bytecode(compiled.bytecode)
    finally:
        host.destroy()

    assert evaluated.error == f"Error: enum:{expected}"


def test_default_v1_declarations_reject_unimplemented_rich_api(tmp_path: Path):
    source = tmp_path / "future-api.hook.ts"
    source.write_text(
        "export function main(): never { "
        "record('future', 1, { value: record.u8(0) }); "
        "return accept(); }"
    )

    with pytest.raises(RuntimeError, match="Cannot find name 'record'"):
        compile_hook(source)


@pytest.mark.parametrize("suffix", [".ts", ".js"])
@pytest.mark.parametrize(
    "member,expected",
    [
        ("xahauFloatV1", XFLArithmeticProfile.XAHAU_FLOAT_V1),
        ("nearestEvenV1", XFLArithmeticProfile.NEAREST_EVEN_V1),
    ],
)
def test_xfl_policy_preserves_canonical_config_profile(
    tmp_path: Path,
    suffix: str,
    member: str,
    expected: XFLArithmeticProfile,
):
    source = tmp_path / f"config-only.hook{suffix}"
    annotation = ": never" if suffix == ".ts" else ""
    source.write_text(
        "export const hookConfig = defineHookConfig({ "
        f"xflArithmetic: XFLProfile.{member} }});\n"
        f"export function main(){annotation} {{ return accept(); }}\n"
    )

    if suffix == ".ts":
        _, _, profile = _typescript_to_javascript(
            source,
            declarations=CANONICAL_HOOKS_API_DECLARATIONS,
        )
    else:
        _, _, profile = _check_javascript(
            source,
            declarations=CANONICAL_HOOKS_API_DECLARATIONS,
        )

    assert profile is expected


@pytest.mark.parametrize(
    "declaration",
    [
        "export const hookConfig = defineHookConfig({ xflArithmetic: 0 });",
        (
            "export const hookConfig: HookConfig = defineHookConfig({ "
            "xflArithmetic: XFLProfile.xahauFloatV1 });"
        ),
        (
            "export let hookConfig = defineHookConfig({ "
            "xflArithmetic: XFLProfile.xahauFloatV1 });"
        ),
        (
            "const hookConfig = defineHookConfig({ "
            "xflArithmetic: XFLProfile.xahauFloatV1 });"
        ),
        (
            "export const hookConfig = (defineHookConfig)({ "
            "xflArithmetic: XFLProfile.xahauFloatV1 });"
        ),
        (
            "export const hookConfig = defineHookConfig("
            "{ xflArithmetic: XFLProfile.xahauFloatV1 } "
            "satisfies HookConfig);"
        ),
        (
            "const hookConfig = defineHookConfig({ "
            "xflArithmetic: XFLProfile.xahauFloatV1 }); "
            "export { hookConfig };"
        ),
        (
            "export const hookConfig = defineHookConfig({ "
            "['xflArithmetic']: XFLProfile.xahauFloatV1 });"
        ),
        (
            "export const hookConfig = defineHookConfig({ "
            "...{ xflArithmetic: XFLProfile.xahauFloatV1 } });"
        ),
        (
            "export const hookConfig = defineHookConfig({ "
            "get xflArithmetic() { return XFLProfile.xahauFloatV1; } });"
        ),
        ("export default { xflArithmetic: XFLProfile.xahauFloatV1 };"),
        (
            "export const renamed = defineHookConfig({ "
            "xflArithmetic: XFLProfile.xahauFloatV1 });"
        ),
        (
            "export const hookConfig = defineHookConfig({ "
            "xflArithmetic: XFLProfile.xahauFloatV1 }); "
            "hookConfig = defineHookConfig({ "
            "xflArithmetic: XFLProfile.nearestEvenV1 });"
        ),
    ],
)
def test_xfl_policy_rejects_malformed_config_without_sensitive_use(
    tmp_path: Path,
    declaration: str,
):
    source = tmp_path / "malformed-config.hook.ts"
    source.write_text(
        f"{declaration}\nexport function main(): never {{ return accept(); }}\n"
    )

    with pytest.raises(RuntimeError, match="JSH-XFL001"):
        _typescript_to_javascript(
            source,
            declarations=CANONICAL_HOOKS_API_DECLARATIONS,
        )


def test_xfl_policy_rejects_multi_declarator_export_const(tmp_path: Path):
    source = tmp_path / "multi-config.hook.ts"
    source.write_text(
        "export const hookConfig=defineHookConfig({xflArithmetic:"
        "XFLProfile.xahauFloatV1}),extra=1;"
        "export function main():never{return accept();}"
    )

    with pytest.raises(RuntimeError, match="JSH-XFL001"):
        _typescript_to_javascript(source, declarations=CANONICAL_HOOKS_API_DECLARATIONS)


def test_xfl_policy_diagnostic_precedes_unrelated_typescript_error(tmp_path: Path):
    source = tmp_path / "precedence.hook.ts"
    source.write_text(
        "export const hookConfig=defineHookConfig({xflArithmetic:1});"
        "const unrelated:string=123;"
        "export function main():never{return accept();}"
    )

    with pytest.raises(RuntimeError, match="JSH-XFL001") as raised:
        _typescript_to_javascript(source, declarations=CANONICAL_HOOKS_API_DECLARATIONS)
    assert "not assignable to type 'string'" not in str(raised.value)


def test_hook_source_cannot_augment_xfl_decimal_to_evade_exact_v1(tmp_path: Path):
    source = tmp_path / "augmentation.hook.ts"
    source.write_text(
        "export {};declare global{interface XFLDecimal{"
        "invert():XFLDecimal;}}"
        "declare const decimal:XFLDecimal;"
        "export function main():never{decimal.invert();return accept();}"
    )

    with pytest.raises(RuntimeError, match="must not augment ambient globals"):
        _typescript_to_javascript(source, declarations=DEFAULT_DECLARATIONS)


def test_checked_javascript_shadow_cannot_mint_hook_config(tmp_path: Path):
    source = tmp_path / "shadowed-config.hook.js"
    source.write_text(
        "const defineHookConfig = value => value;\n"
        "export const hookConfig = defineHookConfig({ xflArithmetic: 1 });\n"
        "export function main() { return accept(); }\n"
    )

    with pytest.raises(RuntimeError, match="JSH-XFL001"):
        _check_javascript(
            source,
            declarations=CANONICAL_HOOKS_API_DECLARATIONS,
        )


@pytest.mark.parametrize("method", _XFL_IMPLEMENTED_METHODS)
def test_xfl_policy_finds_sensitive_broad_member_by_declaration_identity(
    tmp_path: Path,
    method: str,
):
    source = tmp_path / "missing-config.hook.ts"
    source.write_text(
        "declare const decimal: XFLDecimal;\n"
        "export function main(): never { "
        f"decimal.{method}(decimal); return accept(); }}\n"
    )

    with pytest.raises(RuntimeError, match="JSH-XFL001"):
        _typescript_to_javascript(
            source,
            declarations=CANONICAL_HOOKS_API_DECLARATIONS,
        )


@pytest.mark.parametrize("suffix", [".ts", ".js"])
@pytest.mark.parametrize(
    "body_template",
    [
        "decimal.{method}(decimal);",
        "(decimal.{method})(decimal);",
        "decimal.{method}?.(decimal);",
        'decimal["{method}"](decimal);',
        'const member = "{method}" as const; decimal[member](decimal);',
        "const operation = decimal.{method}; operation(decimal);",
        "const {{ {method}: operation }} = decimal; operation(decimal);",
        "(0, decimal.{method})(decimal);",
        "const operation = decimal.{method}.bind(decimal); operation(decimal);",
    ],
)
@pytest.mark.parametrize("method", _XFL_IMPLEMENTED_METHODS)
def test_xfl_policy_covers_ratified_call_shapes(
    tmp_path: Path,
    suffix: str,
    body_template: str,
    method: str,
):
    body = body_template.format(method=method)
    declaration, signature = _xfl_test_declaration(suffix)
    if suffix == ".js":
        body = body.replace(" as const;", ";")
    source = tmp_path / f"sensitive-shape.hook{suffix}"
    source.write_text(
        f"{declaration}\n"
        f"export function main(){signature} {{ {body} return accept(); }}\n"
    )

    compiler = _check_javascript if suffix == ".js" else _typescript_to_javascript
    with pytest.raises(RuntimeError, match="JSH-XFL001"):
        compiler(source, declarations=CANONICAL_HOOKS_API_DECLARATIONS)


@pytest.mark.parametrize("suffix", [".ts", ".js"])
@pytest.mark.parametrize("method", _XFL_IMPLEMENTED_METHODS)
def test_xfl_policy_rejects_unimplemented_profile_with_exact_config(
    tmp_path: Path,
    suffix: str,
    method: str,
):
    source = tmp_path / f"configured-sensitive.hook{suffix}"
    declaration, signature = _xfl_test_declaration(suffix)
    source.write_text(
        declaration + "export const hookConfig = defineHookConfig({ "
        "xflArithmetic: XFLProfile.nearestEvenV1 });\n"
        f"export function main(){signature} {{ "
        f"const next = rollback.onFail(decimal.{method}(decimal), 'operation failed'); "
        "trace('next', next.toString()); return accept(); }\n"
    )

    with pytest.raises(RuntimeError, match="JSH-XFL002") as raised:
        compiler = _check_javascript if suffix == ".js" else _typescript_to_javascript
        compiler(
            source,
            declarations=CANONICAL_HOOKS_API_DECLARATIONS,
        )
    assert f"XFLDecimal.{method}" in str(raised.value)
    assert "nearestEvenV1" in str(raised.value)


@pytest.mark.parametrize("suffix", [".ts", ".js"])
@pytest.mark.parametrize("method", _XFL_IMPLEMENTED_METHODS)
def test_xfl_policy_accepts_implemented_profile_with_exact_config(
    tmp_path: Path,
    suffix: str,
    method: str,
):
    source = tmp_path / f"configured-sensitive.hook{suffix}"
    declaration, signature = _xfl_test_declaration(suffix)
    source.write_text(
        declaration + "export const hookConfig = defineHookConfig({ "
        "xflArithmetic: XFLProfile.xahauFloatV1 });\n"
        f"export function main(){signature} {{ "
        f"const next = rollback.onFail(decimal.{method}(decimal), 'operation failed'); "
        "trace('next', next.toString()); return accept(); }\n"
    )

    compiler = _check_javascript if suffix == ".js" else _typescript_to_javascript
    _, _, profile = compiler(
        source,
        declarations=CANONICAL_HOOKS_API_DECLARATIONS,
    )

    assert profile is XFLArithmeticProfile.XAHAU_FLOAT_V1


@pytest.mark.parametrize("suffix", [".ts", ".js"])
@pytest.mark.parametrize("profile", ["xahauFloatV1", "nearestEvenV1"])
@pytest.mark.parametrize("method", _XFL_IMPLEMENTED_METHODS)
def test_xfl_policy_rejects_module_evaluation_call(
    tmp_path: Path,
    suffix: str,
    profile: str,
    method: str,
):
    source = tmp_path / f"module-sensitive.hook{suffix}"
    declaration, signature = _xfl_test_declaration(suffix)
    source.write_text(
        declaration + "export const hookConfig = defineHookConfig({ "
        f"xflArithmetic: XFLProfile.{profile} }});\n"
        f"const result = decimal.{method}(decimal);\n"
        f"export function main(){signature} {{ return accept(); }}\n"
    )

    with pytest.raises(RuntimeError, match="JSH-XFL002") as raised:
        compiler = _check_javascript if suffix == ".js" else _typescript_to_javascript
        compiler(
            source,
            declarations=CANONICAL_HOOKS_API_DECLARATIONS,
        )
    assert f"XFLDecimal.{method}" in str(raised.value)
    assert "module evaluation" in str(raised.value)


def test_xfl_policy_ignores_unrelated_add_spelling(tmp_path: Path):
    source = tmp_path / "unrelated-add.hook.ts"
    source.write_text(
        "const value = { add(): number { return 1; } };\n"
        "export function main(): never { "
        "trace('value', value.add()); return accept(); }\n"
    )

    _, _, profile = _typescript_to_javascript(
        source,
        declarations=CANONICAL_HOOKS_API_DECLARATIONS,
    )

    assert profile is XFLArithmeticProfile.NONE


@pytest.mark.parametrize(
    "declarations,expression",
    [
        (
            "declare const value: UInt8;",
            "rollback.onFail(value.add(1), 'add failed');",
        ),
        ("declare const decimal: XFLDecimal;", "void decimal.sign();"),
        (
            "const value = { add(): number { return 1; } };",
            "void value.add();",
        ),
    ],
)
def test_xfl_policy_negative_controls_do_not_require_config(
    tmp_path: Path,
    declarations: str,
    expression: str,
):
    source = tmp_path / "profile-independent.hook.ts"
    source.write_text(
        f"{declarations}\n"
        f"export function main(): never {{ {expression} return accept(); }}\n"
    )

    _, _, profile = _typescript_to_javascript(
        source,
        declarations=CANONICAL_HOOKS_API_DECLARATIONS,
    )

    assert profile is XFLArithmeticProfile.NONE


@pytest.mark.parametrize("suffix", [".ts", ".js"])
@pytest.mark.parametrize("method", _XFL_IMPLEMENTED_METHODS)
def test_default_v1_arithmetic_attempt_requires_profile(
    tmp_path: Path,
    suffix: str,
    method: str,
):
    source = tmp_path / f"undeclared-{method}.hook{suffix}"
    declaration, signature = _xfl_test_declaration(suffix)
    source.write_text(
        declaration + f"export function main(){signature} {{ "
        f"decimal.{method}(decimal); return accept(); }}\n"
    )

    compiler = _check_javascript if suffix == ".js" else _typescript_to_javascript
    with pytest.raises(RuntimeError, match="JSH-XFL001"):
        compiler(source, declarations=DEFAULT_DECLARATIONS)


def test_result_moot_is_restricted_to_void_results(tmp_path: Path):
    source = tmp_path / "moot-value.hook.ts"
    source.write_text(
        "export function main(): never { "
        "state.get('meaningful').moot(); "
        "return accept(); }"
    )

    with pytest.raises(RuntimeError, match="Property 'moot' does not exist"):
        compile_hook(source)


def test_rollback_require_present_strips_nullish_result_values(tmp_path: Path):
    source = tmp_path / "require-nullish.hook.ts"
    source.write_text(
        """
        declare const nullable: Result<string | null | undefined, HostError>;
        declare const zeroResult: Result<0, HostError>;

        export function main(): never {
          const present: string = rollback.requirePresent(nullable, "required");
          const zero: 0 = rollback.requirePresent(zeroResult, "required");
          trace(present, zero);
          return accept();
        }
        """
    )

    assert compile_hook(source).bytecode


def test_accept_require_assertion_narrows_instanceof_condition(tmp_path: Path):
    source = tmp_path / "accept-require-narrowing.hook.ts"
    source.write_text(
        """
        export function main(): never {
          const transaction = otxn.object();
          accept.require(
            transaction instanceof Payment,
            "not an incoming Payment",
          );
          const payment: Payment = transaction;
          const amount: Amount = transaction.Amount;
          void payment.Destination;
          void amount;
          return accept();
        }
        """
    )

    # Red control: this exact Hook does not type-check if require merely
    # returns void. The assertion signature, not instanceof syntax alone,
    # carries the narrowing beyond the helper call.
    declarations = tmp_path / "without-accept-require-assertion.d.ts"
    projected = XAHAU_V1_HOOKS_API_DECLARATIONS.read_text()
    assert "): asserts condition;" in projected
    declarations.write_text(projected.replace(
        "): asserts condition;", "): void;", 1
    ))
    with pytest.raises(RuntimeError, match="Type 'Transaction' is missing"):
        _typescript_to_javascript(source, declarations=declarations)

    assert compile_hook(source).bytecode


def test_legacy_rich_roots_are_not_public_constructors(tmp_path: Path):
    source = tmp_path / "factory-roots.hook.ts"
    source.write_text(
        """
        export function main(): never {
          // @ts-expect-error STBlob is a factory object, not a constructor.
          const blob = new STBlob();
          // @ts-expect-error Hash256 is not a base class.
          class DerivedHash extends Hash256 {}
          // @ts-expect-error AccountID is not a public constructor.
          const account = new AccountID();
          void blob;
          void DerivedHash;
          void account;
          return accept();
        }
        """
    )

    assert compile_hook(source).bytecode


def test_selected_provider_minted_types_have_narrowing_runtime_nouns(
    tmp_path: Path,
):
    source = tmp_path / "provider-minted-types.hook.ts"
    source.write_text(
        """
        declare const decimal: XFLDecimal;
        declare const amount: Amount;
        declare const pathSet: PathSet;
        declare const candidate: unknown;

        export function main(): never {
          const negative: boolean = decimal.isNegative();
          const zero: boolean = decimal.isZero();
          const amountKind: "native" | "iou" | "mpt" = amount.kind;
          const firstPath = pathSet.at(0);

          const decimalType: RuntimeType<XFLDecimal> = XFLDecimal;
          // @ts-expect-error XFLDecimal has no public prototype value.
          const decimalPrototype = XFLDecimal.prototype;
          // @ts-expect-error XFLDecimal has no public constructor value.
          const constructedDecimal = new XFLDecimal();
          const decimalInstance: boolean = decimal instanceof XFLDecimal;

          const amountType: RuntimeType<Amount> = Amount;
          // @ts-expect-error Amount has no public prototype value.
          const amountPrototype = Amount.prototype;
          // @ts-expect-error Amount has no public constructor value.
          const constructedAmount = new Amount();
          const amountInstance: boolean = amount instanceof Amount;

          const pathSetType: RuntimeType<PathSet> = PathSet;
          // @ts-expect-error PathSet has no public prototype value.
          const pathSetPrototype = PathSet.prototype;
          // @ts-expect-error PathSet has no public constructor value.
          const constructedPathSet = new PathSet();
          const pathSetInstance: boolean = pathSet instanceof PathSet;

          if (candidate instanceof Amount) {
            const narrowed: Amount = candidate;
            void narrowed.kind;
          }

          void decimalType;
          void amountType;
          void pathSetType;

          trace(
            "provider minted",
            [
              negative,
              zero,
              amountKind,
              firstPath,
                  decimalPrototype,
                  constructedDecimal,
                  decimalInstance,
                  amountPrototype,
                  constructedAmount,
                  amountInstance,
                  pathSetPrototype,
              constructedPathSet,
              pathSetInstance,
            ],
          );
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
        f"export function main(): never {{ {statement} return accept(); }}"
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
    source.write_text(f"export function main(): never {{ {body} return accept(); }}")

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
    source.write_text(f"export function main(): never {{ {body} return accept(); }}")

    assert compile_hook(source).bytecode


@pytest.mark.parametrize(
    "body",
    [
        ("state.get('key').okOrHandle(error => rollback('read failed', error.code));"),
        (
            "function abortWith(error: HostError): never { "
            "rollback('read failed', error.code); } "
            "state.get('key').okOrHandle(abortWith);"
        ),
    ],
)
def test_result_dataflow_rejects_nonreturning_ok_or_handle_handlers(
    tmp_path: Path,
    body: str,
):
    source = tmp_path / "nonreturning-ok-or-handle.hook.ts"
    source.write_text(f"export function main(): never {{ {body} return accept(); }}")

    with pytest.raises(
        RuntimeError,
        match="okOrHandle requires a returning fallback handler",
    ):
        compile_hook(source)


def test_result_dataflow_accepts_conditionally_terminal_fallback_handler(
    tmp_path: Path,
):
    source = tmp_path / "returning-ok-or-handle.hook.ts"
    source.write_text(
        "export function main(): never { "
        "const value = state.get('key').okOrHandle(error => { "
        "if (error.code === -1) rollback('fatal read', error.code); "
        "return undefined; }); void value; return accept(); }"
    )

    assert compile_hook(source).bytecode


def test_raw_javascript_uses_the_same_result_dataflow_gate(tmp_path: Path):
    source = tmp_path / "discarded-result.hook.js"
    source.write_text("export function main() { state.get('key'); return accept(); }")

    with pytest.raises(RuntimeError, match="six legal exits"):
        compile_hook(source)


@pytest.mark.parametrize(
    "body",
    [
        "class Hidden { value = state.get('key'); } void new Hidden();",
        "class Hidden { static { state.get('key'); } } void Hidden;",
        "class Hidden { static value = state.get('key'); } void Hidden;",
        (
            "function hidden(value = (state.get('key'), 'fallback')) "
            "{ void value; } hidden();"
        ),
    ],
)
def test_result_dataflow_rejects_class_and_parameter_initializer_holes(
    tmp_path: Path,
    body: str,
):
    source = tmp_path / "initializer-result.hook.ts"
    source.write_text(f"export function main(): never {{ {body} return accept(); }}")

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
    assert compiled.profile is XFLArithmeticProfile.NONE
    assert packaged.artifact
    assert packaged.profile is XFLArithmeticProfile.NONE


def test_typescript_hook_compiles_to_provider_bytecode(tmp_path: Path):
    source = tmp_path / "accept.hook.ts"
    source.write_text(
        """
        export function main(): never {
          throw new Error("entered main");
        }
        export function callback(info: CallbackInfo): never {
          throw new Error(`entered callback:${info.failureBitSet}:${info.rawFlags}`);
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
          throw new Error(`packaged:${info.failureBitSet}:${info.rawFlags}`);
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
    assert parsed.profile is XFLArithmeticProfile.NONE
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


@pytest.mark.parametrize(
    ("member", "expected", "wire"),
    [
        ("xahauFloatV1", XFLArithmeticProfile.XAHAU_FLOAT_V1, b"\x00\x01"),
        ("nearestEvenV1", XFLArithmeticProfile.NEAREST_EVEN_V1, b"\x00\x02"),
    ],
)
def test_packaging_joins_source_provider_and_v1_header_profile(
    tmp_path: Path,
    member: str,
    expected: XFLArithmeticProfile,
    wire: bytes,
):
    source = tmp_path / "profiled.hook.ts"
    source.write_text(
        "export const hookConfig=defineHookConfig({xflArithmetic:"
        f"XFLProfile.{member}}});"
        "export function main():never{return accept();}"
    )

    packaged = package_hook(
        source,
        hook_api_version=1,
        bytecode_abi_id=bytes(range(32)),
        runtime_profile_id=bytes(range(32, 64)),
    )
    parsed = parse_hook_artifact(packaged.artifact)

    assert packaged.profile is expected
    assert parsed.profile is expected
    assert packaged.artifact[10:12] == wire


def test_compiler_rejects_provider_profile_disagreement(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
):
    source = tmp_path / "none.hook.ts"
    source.write_text("export function main():never{return accept();}")
    monkeypatch.setattr(
        WasmHost,
        "validate_hook_bytecode",
        lambda _self, _bytecode: HookModuleValidation(
            valid=True,
            profile=XFLArithmeticProfile.XAHAU_FLOAT_V1,
        ),
    )

    with pytest.raises(RuntimeError, match="profile disagreement"):
        compile_hook(source)


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


def test_compiler_rejects_numeric_callback_parameter(tmp_path: Path):
    source = tmp_path / "old-callback.hook.ts"
    source.write_text(
        """
        export function main(): never {
          accept("ok");
        }
        export function callback(_reserved: number): never {
          accept("cb", _reserved);
        }
        """
    )

    with pytest.raises(RuntimeError, match="CallbackInfo"):
        compile_hook(source)


def test_compiler_accepts_callback_info_parameter(tmp_path: Path):
    source = tmp_path / "callback-info.hook.ts"
    source.write_text(
        """
        export function main(): never {
          accept("ok");
        }
        export function callback(info: CallbackInfo): never {
          if (info.failureBitSet) rollback("emitted failed", info.rawFlags);
          accept("cb");
        }
        """
    )

    assert compile_hook(source).bytecode


def test_compiler_driver_creates_one_program(tmp_path: Path):
    from jshookz.hook_compiler import _frontend_driver_js, _typescript_library

    source = tmp_path / "one-program.hook.ts"
    source.write_text("export function main(): never { return accept(); }")
    config = tmp_path / "tsconfig.json"
    config.write_text(
        __import__("json").dumps(
            {
                "compilerOptions": {
                    "lib": ["ES2023"],
                    "module": "ESNext",
                    "noEmit": True,
                    "strict": True,
                    "target": "ES2023",
                },
                "files": [str(DEFAULT_DECLARATIONS), str(source)],
            }
        )
    )
    env = os.environ.copy()
    env["JSHOOKZ_COUNT_PROGRAM"] = "1"
    completed = subprocess.run(
        [
            "node",
            str(_frontend_driver_js(None)),
            str(_typescript_library(None)),
            str(config),
            str(source),
            str(DEFAULT_DECLARATIONS),
        ],
        capture_output=True,
        text=True,
        env=env,
        check=False,
    )
    assert completed.returncode == 0, completed.stderr
    assert "createProgram=1" in completed.stderr
    assert "kind=ok" in completed.stderr
    assert "xflProfile=none" in completed.stderr


def test_compiler_frontend_typechecks():
    from jshookz.hook_compiler import (
        _FRONTEND_TSCONFIG,
        _typescript_executable,
    )

    completed = subprocess.run(
        [_typescript_executable(None), "--noEmit", "-p", str(_FRONTEND_TSCONFIG)],
        capture_output=True,
        text=True,
        check=False,
    )
    assert completed.returncode == 0, completed.stdout + completed.stderr


def test_frontend_emit_publishes_complete_entry_policy():
    from jshookz.hook_compiler import _frontend_driver_js

    driver = _frontend_driver_js(None)
    policy = driver.parent / "entry_policy.js"
    assert policy.is_file()
    completed = subprocess.run(
        [
            "node",
            "-e",
            "const m = require(process.argv[1]);"
            " if (typeof m.checkHookImports !== 'function')"
            " process.exit(2)",
            str(policy),
        ],
        check=False,
    )
    assert completed.returncode == 0


def test_xfl_policy_rejects_unsupported_generated_ledger_schema(tmp_path: Path):
    from jshookz.hook_compiler import _frontend_driver_js

    emitted = _frontend_driver_js(None).parent
    policy = tmp_path / "xfl_profile_policy.js"
    ledger = tmp_path / "xfl_profile_ledger.js"
    policy.write_text((emitted / policy.name).read_text())
    ledger.write_text(
        (emitted / ledger.name)
        .read_text()
        .replace("jshookz.xfl-profile-ledger.v1", "jshookz.xfl-profile-ledger.v0")
    )
    script = (
        "const p=require(process.argv[1]);"
        "try{p.checkXFLProfilePolicy({},{},{},{});process.exit(2)}"
        "catch(error){process.stderr.write(String(error));}"
    )

    completed = subprocess.run(
        ["node", "-e", script, str(policy)],
        capture_output=True,
        text=True,
        check=False,
    )

    assert completed.returncode == 0
    assert "unsupported profile ledger schema" in completed.stderr


@pytest.mark.parametrize("method", _XFL_IMPLEMENTED_METHODS)
def test_xfl_policy_turns_red_when_generated_implementation_is_removed(
    tmp_path: Path,
    method: str,
):
    from jshookz.hook_compiler import _frontend_driver_js, _typescript_library

    emitted = _frontend_driver_js(None).parent
    policy = tmp_path / "xfl_profile_policy.js"
    ledger = tmp_path / "xfl_profile_ledger.js"
    policy.write_text((emitted / policy.name).read_text())
    original_ledger = (emitted / ledger.name).read_text()
    implementation_members = [
        f"XFLDecimal.{implemented}" for implemented in _XFL_IMPLEMENTED_METHODS
    ]
    original_line = f'  "xahauFloatV1": {json.dumps(implementation_members)},'
    remaining_members = [
        member for member in implementation_members if member != f"XFLDecimal.{method}"
    ]
    mutated_line = f'  "xahauFloatV1": {json.dumps(remaining_members)},'
    mutated_ledger = original_ledger.replace(
        original_line,
        mutated_line,
    )
    assert mutated_ledger != original_ledger
    ledger.write_text(mutated_ledger)

    source = tmp_path / "profiled.ts"
    source.write_text(
        "declare const decimal:XFLDecimal;"
        "export const hookConfig=defineHookConfig({xflArithmetic:"
        "XFLProfile.xahauFloatV1});"
        "export function main():never{"
        f"rollback.onFail(decimal.{method}(decimal),'operation failed');"
        "return accept();}"
    )
    config = tmp_path / "tsconfig.json"
    config.write_text(
        json.dumps(
            {
                "compilerOptions": {
                    "lib": ["ES2023"],
                    "module": "ESNext",
                    "noEmit": True,
                    "strict": True,
                    "target": "ES2023",
                },
                "files": [
                    str(CANONICAL_HOOKS_API_DECLARATIONS),
                    str(source),
                ],
            }
        )
    )
    script = """
const ts = require(process.argv[1]);
const policy = require(process.argv[2]);
const configPath = process.argv[3];
const sourcePath = process.argv[4];
const declarationPath = process.argv[5];
const config = ts.readConfigFile(configPath, ts.sys.readFile);
const parsed = ts.parseJsonConfigFileContent(
  config.config, ts.sys, require('path').dirname(configPath));
const program = ts.createProgram(parsed.fileNames, parsed.options);
const result = policy.checkXFLProfilePolicy(
  ts,
  program,
  program.getSourceFile(sourcePath),
  program.getSourceFile(declarationPath),
);
process.stdout.write(JSON.stringify(result));
"""
    completed = subprocess.run(
        [
            "node",
            "-e",
            script,
            str(_typescript_library(None)),
            str(policy),
            str(config),
            str(source),
            str(CANONICAL_HOOKS_API_DECLARATIONS),
        ],
        capture_output=True,
        text=True,
        check=False,
    )

    assert completed.returncode == 0, completed.stderr
    result = json.loads(completed.stdout)
    assert result["profile"] == "xahauFloatV1"
    assert len(result["diagnostics"]) == 1
    assert "JSH-XFL002" in result["diagnostics"][0]
    assert f"XFLDecimal.{method}" in result["diagnostics"][0]
    assert "xahauFloatV1" in result["diagnostics"][0]


def test_xfl_policy_walks_unimported_executable_helper_in_synthetic_program(
    tmp_path: Path,
):
    from jshookz.hook_compiler import _frontend_driver_js, _typescript_library

    entry = tmp_path / "entry.ts"
    entry.write_text("export function main(): never { return accept(); }\n")
    helper = tmp_path / "helper.ts"
    helper.write_text(
        "declare const decimal: XFLDecimal;\n"
        "export function helper(): void { decimal.add(decimal); }\n"
    )
    config = tmp_path / "tsconfig.json"
    config.write_text(
        json.dumps(
            {
                "compilerOptions": {
                    "lib": ["ES2023"],
                    "module": "ESNext",
                    "noEmit": True,
                    "strict": True,
                    "target": "ES2023",
                },
                "files": [
                    str(CANONICAL_HOOKS_API_DECLARATIONS),
                    str(entry),
                    str(helper),
                ],
            }
        )
    )
    driver = _frontend_driver_js(None)
    policy = driver.parent / "xfl_profile_policy.js"
    script = """
const ts = require(process.argv[1]);
const policy = require(process.argv[2]);
const configPath = process.argv[3];
const entryPath = process.argv[4];
const declarationPath = process.argv[5];
const config = ts.readConfigFile(configPath, ts.sys.readFile);
const parsed = ts.parseJsonConfigFileContent(
  config.config, ts.sys, require('path').dirname(configPath));
const program = ts.createProgram(parsed.fileNames, parsed.options);
const result = policy.checkXFLProfilePolicy(
  ts,
  program,
  program.getSourceFile(entryPath),
  program.getSourceFile(declarationPath),
);
process.stdout.write(JSON.stringify(result));
"""
    completed = subprocess.run(
        [
            "node",
            "-e",
            script,
            str(_typescript_library(None)),
            str(policy),
            str(config),
            str(entry),
            str(CANONICAL_HOOKS_API_DECLARATIONS),
        ],
        capture_output=True,
        text=True,
        check=False,
    )

    assert completed.returncode == 0, completed.stderr
    result = json.loads(completed.stdout)
    assert result["profile"] == "none"
    assert len(result["diagnostics"]) == 1
    assert str(helper) in result["diagnostics"][0]
    assert "JSH-XFL001" in result["diagnostics"][0]


def test_frontend_emit_survives_parallel_workers():
    import shutil
    import tempfile
    from concurrent.futures import ThreadPoolExecutor

    from jshookz.hook_compiler import _frontend_driver_js

    cache = Path(tempfile.gettempdir()) / "jshookz-frontend"
    if cache.is_dir():
        shutil.rmtree(cache)

    def one(_: int) -> Path:
        driver = _frontend_driver_js(None)
        policy = driver.parent / "entry_policy.js"
        assert driver.is_file() and policy.is_file()
        assert "exports.checkHookImports" in policy.read_text()
        return driver

    with ThreadPoolExecutor(max_workers=8) as pool:
        paths = list(pool.map(one, range(8)))
    assert len(set(paths)) == 1


def test_compiler_bundles_static_relative_helper_import(tmp_path: Path):
    helper = tmp_path / "helper.ts"
    helper.write_text("export function helper(): number { return 1; }\n")
    source = tmp_path / "imports.hook.ts"
    source.write_text(
        """
        import { helper } from "./helper";
        export function main(): never {
          throw new Error(String(helper()));
        }
        """
    )
    compiled = compile_hook(source)

    assert "from \"./helper\"" not in compiled.javascript
    host = WasmHost(wasm_path=XAHAU_HOOK_PROVIDER_WASM)
    host.init()
    try:
        evaluated = host.run_hook_bytecode(compiled.bytecode)
    finally:
        host.destroy()
    assert evaluated.error == "Error: 1"


def test_compiler_rejects_rest_number_callback(tmp_path: Path):
    source = tmp_path / "rest-callback.hook.ts"
    source.write_text(
        """
        export function main(): never { accept("ok"); }
        export function callback(...reserved: number[]): never {
          accept("cb", reserved[0]);
        }
        """
    )
    with pytest.raises(RuntimeError, match="CallbackInfo"):
        compile_hook(source)


def test_compiler_rejects_function_typed_callback(tmp_path: Path):
    source = tmp_path / "function-callback.hook.ts"
    source.write_text(
        """
        export function main(): never { accept("ok"); }
        export const callback: Function = (n: number) => accept("cb", n);
        """
    )
    with pytest.raises(RuntimeError, match="CallbackInfo"):
        compile_hook(source)


def test_compiler_emits_noncallable_entries_when_flagged(tmp_path: Path):
    source = tmp_path / "noncallable.hook.ts"
    source.write_text(
        """
        // @jshookz-allow-malformed
        export const main = 1;
        export const callback = 2;
        """
    )
    compiled = compile_hook(source)
    assert compiled.bytecode


def test_compiler_emits_malformed_bytecode_when_flagged(tmp_path: Path):
    source = tmp_path / "missing-main.hook.ts"
    source.write_text(
        """
        export function callback(info: CallbackInfo): never {
          void info;
          accept("callback only", 1);
        }
        """
    )

    with pytest.raises(RuntimeError, match="no exported main entry point"):
        compile_hook(source)

    compiled = compile_hook(source, allow_malformed=True)
    assert compiled.bytecode


def test_allow_malformed_directive_ignores_string_and_block_comment(
    tmp_path: Path,
):
    source = tmp_path / "false-hatch.hook.ts"
    source.write_text(
        """
        /* // @jshookz-allow-malformed */
        export function main(): never {
          void "// @jshookz-allow-malformed";
          accept("ok");
        }
        void ledger.sequence;
        """
    )
    with pytest.raises(RuntimeError, match="not deployable"):
        compile_hook(source)


def test_compiler_emits_malformed_bytecode_for_source_directive(tmp_path: Path):
    source = tmp_path / "host-init.hook.ts"
    source.write_text(
        """
        // @jshookz-allow-malformed
        export function main(_reserved: number): never {
          accept("entry", _reserved);
        }
        void ledger.sequence;
        """
    )

    compiled = compile_hook(source)
    assert compiled.bytecode


def test_packager_still_rejects_malformed_directive(tmp_path: Path):
    source = tmp_path / "packaged-malformed.hook.ts"
    source.write_text(
        """
        // @jshookz-allow-malformed
        export function callback(info: CallbackInfo): never {
          void info;
          accept("callback only", 1);
        }
        """
    )

    with pytest.raises(RuntimeError, match="not deployable"):
        package_hook(
            source,
            hook_api_version=1,
            bytecode_abi_id=bytes(range(32)),
            runtime_profile_id=bytes(range(32, 64)),
        )


def test_provider_dispatches_callback_export(tmp_path: Path):
    source = tmp_path / "callback.hook.ts"
    source.write_text(
        """
        export function main(): never {
          throw new Error("entered main");
        }
        export function callback(info: CallbackInfo): never {
          throw new Error(`entered callback:${info.failureBitSet}:${info.rawFlags}`);
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
