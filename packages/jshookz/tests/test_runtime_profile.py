import copy
import hashlib
import json
from pathlib import Path

import pytest

from jshookz.paths import (
    XAHAU_HOOK_PROVIDER_WASM,
    XAHAU_RUNTIME_PROFILE_LOCK,
    XAHAU_RUNTIME_PROFILE_SOURCE,
)
from jshookz.build import _validate_native_abi, seal_xahau_hook_provider_bundle
from jshookz.host import WasmHost
from jshookz.runtime_profile import (
    build_runtime_profile_lock,
    canonical_json_bytes,
    load_runtime_profile_lock,
    verify_runtime_profile_lock,
)


SOURCE = XAHAU_RUNTIME_PROFILE_SOURCE
CHECKED_LOCK = XAHAU_RUNTIME_PROFILE_LOCK


def _write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def test_checked_profile_lock_is_fresh():
    expected = build_runtime_profile_lock(SOURCE, XAHAU_HOOK_PROVIDER_WASM)
    assert json.loads(CHECKED_LOCK.read_text()) == expected


def test_profile_lock_pins_provider_and_has_no_wasi(tmp_path: Path):
    lock = build_runtime_profile_lock(SOURCE, XAHAU_HOOK_PROVIDER_WASM)
    lock_path = tmp_path / "profile.lock.json"
    _write_json(lock_path, lock)

    loaded = verify_runtime_profile_lock(lock_path, XAHAU_HOOK_PROVIDER_WASM)

    assert loaded.bytecode_abi_id.hex() == lock["bytecode_abi_id"]
    assert loaded.runtime_profile_id.hex() == lock["runtime_profile_id"]
    assert len(loaded.bytecode_abi_id) == 32
    assert len(loaded.runtime_profile_id) == 32
    assert {item["module"] for item in lock["provider"]["imports"]} == {"env"}
    memory = next(
        item for item in lock["provider"]["exports"] if item["name"] == "memory"
    )
    assert memory == {
        "name": "memory",
        "kind": "memory",
        "minimum_pages": 4,
        "maximum_pages": 512,
        "memory64": False,
        "shared": False,
    }
    assert canonical_json_bytes(lock).endswith(b"\n")


def test_profile_lock_rejects_tampered_identity(tmp_path: Path):
    lock = build_runtime_profile_lock(SOURCE, XAHAU_HOOK_PROVIDER_WASM)
    lock["runtime_profile_id"] = "00" * 32
    lock_path = tmp_path / "profile.lock.json"
    _write_json(lock_path, lock)

    with pytest.raises(ValueError, match="runtime profile identity"):
        verify_runtime_profile_lock(lock_path, XAHAU_HOOK_PROVIDER_WASM)


def test_profile_source_rejects_import_drift(tmp_path: Path):
    source = json.loads(SOURCE.read_text())
    drifted = copy.deepcopy(source)
    drifted["provider"]["imports"] = drifted["provider"]["imports"][1:]
    source_path = tmp_path / "drifted.source.json"
    _write_json(source_path, drifted)

    with pytest.raises(ValueError, match="import surface differs"):
        build_runtime_profile_lock(source_path, XAHAU_HOOK_PROVIDER_WASM)


def test_profile_source_rejects_export_signature_drift(tmp_path: Path):
    source = json.loads(SOURCE.read_text())
    drifted = copy.deepcopy(source)
    qjs_hook = next(
        item
        for item in drifted["provider"]["required_exports"]
        if item["name"] == "qjs_hook"
    )
    qjs_hook["results"] = ["i64"]
    source_path = tmp_path / "drifted.source.json"
    _write_json(source_path, drifted)

    with pytest.raises(ValueError, match="qjs_hook differs"):
        build_runtime_profile_lock(source_path, XAHAU_HOOK_PROVIDER_WASM)


def test_profile_verification_reapplies_source_policy(tmp_path: Path):
    lock = build_runtime_profile_lock(SOURCE, XAHAU_HOOK_PROVIDER_WASM)
    lock["source"]["provider"]["forbidden_import_modules"] = ["env"]
    identity_manifest = {
        "source": lock["source"],
        "bytecode_abi_id": lock["bytecode_abi_id"],
        "provider_wasm_sha256": lock["provider"]["sha256"],
        "provider_imports": lock["provider"]["imports"],
        "provider_exports": lock["provider"]["exports"],
    }
    lock["runtime_profile_id"] = hashlib.sha256(
        canonical_json_bytes(
            {
                "domain": "xahau.quickjs.runtime-profile.v1",
                "manifest": identity_manifest,
            }
        )
    ).hexdigest()
    lock_path = tmp_path / "policy-drift.lock.json"
    _write_json(lock_path, lock)

    with pytest.raises(ValueError, match="forbidden imports"):
        verify_runtime_profile_lock(lock_path, XAHAU_HOOK_PROVIDER_WASM)


