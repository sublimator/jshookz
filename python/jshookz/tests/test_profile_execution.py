"""Behavioral gates for the sealed execution-profile limits."""

import json
from dataclasses import dataclass, replace
from pathlib import Path

from jshookz.host import WasmHost
from jshookz.paths import (
    XAHAU_HOOK_PROVIDER_UNWIZERED_WASM,
    XAHAU_HOOK_PROVIDER_WASM,
    XAHAU_RUNTIME_PROFILE_LOCK,
    XAHAU_RUNTIME_PROFILE_SOURCE,
)
from jshookz.runtime_profile import (
    profile_execution_limits,
    verify_runtime_profile_lock,
)


class _TraceHost:
    def __init__(self):
        self.calls = 0

    def trace(self, *_args):
        self.calls += 1
        return 0


class _BoundedReadHost:
    def otxn_param(self, *_args):
        return -5

    def ledger_nonce(self, *_args):
        return -77

    def state_foreign(self, *_args):
        return -5


class _AgainHost:
    def __init__(self):
        self.calls = 0

    def hook_again(self):
        self.calls += 1
        return 1


@dataclass(frozen=True)
class _InitializationFuel:
    envelope: int
    instantiation_used: int
    profile_configuration_used: int
    qjs_init_used: int
    total_used: int
    remaining: int


_COLD_INITIALIZATION_CEILING = 5_000_000
_COLD_INITIALIZATION_HEADROOM = 500_000
_SEALED_INITIALIZATION_CEILING = 1_000_000
_SEALED_INITIALIZATION_HEADROOM = 4_000_000


def _limits():
    lock = verify_runtime_profile_lock(
        XAHAU_RUNTIME_PROFILE_LOCK,
        XAHAU_HOOK_PROVIDER_WASM,
    )
    return profile_execution_limits(lock)


def _source_initialization_fuel() -> int:
    source = json.loads(XAHAU_RUNTIME_PROFILE_SOURCE.read_text())
    value = source["limits"]["wasmtime_fuel_per_initialization"]
    assert isinstance(value, int) and not isinstance(value, bool) and value > 0
    return value


def _measure_initialization_fuel(wasm_path: Path) -> _InitializationFuel:
    limits = _limits()
    host = WasmHost(wasm_path=wasm_path, execution_limits=limits)
    try:
        after_instantiation = host.store.get_fuel()
        host.set_memory_limit(limits.quickjs_heap_bytes)
        host.set_max_stack_size(limits.quickjs_stack_bytes)
        before_qjs_init = host.store.get_fuel()
        host.instance.exports(host.store)["qjs_init"](host.store)
        after_qjs_init = host.store.get_fuel()
        measurement = _InitializationFuel(
            envelope=limits.initialization_fuel,
            instantiation_used=limits.initialization_fuel - after_instantiation,
            profile_configuration_used=after_instantiation - before_qjs_init,
            qjs_init_used=before_qjs_init - after_qjs_init,
            total_used=limits.initialization_fuel - after_qjs_init,
            remaining=after_qjs_init,
        )
    finally:
        host.destroy()

    assert host._destroyed
    assert host.store is None
    assert host.instance is None
    return measurement


def _assert_initialization_accounting(measurement: _InitializationFuel) -> None:
    assert measurement.envelope == _source_initialization_fuel()
    assert measurement.instantiation_used > 0
    assert measurement.profile_configuration_used > 0
    assert measurement.qjs_init_used > 0
    assert measurement.total_used == (
        measurement.instantiation_used
        + measurement.profile_configuration_used
        + measurement.qjs_init_used
    )
    assert measurement.remaining == measurement.envelope - measurement.total_used
    assert 0 < measurement.remaining < measurement.envelope


def test_quickjs_heap_limit_counts_cumulative_allocations():
    host = WasmHost(wasm_path=XAHAU_HOOK_PROVIDER_WASM)
    host.init()
    host.set_memory_limit(1_000_000)
    try:
        result = host.eval(
            "globalThis.kept = [];"
            "for (let i = 0; i < 80; i++) "
            "  kept.push(new Uint8Array(200 * 1024));"
            "kept.length"
        )
    finally:
        host.destroy()

    assert not result.ok
    assert "out of memory" in (result.error or "").lower()


def test_preinitialized_init_fits_a_small_fuel_budget():
    limits = replace(_limits(), initialization_fuel=1_000_000)
    host = WasmHost(
        wasm_path=XAHAU_HOOK_PROVIDER_WASM,
        execution_limits=limits,
    )
    try:
        host.init()
    finally:
        host.destroy()


