"""Sealed-provider and packaged-Hook gates for xahauFloatV1 add/subtract."""

from __future__ import annotations

import importlib.util
import json
from dataclasses import dataclass, field
from pathlib import Path
from types import ModuleType

import pytest

from jshookz.hook_artifact import parse_hook_artifact
from jshookz.hook_compiler import package_hook
from jshookz.host import ContractResult, WasmHost
from jshookz.paths import (
    XAHAU_HOOK_PROVIDER_WASM,
    XAHAU_RUNTIME_PROFILE_LOCK,
)
from jshookz.runtime_profile import (
    profile_execution_limits,
    verify_runtime_profile_lock,
)
from jshookz.xfl_profile import XFLArithmeticProfile


ROOT = Path(__file__).resolve().parents[3]
ORACLE_PATH = ROOT / "cpp/xahau-types/tests/xahau_float_v1_add_subtract_oracle.json"
ORACLE_READER_PATH = ROOT / "scripts/xfl_arithmetic_oracle.py"
_CURRENCY = "0000000000000000000000005553440000000000"
_ISSUER = "B5F762798A53D543A014CAF8B297CFF8F2F937E8"
_SUCCESS_MESSAGE = "xfl oracle"
_WASM_MALLOC_OVERHEAD = 16
_TIGHT_MAX_ALIGNMENT_FUEL_CEILING = 228_900
_PACKAGED_ORACLE_BATCH_SIZE = 16


