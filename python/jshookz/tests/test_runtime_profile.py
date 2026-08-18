import copy
import hashlib
import importlib.metadata
import json
import tomllib
from pathlib import Path

import pytest

from jshookz.paths import (
    XAHAU_HOOK_PROVIDER_WASM,
    XAHAU_RUNTIME_PROFILE_SOURCE,
    XAHAU_V1_HOOKS_API_DECLARATIONS,
    XAHAU_V1_JAVASCRIPT_SURFACE,
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


def _write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def test_native_engine_and_python_oracle_versions_are_named_separately():
    source = json.loads(SOURCE.read_text())
    project = tomllib.loads(
        (SOURCE.parents[2] / "python/jshookz/pyproject.toml").read_text()
    )["project"]

    assert source["engine"]["implementation"] == "wasmtime-native-c-api"
    assert source["engine"]["version"] == "47.0.3"
    assert "wasmtime==47.0.1" in project["dependencies"]
    assert importlib.metadata.version("wasmtime") == "47.0.1"


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
        "minimum_pages": 6,
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
        "javascript_surface": lock["javascript_surface"],
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


def test_profile_verification_rejects_javascript_surface_drift(tmp_path: Path):
    lock = build_runtime_profile_lock(SOURCE, XAHAU_HOOK_PROVIDER_WASM)
    lock_path = tmp_path / "profile.lock.json"
    _write_json(lock_path, lock)

    surface = json.loads(XAHAU_V1_JAVASCRIPT_SURFACE.read_text())
    surface["globals"]["invented"] = "function"
    surface_path = tmp_path / "surface.json"
    _write_json(surface_path, surface)

    with pytest.raises(ValueError, match="JavaScript surface does not match"):
        verify_runtime_profile_lock(
            lock_path,
            XAHAU_HOOK_PROVIDER_WASM,
            surface_path,
            XAHAU_V1_HOOKS_API_DECLARATIONS,
        )


def test_profile_opens_its_selected_javascript_artifacts(tmp_path: Path):
    source = copy.deepcopy(json.loads(SOURCE.read_text()))
    selected_root = "selected/api"
    source["javascript_surface"]["manifest"] = f"{selected_root}/surface.json"
    source["javascript_surface"]["declaration"] = f"{selected_root}/api.d.ts"
    source_path = tmp_path / "profiles/profile.source.json"
    source_path.parent.mkdir(parents=True)
    selected = tmp_path / selected_root
    selected.mkdir(parents=True)
    declaration_path = selected / "api.d.ts"
    declaration_path.write_bytes(XAHAU_V1_HOOKS_API_DECLARATIONS.read_bytes())
    surface = json.loads(XAHAU_V1_JAVASCRIPT_SURFACE.read_text())
    surface["declaration"]["path"] = f"{selected_root}/api.d.ts"
    surface_path = selected / "surface.json"
    _write_json(surface_path, surface)
    _write_json(source_path, source)

    lock = build_runtime_profile_lock(source_path, XAHAU_HOOK_PROVIDER_WASM)

    assert lock["javascript_surface"]["manifest"] == f"{selected_root}/surface.json"
    assert lock["javascript_surface"]["declaration"] == f"{selected_root}/api.d.ts"
    assert lock["javascript_surface"]["sha256"] == hashlib.sha256(
        surface_path.read_bytes()
    ).hexdigest()


def test_profile_does_not_fall_back_for_an_unselected_manifest(tmp_path: Path):
    source = copy.deepcopy(json.loads(SOURCE.read_text()))
    source["javascript_surface"]["manifest"] = "missing/selected-surface.json"
    source_path = tmp_path / "profile.source.json"
    _write_json(source_path, source)

    with pytest.raises(ValueError, match="selected runtime-profile.*does not exist"):
        build_runtime_profile_lock(source_path, XAHAU_HOOK_PROVIDER_WASM)


def test_load_profile_rejects_short_identity(tmp_path: Path):
    lock = build_runtime_profile_lock(SOURCE, XAHAU_HOOK_PROVIDER_WASM)
    lock["bytecode_abi_id"] = "ff"
    lock_path = tmp_path / "profile.lock.json"
    _write_json(lock_path, lock)

    with pytest.raises(ValueError, match="32 bytes"):
        load_runtime_profile_lock(lock_path)


def test_provider_bundle_emits_the_profile_lock(tmp_path: Path):
    lock = build_runtime_profile_lock(SOURCE, XAHAU_HOOK_PROVIDER_WASM)
    lock_path = tmp_path / "profile.lock.json"
    manifest_path = tmp_path / "jshookz_provider.manifest.json"
    cmake_manifest_path = tmp_path / "jshookz_provider.manifest.cmake"
    native_abi_path = tmp_path / "jshookz_provider.native-abi.json"
    emitted = seal_xahau_hook_provider_bundle(
        XAHAU_HOOK_PROVIDER_WASM,
        lock_path,
        manifest_path,
        cmake_manifest_path,
        native_abi_path,
    )

    assert emitted == manifest_path
    assert json.loads(manifest_path.read_text()) == lock
    assert lock_path.read_bytes() == manifest_path.read_bytes()
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
    imports = build_runtime_profile_lock(SOURCE, XAHAU_HOOK_PROVIDER_WASM)[
        "provider"
    ]["imports"]
    with pytest.raises(ValueError, match="pinned raw Hook ABI"):
        _validate_native_abi([*imports, imports[0]])


class _LedgerClockHost:
    def ledger_last_time(self) -> int:
        return 123

    def state_set(self, *_args: object) -> int:
        return 0


QUICKJS_LANGUAGE_GLOBALS = frozenset(
    {
        "AggregateError",
        "Array",
        "ArrayBuffer",
        "BigInt",
        "BigInt64Array",
        "BigUint64Array",
        "Boolean",
        "DataView",
        "Date",
        "Error",
        "EvalError",
        "Float16Array",
        "Float32Array",
        "Float64Array",
        "Function",
        "Infinity",
        "Int16Array",
        "Int32Array",
        "Int8Array",
        "InternalError",
        "Iterator",
        "JSON",
        "Map",
        "Math",
        "NaN",
        "Number",
        "Object",
        "Promise",
        "Proxy",
        "RangeError",
        "ReferenceError",
        "Reflect",
        "RegExp",
        "Set",
        "String",
        "Symbol",
        "SyntaxError",
        "TypeError",
        "URIError",
        "Uint16Array",
        "Uint32Array",
        "Uint8Array",
        "Uint8ClampedArray",
        "WeakMap",
        "WeakSet",
        "decodeURI",
        "decodeURIComponent",
        "encodeURI",
        "encodeURIComponent",
        "escape",
        "eval",
        "globalThis",
        "isFinite",
        "isNaN",
        "parseFloat",
        "parseInt",
        "undefined",
        "unescape",
    }
)


def _surface_probe_javascript(surface: dict[str, object]) -> str:
    """Turn the generated surface manifest into one provider-side probe."""
    encoded = json.dumps(json.dumps(surface, sort_keys=True))
    language_globals = json.dumps(sorted(QUICKJS_LANGUAGE_GLOBALS))
    return f"""
JSON.stringify((() => {{
  const surface = JSON.parse({encoded});
  const languageGlobals = new Set({language_globals});
  const functionOwn = new Set(["arguments", "caller", "length", "name", "prototype"]);
  const failures = [];
  const compareNames = (label, actual, expected) => {{
    const actualSet = new Set(actual);
    const expectedSet = new Set(expected);
    for (const name of expected)
      if (!actualSet.has(name)) failures.push(`${{label}}.${{name}}: missing`);
    for (const name of actual)
      if (!expectedSet.has(name)) failures.push(`${{label}}.${{name}}: extra`);
  }};
  const checkMembers = (label, receiver, members, ignored = new Set()) => {{
    compareNames(
      label,
      Object.getOwnPropertyNames(receiver).filter(name => !ignored.has(name)).sort(),
      Object.keys(members).sort(),
    );
    for (const [name, kind] of Object.entries(members)) {{
      if (kind === "function" ? typeof receiver[name] !== "function"
                              : !(name in receiver))
        failures.push(`${{label}}.${{name}}: expected ${{kind}}`);
    }}
  }};

  compareNames(
    "global",
    Object.getOwnPropertyNames(globalThis)
      .filter(name => !languageGlobals.has(name)).sort(),
    Object.keys(surface.globals).sort(),
  );
  for (const [name, kind] of Object.entries(surface.globals))
    if (typeof globalThis[name] !== kind)
      failures.push(`global ${{name}}: expected ${{kind}}`);

  for (const group of ["namespaces", "statics"])
    for (const [name, members] of Object.entries(surface[group]))
      checkMembers(
        `${{group.slice(0, -1)}} ${{name}}`,
        globalThis[name],
        members,
        typeof globalThis[name] === "function" ? functionOwn : new Set(),
      );

  for (const [name, profile] of Object.entries(surface.prototypes)) {{
    const receiver = eval(profile.probe);
    const prototype = Object.getPrototypeOf(receiver);
    for (const [member, kind] of Object.entries(profile.members))
      if (kind === "function" ? typeof receiver[member] !== "function"
                              : !(member in receiver))
        failures.push(`prototype ${{name}}.${{member}}: expected ${{kind}}`);
    compareNames(
      `prototype ${{name}}`,
      Object.getOwnPropertyNames(prototype).sort(),
      [...profile.own].sort(),
    );
    if (profile.frozen && !Object.isFrozen(prototype))
      failures.push(`prototype ${{name}}: not frozen`);
    if (name === "Result" && "moot" in receiver)
      failures.push("prototype Result.moot: unexpectedly present");
    if (name === "VoidResult" && !Object.isFrozen(Object.getPrototypeOf(prototype)))
      failures.push("prototype VoidResult: base not frozen");
  }}
  for (const name of ["zero", "one"]) {{
    if (AccountID[name] !== AccountID[name])
      failures.push(`static AccountID.${{name}}: identity is not stable`);
    if (!Object.isFrozen(AccountID[name]))
      failures.push(`static AccountID.${{name}}: value is not frozen`);
  }}
  return failures;
}})())
""".strip()


def test_profile_javascript_api_matches_generated_surface():
    surface = json.loads(XAHAU_V1_JAVASCRIPT_SURFACE.read_text())
    host = WasmHost.profiled(
        handler=_LedgerClockHost(),
        wasm_path=XAHAU_HOOK_PROVIDER_WASM,
    )
    host.init()
    try:
        result = host.eval(_surface_probe_javascript(surface))
    finally:
        host.destroy()

    assert result.ok, result.error
    assert json.loads(result.result_value) == []


def test_profile_surface_probe_rejects_an_extra_provider_member():
    surface = json.loads(XAHAU_V1_JAVASCRIPT_SURFACE.read_text())
    del surface["namespaces"]["state"]["set"]
    host = WasmHost.profiled(
        handler=_LedgerClockHost(),
        wasm_path=XAHAU_HOOK_PROVIDER_WASM,
    )
    host.init()
    try:
        result = host.eval(_surface_probe_javascript(surface))
    finally:
        host.destroy()

    assert result.ok, result.error
    assert "namespace state.set: extra" in json.loads(result.result_value)


def test_profile_surface_probe_rejects_an_extra_provider_root():
    surface = json.loads(XAHAU_V1_JAVASCRIPT_SURFACE.read_text())
    probe = "globalThis.smuggledProviderRoot = {};\n" + _surface_probe_javascript(surface)
    host = WasmHost.profiled(
        handler=_LedgerClockHost(),
        wasm_path=XAHAU_HOOK_PROVIDER_WASM,
    )
    host.init()
    try:
        result = host.eval(probe)
    finally:
        host.destroy()

    assert result.ok, result.error
    assert "global.smuggledProviderRoot: extra" in json.loads(result.result_value)


def test_profile_javascript_surface_is_ledger_derived_and_reduced():
    host = WasmHost.profiled(
        handler=_LedgerClockHost(),
        wasm_path=XAHAU_HOOK_PROVIDER_WASM,
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
            "setTimeout: typeof setTimeout,"
            "clearTimeout: typeof clearTimeout,"
            "setInterval: typeof setInterval,"
            "clearInterval: typeof clearInterval,"
            "setImmediate: typeof setImmediate,"
            "clearImmediate: typeof clearImmediate,"
            "requestAnimationFrame: typeof requestAnimationFrame,"
            "cancelAnimationFrame: typeof cancelAnimationFrame,"
            "queueMicrotask: typeof queueMicrotask,"
            "accept: typeof accept,"
            "rollback: typeof rollback,"
            "rollbackOnFail: typeof rollback.onFail,"
            "hookAccount: typeof hook.account,"
            "hookAccept: typeof hook.accept,"
            "hookRollback: typeof hook.rollback,"
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
        "setTimeout": "undefined",
        "clearTimeout": "undefined",
        "setInterval": "undefined",
        "clearInterval": "undefined",
        "setImmediate": "undefined",
        "clearImmediate": "undefined",
        "requestAnimationFrame": "undefined",
        "cancelAnimationFrame": "undefined",
        "queueMicrotask": "undefined",
        "accept": "function",
        "rollback": "function",
        "rollbackOnFail": "function",
        "hookAccount": "function",
        "hookAccept": "undefined",
        "hookRollback": "undefined",
        "promise": "function",
        "number": "function",
    }