def test_cold_initialization_fits_the_profile_fuel_envelope():
    measurement = _measure_initialization_fuel(XAHAU_HOOK_PROVIDER_UNWIZERED_WASM)

    _assert_initialization_accounting(measurement)
    assert measurement.qjs_init_used > 3_000_000
    assert measurement.total_used <= _COLD_INITIALIZATION_CEILING
    assert measurement.remaining >= _COLD_INITIALIZATION_HEADROOM


def test_sealed_initialization_uses_the_noop_path_with_more_headroom():
    cold = _measure_initialization_fuel(XAHAU_HOOK_PROVIDER_UNWIZERED_WASM)
    sealed = _measure_initialization_fuel(XAHAU_HOOK_PROVIDER_WASM)

    _assert_initialization_accounting(cold)
    _assert_initialization_accounting(sealed)
    assert sealed.qjs_init_used <= 16
    assert cold.qjs_init_used > sealed.qjs_init_used
    assert sealed.total_used <= _SEALED_INITIALIZATION_CEILING
    assert sealed.remaining >= _SEALED_INITIALIZATION_HEADROOM
    assert sealed.total_used < cold.total_used
    assert sealed.remaining > cold.remaining


def test_profile_resets_to_invocation_fuel_and_cleans_up_after_exhaustion():
    limits = _limits()
    host = WasmHost.profiled()
    host.init()
    assert host.store.get_fuel() == limits.invocation_fuel
    try:
        result = host.eval("for (;;) {}")
    finally:
        host.destroy()

    assert not result.ok
    assert result.error == "out of gas"
    assert 0 < result.gas_used <= limits.invocation_fuel


def test_profile_heap_limit_counts_cumulative_allocations():
    host = WasmHost.profiled()
    host.init()
    try:
        result = host.eval(
            "globalThis.kept = [];"
            "for (let i = 0; i < 100; i++) "
            "  kept.push(new Uint8Array(200 * 1024));"
            "kept.length"
        )
    finally:
        host.destroy()

    assert not result.ok
    assert "out of memory" in (result.error or "").lower()


def test_profile_stack_limit_rejects_unbounded_recursion():
    host = WasmHost.profiled()
    host.init()
    try:
        result = host.eval("function recurse() { return recurse(); } recurse()")
    finally:
        host.destroy()

    assert not result.ok
    assert "stack overflow" in (result.error or "").lower()


def test_profile_host_work_counts_addressed_bytes_before_dispatch():
    handler = _TraceHost()
    limits = replace(_limits(), host_work_budget=5)
    host = WasmHost(handler=handler, execution_limits=limits)
    host.init()
    try:
        small = host.eval('trace("abc")')
        exhausted = host.eval('trace("x")')
    finally:
        host.destroy()

    assert small.ok, small.error
    assert small.host_work_used == 4
    assert not exhausted.ok
    assert exhausted.error == "host work budget exhausted"
    assert exhausted.host_work_used == limits.host_work_budget
    assert handler.calls == 1


def test_hook_again_charges_one_base_unit_and_exhausts_before_dispatch():
    handler = _AgainHost()
    limits = replace(_limits(), host_work_budget=2)
    host = WasmHost(handler=handler, execution_limits=limits)
    host.init()
    try:
        first = host.eval("hook.again().moot()")
        second = host.eval("hook.again().moot()")
        exhausted = host.eval("hook.again().moot()")
    finally:
        host.destroy()

    assert first.ok, first.error
    assert first.host_work_used == 1
    assert second.ok, second.error
    assert second.host_work_used == 2
    assert not exhausted.ok
    assert exhausted.error == "host work budget exhausted"
    assert exhausted.host_work_used == 2
    assert handler.calls == 2


def test_parameter_and_nonce_charge_their_bounded_addressed_lengths():
    host = WasmHost.profiled(handler=_BoundedReadHost())
    host.init()
    try:
        parameter = host.eval(
            "otxn.param(new Uint8Array([1,2,3])).ok"
        )
        nonce = host.eval("ledger.nonce().ok")
    finally:
        host.destroy()

    assert parameter.ok, parameter.error
    assert parameter.result_value == "true"
    assert parameter.host_work_used == 1 + 256 + 3
    assert nonce.ok, nonce.error
    assert nonce.result_value == "false"
    assert nonce.host_work_used - parameter.host_work_used == 1 + 32


def test_foreign_state_charges_output_key_namespace_and_account_lengths():
    host = WasmHost.profiled(handler=_BoundedReadHost())
    host.init()
    try:
        result = host.eval(
            "state.foreign(AccountID.zero, Hash256.zero).get('KEY').ok"
        )
    finally:
        host.destroy()

    assert result.ok, result.error
    assert result.result_value == "true"
    assert result.host_work_used == 1 + 4096 + 3 + 32 + 20