def _load_oracle_reader() -> ModuleType:
    spec = importlib.util.spec_from_file_location(
        "xfl_arithmetic_execution_oracle", ORACLE_READER_PATH
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


ORACLE = _load_oracle_reader().load_fixture(ORACLE_PATH)


def _typed_integer(leaf: dict[str, str]) -> int:
    return int(leaf["val"])


def _amount_object_hex(raw: int) -> str:
    # XFL and IOU STAmount share the decimal payload. STAmount additionally
    # sets its non-native bit and carries one currency and issuer.
    amount_word = (raw | (1 << 63)).to_bytes(8, "big").hex().upper()
    return f"61{amount_word}{_CURRENCY}{_ISSUER}"


def _decimal_expression(raw: int) -> str:
    return (
        "rollback.requirePresent(util.decodeObject(STBlob.fromHex("
        f'"{_amount_object_hex(raw)}"))'
        '.get(Field.Amount),"oracle amount").toXFL()'
    )


def _case_statements(cases: list[dict[str, object]]) -> str:
    statements: list[str] = []
    for index, case in enumerate(cases):
        case_id = str(case["id"])
        operation = str(case["operation"])
        left = _typed_integer(case["a"])  # type: ignore[arg-type]
        right = _typed_integer(case["b"])  # type: ignore[arg-type]
        result = case["result"]
        assert isinstance(result, dict)
        statements.append(
            "{"
            f"const result={_decimal_expression(left)}.{operation}("
            f"{_decimal_expression(right)});"
        )
        if result["ok"] is True:
            expected = _typed_integer(result["value"])  # type: ignore[arg-type]
            statements.append(
                f'if(!result.ok)rollback("{case_id}:unexpected-failure");'
                "if(!(result.value instanceof XFLDecimal))"
                f'rollback("{case_id}:nominal-value");'
                "if(!Object.isFrozen(result.value))"
                f'rollback("{case_id}:mutable-value");'
                f"if(!result.value.equals({_decimal_expression(expected)}))"
                f'rollback("{case_id}:wrong-word");'
            )
        else:
            statements.append(
                f'if(result.ok){{rollback("{case_id}:unexpected-success");}}else{{'
                'if(result.error.domain!=="xfl"||'
                'result.error.issue!=="overflow")'
                f'rollback("{case_id}:wrong-error");'
                "if(Object.getPrototypeOf(result.error)!==null||"
                "Object.isExtensible(result.error))"
                f'rollback("{case_id}:error-shape");}}'
            )
        statements.append("}")
    return "".join(statements)


def _profiled_source(body: str, *, callback: bool = False) -> str:
    entry = "callback(info:CallbackInfo)" if callback else "main()"
    return (
        "export const hookConfig=defineHookConfig({"
        "xflArithmetic:XFLProfile.xahauFloatV1});"
        f"export function {entry}:never{{{body}}}"
        + (
            'export function main():never{return accept("xfl oracle",0);}'
            if callback
            else ""
        )
    )


def _profile_identities() -> tuple[bytes, bytes]:
    lock = json.loads(XAHAU_RUNTIME_PROFILE_LOCK.read_text())
    return bytes.fromhex(lock["bytecode_abi_id"]), bytes.fromhex(
        lock["runtime_profile_id"]
    )


def _package(tmp_path: Path, source_text: str, name: str = "xfl.hook.ts"):
    source = tmp_path / name
    source.write_text(source_text)
    bytecode_abi_id, runtime_profile_id = _profile_identities()
    return package_hook(
        source,
        hook_api_version=1,
        bytecode_abi_id=bytecode_abi_id,
        runtime_profile_id=runtime_profile_id,
        profile_path=XAHAU_RUNTIME_PROFILE_LOCK,
        wasm_path=XAHAU_HOOK_PROVIDER_WASM,
    )


@dataclass
class _RecordingHost:
    calls: list[tuple[object, ...]] = field(default_factory=list)
    host: WasmHost | None = field(default=None, repr=False)

    def _message(self, pointer: int, length: int) -> str:
        assert self.host is not None
        return self.host._read_wasm_string(pointer, length)

    def accept(self, pointer: int, length: int, code: int) -> int:
        message = self._message(pointer, length)
        self.calls.append(("accept", message, code))
        return code

    def rollback(self, pointer: int, length: int, code: int) -> int:
        message = self._message(pointer, length)
        self.calls.append(("rollback", message, code))
        return code or -1

    def trace(
        self,
        label_pointer: int,
        label_length: int,
        value_pointer: int,
        value_length: int,
        as_hex: int,
    ) -> int:
        self.calls.append(
            (
                "trace",
                self._message(label_pointer, label_length),
                self._message(value_pointer, value_length),
                as_hex,
            )
        )
        return 0


def _execute(bytecode: bytes, *, export: str = "hook", reserved: int = 0):
    handler = _RecordingHost()
    host = WasmHost.profiled(handler=handler)
    handler.host = host
    host.init()
    try:
        result = host.run_hook_bytecode(
            bytecode,
            export=export,
            reserved=reserved,
        )
    finally:
        host.destroy()
    return result, handler


def _execute_on_wasm(
    bytecode: bytes,
    wasm_path: Path,
    *,
    export: str = "hook",
    reserved: int = 0,
):
    limits = profile_execution_limits(
        verify_runtime_profile_lock(
            XAHAU_RUNTIME_PROFILE_LOCK,
            XAHAU_HOOK_PROVIDER_WASM,
        )
    )
    handler = _RecordingHost()
    host = WasmHost(
        handler=handler,
        wasm_path=wasm_path,
        execution_limits=limits,
    )
    handler.host = host
    host.init()
    try:
        result = host.run_hook_bytecode(
            bytecode,
            export=export,
            reserved=reserved,
        )
    finally:
        host.destroy()
    return result, handler


def _assert_terminal_only(result: ContractResult, handler: _RecordingHost) -> None:
    assert result.ok, result.error
    assert handler.calls == [("accept", _SUCCESS_MESSAGE, 0)]
    assert result.host_work_used == 1 + len(_SUCCESS_MESSAGE)


def _assert_fuel_ceiling(result: ContractResult, fuel_ceiling: int) -> None:
    assert 10_000 <= fuel_ceiling - result.gas_used


def test_all_oracle_rows_execute_through_packaged_sealed_provider(tmp_path: Path):
    cases = ORACLE["cases"]
    covered_ids: list[str] = []
    for batch_index, offset in enumerate(
        range(0, len(cases), _PACKAGED_ORACLE_BATCH_SIZE)
    ):
        batch = cases[offset : offset + _PACKAGED_ORACLE_BATCH_SIZE]
        covered_ids.extend(str(case["id"]) for case in batch)
        source = _profiled_source(
            _case_statements(batch) + f'return accept("{_SUCCESS_MESSAGE}",0);'
        )
        packaged = _package(tmp_path, source, f"oracle-{batch_index}.hook.ts")
        parsed = parse_hook_artifact(packaged.artifact)

        first, first_handler = _execute(parsed.payload)
        second, second_handler = _execute(parsed.payload)

        assert packaged.profile is XFLArithmeticProfile.XAHAU_FLOAT_V1
        assert parsed.profile is XFLArithmeticProfile.XAHAU_FLOAT_V1
        assert first == second
        _assert_terminal_only(first, first_handler)
        assert second_handler.calls == first_handler.calls
        assert 0 < first.gas_used < 50_000_000

    assert covered_ids == [str(case["id"]) for case in cases]


def test_arithmetic_adds_no_host_call_or_host_work(tmp_path: Path):
    representative = [
        case
        for case in ORACLE["cases"]
        if case["id"]
        in {
            "add.zero-positive",
            "add.same-exponent-positive",
            "add.align-delta-16-six-tenths",
            "add.align-delta-176",
            "add.carry-positive",
            "add.overflow-positive",
            "subtract.self",
        }
    ]
    arithmetic = _package(
        tmp_path,
        _profiled_source(
            _case_statements(representative) + f'return accept("{_SUCCESS_MESSAGE}",0);'
        ),
        "arithmetic.hook.ts",
    )
    terminal = _package(
        tmp_path,
        _profiled_source(f'return accept("{_SUCCESS_MESSAGE}",0);'),
        "terminal.hook.ts",
    )

    arithmetic_result, arithmetic_handler = _execute(arithmetic.bytecode)
    terminal_result, terminal_handler = _execute(terminal.bytecode)

    expected_calls = [("accept", _SUCCESS_MESSAGE, 0)]
    assert arithmetic_result.ok and terminal_result.ok
    assert arithmetic_handler.calls == expected_calls
    assert terminal_handler.calls == expected_calls
    assert arithmetic_result.host_work_used == terminal_result.host_work_used
    assert arithmetic_result.host_work_used == 1 + len(_SUCCESS_MESSAGE)


def test_packaged_metamorphic_laws_supplement_the_oracle(tmp_path: Path):
    case = next(
        case
        for case in ORACLE["cases"]
        if case["id"] == "add.align-delta-16-six-tenths"
    )
    left = _decimal_expression(_typed_integer(case["a"]))  # type: ignore[arg-type]
    right = _decimal_expression(_typed_integer(case["b"]))  # type: ignore[arg-type]
    zero = _decimal_expression(0)
    body = (
        f"const left={left};const right={right};const zero={zero};"
        "const forward=rollback.onFail(left.add(right),'forward');"
        "const reverse=rollback.onFail(right.add(left),'reverse');"
        "if(!forward.equals(reverse))rollback('commutativity');"
        "const identity=rollback.onFail(left.add(zero),'identity');"
        "if(!identity.equals(left))rollback('zero identity');"
        "const self=rollback.onFail(left.subtract(left),'self');"
        "if(!self.isZero())rollback('self cancellation');"
        "const difference=rollback.onFail(left.subtract(right),'subtract');"
        "const addNegate=rollback.onFail(left.add(right.negate()),'add negate');"
        "if(!difference.equals(addNegate))rollback('subtract/add-negate');"
        f'return accept("{_SUCCESS_MESSAGE}",0);'
    )
    packaged = _package(
        tmp_path,
        _profiled_source(body),
        "metamorphic.hook.ts",
    )

    result, handler = _execute(packaged.bytecode)

    _assert_terminal_only(result, handler)


@pytest.mark.parametrize(
    ("case_id", "fuel_ceiling"),
    [
        ("add.zero-positive", 250_000),
        ("add.same-exponent-positive", 255_000),
        ("add.align-delta-176", 250_000),
        ("add.exact-cancellation", 255_000),
        ("add.carry-positive", 255_000),
        ("add.overflow-positive", 235_000),
        ("subtract.self", 255_000),
    ],
)
def test_packaged_operation_lanes_have_deterministic_bounded_fuel(
    tmp_path: Path,
    case_id: str,
    fuel_ceiling: int,
):
    case = next(case for case in ORACLE["cases"] if case["id"] == case_id)
    packaged = _package(
        tmp_path,
        _profiled_source(
            _case_statements([case]) + f'return accept("{_SUCCESS_MESSAGE}",0);'
        ),
        f"{case_id.replace('.', '-')}.hook.ts",
    )

    first, first_handler = _execute(packaged.bytecode)
    second, second_handler = _execute(packaged.bytecode)

    _assert_terminal_only(first, first_handler)
    _assert_terminal_only(second, second_handler)
    assert first.gas_used == second.gas_used
    _assert_fuel_ceiling(first, fuel_ceiling)


def test_maximum_alignment_ceiling_rejects_kernel_o_gap_division(
    tmp_path: Path,
    xfl_gap_loop_mutant_wasm: Path,
):
    case = next(case for case in ORACLE["cases"] if case["id"] == "add.align-delta-176")
    packaged = _package(
        tmp_path,
        _profiled_source(
            _case_statements([case]) + f'return accept("{_SUCCESS_MESSAGE}",0);'
        ),
        "maximum-alignment-kernel-mutant-control.hook.ts",
    )

    green_result, green_handler = _execute(packaged.bytecode)
    mutant_result, mutant_handler = _execute_on_wasm(
        packaged.bytecode,
        xfl_gap_loop_mutant_wasm,
    )

    _assert_terminal_only(green_result, green_handler)
    _assert_terminal_only(mutant_result, mutant_handler)
    # Wasmtime fuel charges the 176-step compiled i64 division loop only 409
    # more units than the O(1) kernel. Keep the ordinary 250k lane's 10k
    # operational headroom, and use this exact-identity tight ceiling solely
    # as the permanent complexity red.
    assert 100 <= _TIGHT_MAX_ALIGNMENT_FUEL_CEILING - green_result.gas_used
    assert mutant_result.gas_used > _TIGHT_MAX_ALIGNMENT_FUEL_CEILING


def test_wrong_route_control_turns_host_call_work_and_fuel_gates_red(
    tmp_path: Path,
):
    case = next(
        case for case in ORACLE["cases"] if case["id"] == "add.same-exponent-positive"
    )
    green = _package(
        tmp_path,
        _profiled_source(
            _case_statements([case]) + f'return accept("{_SUCCESS_MESSAGE}",0);'
        ),
        "green-route.hook.ts",
    )
    wrong = _package(
        tmp_path,
        _profiled_source(
            _case_statements([case])
            + 'trace("wrong-route","after arithmetic");'
            + f'return accept("{_SUCCESS_MESSAGE}",0);'
        ),
        "wrong-route.hook.ts",
    )

    green_result, green_handler = _execute(green.bytecode)
    wrong_result, wrong_handler = _execute(wrong.bytecode)

    _assert_terminal_only(green_result, green_handler)
    with pytest.raises(AssertionError):
        _assert_terminal_only(wrong_result, wrong_handler)
    assert wrong_handler.calls == [
        ("trace", "wrong-route", "after arithmetic", 0),
        ("accept", _SUCCESS_MESSAGE, 0),
    ]
    assert wrong_result.host_work_used > green_result.host_work_used
    assert wrong_result.gas_used > green_result.gas_used
    _assert_fuel_ceiling(green_result, 255_000)
    with pytest.raises(AssertionError):
        _assert_fuel_ceiling(wrong_result, 255_000)


def _resource_measurement(
    resource_probe_wasm: Path,
    bytecode: bytes,
    selector: int,
) -> tuple[int, int]:
    limits = profile_execution_limits(
        verify_runtime_profile_lock(
            XAHAU_RUNTIME_PROFILE_LOCK,
            XAHAU_HOOK_PROVIDER_WASM,
        )
    )
    handler = _RecordingHost()
    host = WasmHost(
        handler=handler,
        wasm_path=resource_probe_wasm,
        execution_limits=limits,
    )
    handler.host = host
    host.init()
    exports = host.instance.exports(host.store)

    def resource(name: str) -> int:
        return exports[name](host.store)

    try:
        before_size = resource("qjs_resource_current_size")
        before_count = resource("qjs_resource_current_count")
        exports["qjs_resource_reset_peak"](host.store)
        result = host.run_hook_bytecode(
            bytecode,
            export="cbak",
            reserved=selector,
        )
        peak_size = resource("qjs_resource_peak_size")
        peak_count = resource("qjs_resource_peak_count")
    finally:
        host.destroy()

    _assert_terminal_only(result, handler)
    before_requested = before_size - before_count * _WASM_MALLOC_OVERHEAD
    peak_requested = peak_size - peak_count * _WASM_MALLOC_OVERHEAD
    return peak_requested - before_requested, peak_count - before_count


def _assert_constant_resources(measurements: list[tuple[int, int]]) -> None:
    assert len(set(measurements)) == 1


def test_arithmetic_resource_peak_is_constant_across_exponent_gaps(
    tmp_path: Path,
    resource_probe_wasm: Path,
):
    lane_ids = (
        "add.same-exponent-positive",
        "add.align-delta-16-six-tenths",
        "add.align-delta-176",
    )
    cases = {case["id"]: case for case in ORACLE["cases"] if case["id"] in lane_ids}
    branches = "".join(
        (f"if(info.rawFlags==={selector}){{{_case_statements([cases[case_id]])}}}else ")
        for selector, case_id in enumerate(lane_ids, start=1)
    )
    source = _profiled_source(
        branches
        + '{rollback("unknown resource lane");}'
        + f'return accept("{_SUCCESS_MESSAGE}",0);',
        callback=True,
    )
    packaged = _package(tmp_path, source, "resource-lanes.hook.ts")

    measurements = [
        _resource_measurement(resource_probe_wasm, packaged.bytecode, selector)
        for selector in range(1, 4)
    ]

    _assert_constant_resources(measurements)

    # Red control: Uint8Array's backing store enters QuickJS's malloc path, so
    # this is the same-harness `malloc(gap)` mutant required by the resource
    # gate. Arithmetic, terminal routing, and provider identity remain valid.
    mutant_branches = "".join(
        (
            f"if(info.rawFlags==={selector}){{"
            + (
                "const exponentGap=176;const gapAllocation="
                "new Uint8Array(exponentGap);gapAllocation[0]=1;"
                if selector == 3
                else ""
            )
            + f"{_case_statements([cases[case_id]])}}}else "
        )
        for selector, case_id in enumerate(lane_ids, start=1)
    )
    mutant = _package(
        tmp_path,
        _profiled_source(
            mutant_branches
            + '{rollback("unknown resource lane");}'
            + f'return accept("{_SUCCESS_MESSAGE}",0);',
            callback=True,
        ),
        "resource-allocation-mutant.hook.ts",
    )
    mutant_measurements = [
        _resource_measurement(resource_probe_wasm, mutant.bytecode, selector)
        for selector in (1, 3)
    ]
    with pytest.raises(AssertionError):
        _assert_constant_resources(mutant_measurements)


def _compile_unchecked(source: str) -> bytes:
    host = WasmHost(wasm_path=XAHAU_HOOK_PROVIDER_WASM)
    host.init()
    try:
        return host.compile_source(source, module=True)
    finally:
        host.destroy()


@pytest.mark.parametrize(
    ("config", "expected"),
    [
        (
            "",
            "TypeError: XFLDecimal.add: arithmetic profile does not implement "
            "operation",
        ),
        (
            "export const hookConfig=defineHookConfig({xflArithmetic:"
            "XFLProfile.nearestEvenV1});",
            "TypeError: XFLDecimal.add: arithmetic profile does not implement "
            "operation",
        ),
    ],
)
def test_unchecked_profile_bypass_fails_closed_before_arithmetic(
    config: str, expected: str
):
    value = _decimal_expression(_typed_integer(ORACLE["guard_drop_control"]["a"]))
    bytecode = _compile_unchecked(
        f"{config}export function main(){{const value={value};"
        "return value.add(value);}"
    )

    result, handler = _execute(bytecode)

    assert result == ContractResult(
        exit_code=-1,
        error=expected,
        result_value=expected,
        gas_used=result.gas_used,
        host_work_used=0,
    )
    assert handler.calls == []


def test_unchecked_module_scope_arithmetic_fails_in_inactive_context():
    value = _decimal_expression(_typed_integer(ORACLE["guard_drop_control"]["a"]))
    bytecode = _compile_unchecked(
        "export const hookConfig=defineHookConfig({xflArithmetic:"
        f"XFLProfile.xahauFloatV1}});const value={value};value.add(value);"
        "export function main(){return accept();}"
    )
    host = WasmHost.profiled()
    host.init()
    try:
        validation = host.validate_hook_bytecode(bytecode)
    finally:
        host.destroy()

    assert not validation.valid
    assert validation.error == (
        "TypeError: XFLDecimal.add: arithmetic profile is inactive"
    )