def test_load_profile_rejects_short_identity(tmp_path: Path):
    lock = build_runtime_profile_lock(SOURCE, XAHAU_HOOK_PROVIDER_WASM)
    lock["bytecode_abi_id"] = "ff"
    lock_path = tmp_path / "profile.lock.json"
    _write_json(lock_path, lock)

    with pytest.raises(ValueError, match="32 bytes"):
        load_runtime_profile_lock(lock_path)


def test_provider_bundle_manifest_is_the_verified_profile_lock(tmp_path: Path):
    lock = build_runtime_profile_lock(SOURCE, XAHAU_HOOK_PROVIDER_WASM)
    lock_path = tmp_path / "profile.lock.json"
    manifest_path = tmp_path / "jshookz_provider.manifest.json"
    cmake_manifest_path = tmp_path / "jshookz_provider.manifest.cmake"
    native_abi_path = tmp_path / "jshookz_provider.native-abi.json"
    _write_json(lock_path, lock)

    emitted = seal_xahau_hook_provider_bundle(
        XAHAU_HOOK_PROVIDER_WASM,
        lock_path,
        manifest_path,
        cmake_manifest_path,
        native_abi_path,
    )

    assert emitted == manifest_path
    assert manifest_path.read_bytes() == lock_path.read_bytes()
    cmake_manifest = cmake_manifest_path.read_text()
    assert f'"{lock["provider"]["sha256"]}"' in cmake_manifest
    assert f'"{lock["bytecode_abi_id"]}"' in cmake_manifest
    assert f'"{lock["runtime_profile_id"]}"' in cmake_manifest
    assert 'XAHAU_QUICKJS_WASMTIME_VERSION "47.0.3"' in cmake_manifest
    assert 'XAHAU_QUICKJS_INITIALIZATION_FUEL "5000000"' in cmake_manifest
    assert (
        'XAHAU_QUICKJS_HOST_WORK_METER "base-plus-addressed-byte-v1"' in cmake_manifest
    )
    assert (
        'XAHAU_QUICKJS_HOST_ADAPTER_POLICY "xahau-raw-hook-host-v1"' in cmake_manifest
    )
    assert 'XAHAU_QUICKJS_PROVIDER_IMPORT_COUNT "13"' in cmake_manifest
    assert (
        'XAHAU_QUICKJS_NATIVE_ABI_FILE "jshookz_provider.native-abi.json"'
        in cmake_manifest
    )
    assert (
        hashlib.sha256(native_abi_path.read_bytes()).hexdigest()
        in cmake_manifest
    )
    assert (
        json.loads(native_abi_path.read_text())["source"]["macro_function_count"]
        == 75
    )


def test_native_projection_rejects_duplicate_provider_import():
    imports = json.loads(CHECKED_LOCK.read_text())["provider"]["imports"]
    with pytest.raises(ValueError, match="pinned raw Hook ABI"):
        _validate_native_abi([*imports, imports[0]])


class _LedgerClockHost:
    def ledger_last_time(self) -> int:
        return 123


def test_profile_javascript_surface_is_ledger_derived_and_reduced():
    host = WasmHost(
        handler=_LedgerClockHost(),
        wasm_path=XAHAU_HOOK_PROVIDER_WASM,
        fuel=50_000_000,
    )
    host.init()
    try:
        expected_ms = (946_684_800 + 123) * 1000
        result = host.eval(
            "JSON.stringify({"
            "now: Date.now(),"
            "constructed: new Date().getTime(),"
            "explicit: new Date(0).getTime(),"
            "constructorEscape: Date.prototype.constructor.now(),"
            "timezone: new Date().getTimezoneOffset(),"
            "random: typeof Math.random,"
            "shared: typeof SharedArrayBuffer,"
            "atomics: typeof Atomics,"
            "weak: typeof WeakRef,"
            "finalizer: typeof FinalizationRegistry,"
            "promise: typeof Promise,"
            "number: typeof Number"
            "})"
        )
    finally:
        host.destroy()

    assert result.ok
    assert json.loads(result.result_value) == {
        "now": expected_ms,
        "constructed": expected_ms,
        "explicit": 0,
        "constructorEscape": expected_ms,
        "timezone": 0,
        "random": "undefined",
        "shared": "undefined",
        "atomics": "undefined",
        "weak": "undefined",
        "finalizer": "undefined",
        "promise": "function",
        "number": "function",
    }
