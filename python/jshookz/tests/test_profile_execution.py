"""Behavioral gates for the sealed execution-profile limits."""

from dataclasses import dataclass, replace
from pathlib import Path

from jshookz.host import WasmHost
from jshookz.paths import (
    XAHAU_HOOK_PROVIDER_WASM,
    XAHAU_HOOK_PROVIDER_UNWIZERED_WASM,
    XAHAU_RUNTIME_PROFILE_LOCK,
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


@dataclass(frozen=True)
class _InitializationFuel:
    envelope: int
    instantiation_used: int
    profile_configuration_used: int
    qjs_init_used: int
    total_used: int
    remaining: int


_COLD_INITIALIZATION = _InitializationFuel(
    envelope=5_000_000,
    instantiation_used=131_141,
    profile_configuration_used=16,
    qjs_init_used=2_995_726,
    total_used=3_126_883,
    remaining=1_873_117,
)
_SEALED_INITIALIZATION = _InitializationFuel(
    envelope=5_000_000,
    instantiation_used=278_535,
    profile_configuration_used=31,
    qjs_init_used=4,
    total_used=278_570,
    remaining=4_721_430,
)


def _limits():
    lock = verify_runtime_profile_lock(
        XAHAU_RUNTIME_PROFILE_LOCK,
        XAHAU_HOOK_PROVIDER_WASM,
    )
    return profile_execution_limits(lock)


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

    assert measurement == _COLD_INITIALIZATION
    assert measurement.envelope == 5_000_000
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


def test_sealed_initialization_uses_the_noop_path_with_more_headroom():
    cold = _measure_initialization_fuel(XAHAU_HOOK_PROVIDER_UNWIZERED_WASM)
    sealed = _measure_initialization_fuel(XAHAU_HOOK_PROVIDER_WASM)

    assert cold == _COLD_INITIALIZATION
    assert sealed == _SEALED_INITIALIZATION
    assert sealed.envelope == cold.envelope == 5_000_000
    assert sealed.qjs_init_used == 4
    assert cold.qjs_init_used > sealed.qjs_init_used
    assert sealed.total_used < cold.total_used
    assert sealed.remaining > cold.remaining
    assert sealed.remaining == sealed.envelope - sealed.total_used


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
    host = WasmHost.profiled(handler=handler)
    host.init()
    try:
        small = host.eval('trace("abc")')
        exhausted = host.eval('trace("x".repeat(1_000_000))')
    finally:
        host.destroy()

    assert small.ok, small.error
    assert small.host_work_used == 4
    assert not exhausted.ok
    assert exhausted.error == "host work budget exhausted"
    assert exhausted.host_work_used == _limits().host_work_budget
    assert handler.calls == 1
