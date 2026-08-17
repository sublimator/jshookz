"""Behavioral gates for the sealed execution-profile limits."""

from dataclasses import replace

import pytest

from jshookz.host import WasmHost
from jshookz.paths import (
    XAHAU_HOOK_PROVIDER_WASM,
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


def _limits():
    lock = verify_runtime_profile_lock(
        XAHAU_RUNTIME_PROFILE_LOCK,
        XAHAU_HOOK_PROVIDER_WASM,
    )
    return profile_execution_limits(lock)


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
