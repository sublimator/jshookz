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
    _validate_provider_policy,
    _wasm_stack_pointer_initial,
    build_runtime_profile_lock,
    canonical_json_bytes,
    load_runtime_profile_lock,
    profile_execution_limits,
    verify_runtime_profile_lock,
)


SOURCE = XAHAU_RUNTIME_PROFILE_SOURCE
OBJECT_LIMITS = {
    "serialized_object_max_bytes": 1_048_576,
    "serialized_object_max_fields": 32_768,
    "serialized_object_max_scopes": 32_769,
    "serialized_object_max_depth": 10,
}


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
    source = json.loads(SOURCE.read_text())
    lock = build_runtime_profile_lock(SOURCE, XAHAU_HOOK_PROVIDER_WASM)
    lock_path = tmp_path / "profile.lock.json"
    _write_json(lock_path, lock)

    loaded = verify_runtime_profile_lock(lock_path, XAHAU_HOOK_PROVIDER_WASM)

    assert loaded.bytecode_abi_id.hex() == lock["bytecode_abi_id"]
    assert loaded.runtime_profile_id.hex() == lock["runtime_profile_id"]
    assert len(loaded.bytecode_abi_id) == 32
    assert len(loaded.runtime_profile_id) == 32
    assert len(lock["provider"]["imports"]) == 17
    assert lock["source"]["limits"]["host_work_budget"] == 2_097_152
    assert {item["module"] for item in lock["provider"]["imports"]} == {"env"}
    assert lock["provider"]["imports"] == sorted(
        source["provider"]["imports"], key=lambda item: (item["module"], item["name"])
    )
    assert lock["provider"]["exports"] == source["provider"]["allowed_exports"]
    assert len(lock["provider"]["exports"]) == 22
    assert "wasi_snapshot_preview1" in source["provider"]["forbidden_import_modules"]
    memory = next(
        item for item in lock["provider"]["exports"] if item["name"] == "memory"
    )
    assert memory == {
        "name": "memory",
        "kind": "memory",
        "minimum_pages": 7,
        "maximum_pages": 512,
        "memory64": False,
        "shared": False,
    }
    assert lock["provider"]["build"] == {
        "wasm_stack_bytes": 131_072,
        "wasm_memory_max_bytes": 33_554_432,
    }
    assert _wasm_stack_pointer_initial(XAHAU_HOOK_PROVIDER_WASM.read_bytes()) == 131_072
    assert canonical_json_bytes(lock).endswith(b"\n")


@pytest.mark.parametrize(
    ("name", "delta"),
    [("wasm_stack_bytes", 65_536), ("wasm_memory_max_bytes", 65_536)],
)
def test_profile_source_rejects_provider_build_drift(
    tmp_path: Path, name: str, delta: int
):
    source = json.loads(SOURCE.read_text())
    source["provider"]["build"][name] += delta
    source_path = tmp_path / "drifted.source.json"
    _write_json(source_path, source)

    with pytest.raises(ValueError, match=rf"provider build {name} differs"):
        build_runtime_profile_lock(source_path, XAHAU_HOOK_PROVIDER_WASM)


def test_profile_projects_the_frozen_object_limits(tmp_path: Path):
    source = json.loads(SOURCE.read_text())
    assert {name: source["limits"][name] for name in OBJECT_LIMITS} == OBJECT_LIMITS

    lock = build_runtime_profile_lock(SOURCE, XAHAU_HOOK_PROVIDER_WASM)
    assert {
        name: lock["source"]["limits"][name] for name in OBJECT_LIMITS
    } == OBJECT_LIMITS
    lock_path = tmp_path / "profile.lock.json"
    _write_json(lock_path, lock)
    projected = profile_execution_limits(
        verify_runtime_profile_lock(lock_path, XAHAU_HOOK_PROVIDER_WASM)
    )

    assert {name: getattr(projected, name) for name in OBJECT_LIMITS} == OBJECT_LIMITS


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


def test_profile_policy_rejects_wasi_even_if_added_to_expected_imports():
    source = json.loads(SOURCE.read_text())
    provider = copy.deepcopy(
        build_runtime_profile_lock(SOURCE, XAHAU_HOOK_PROVIDER_WASM)["provider"]
    )
    wasi_import = {
        "module": "wasi_snapshot_preview1",
        "name": "proc_exit",
        "params": ["i32"],
        "results": [],
    }
    source["provider"]["imports"].append(wasi_import)
    provider["imports"].append(wasi_import)
    provider["imports"].sort(key=lambda item: (item["module"], item["name"]))

    with pytest.raises(ValueError, match="forbidden imports"):
        _validate_provider_policy(source, provider)


def test_profile_source_rejects_export_signature_drift(tmp_path: Path):
    source = json.loads(SOURCE.read_text())
    drifted = copy.deepcopy(source)
    qjs_hook = next(
        item
        for item in drifted["provider"]["allowed_exports"]
        if item["name"] == "qjs_hook"
    )
    qjs_hook["results"] = ["i64"]
    source_path = tmp_path / "drifted.source.json"
    _write_json(source_path, drifted)

    with pytest.raises(ValueError, match="qjs_hook differs"):
        build_runtime_profile_lock(source_path, XAHAU_HOOK_PROVIDER_WASM)


def test_profile_source_rejects_memory_limit_drift(tmp_path: Path):
    source = json.loads(SOURCE.read_text())
    memory = next(
        item
        for item in source["provider"]["allowed_exports"]
        if item["name"] == "memory"
    )
    memory["maximum_pages"] += 1
    source_path = tmp_path / "drifted.source.json"
    _write_json(source_path, source)

    with pytest.raises(ValueError, match="memory differs"):
        build_runtime_profile_lock(source_path, XAHAU_HOOK_PROVIDER_WASM)


def test_profile_source_rejects_deleted_allowed_export(tmp_path: Path):
    source = json.loads(SOURCE.read_text())
    drifted = copy.deepcopy(source)
    drifted["provider"]["allowed_exports"] = drifted["provider"]["allowed_exports"][:-1]
    source_path = tmp_path / "drifted.source.json"
    _write_json(source_path, drifted)

    with pytest.raises(ValueError, match="outside allowed_exports"):
        build_runtime_profile_lock(source_path, XAHAU_HOOK_PROVIDER_WASM)


def test_profile_source_rejects_added_allowed_export(tmp_path: Path):
    source = json.loads(SOURCE.read_text())
    drifted = copy.deepcopy(source)
    drifted["provider"]["allowed_exports"].append(
        {"name": "zz_invented", "kind": "function", "params": [], "results": []}
    )
    source_path = tmp_path / "drifted.source.json"
    _write_json(source_path, drifted)

    with pytest.raises(ValueError, match="missing allowed exports"):
        build_runtime_profile_lock(source_path, XAHAU_HOOK_PROVIDER_WASM)


def test_profile_source_rejects_reordered_allowed_exports(tmp_path: Path):
    source = json.loads(SOURCE.read_text())
    drifted = copy.deepcopy(source)
    exports = drifted["provider"]["allowed_exports"]
    exports[0], exports[1] = exports[1], exports[0]
    source_path = tmp_path / "drifted.source.json"
    _write_json(source_path, drifted)

    with pytest.raises(ValueError, match="normalized by export name"):
        build_runtime_profile_lock(source_path, XAHAU_HOOK_PROVIDER_WASM)


def test_profile_policy_rejects_extra_provider_export():
    source = json.loads(SOURCE.read_text())
    provider = copy.deepcopy(
        build_runtime_profile_lock(SOURCE, XAHAU_HOOK_PROVIDER_WASM)["provider"]
    )
    provider["exports"].append(
        {"name": "zz_smuggled", "kind": "function", "params": [], "results": []}
    )

    with pytest.raises(ValueError, match="outside allowed_exports"):
        _validate_provider_policy(source, provider)


@pytest.mark.parametrize("name", OBJECT_LIMITS)
def test_profile_source_rejects_deleted_object_limit(tmp_path: Path, name: str):
    source = json.loads(SOURCE.read_text())
    del source["limits"][name]
    source_path = tmp_path / "drifted.source.json"
    _write_json(source_path, source)

    with pytest.raises(ValueError, match=rf"object limit {name!r}"):
        build_runtime_profile_lock(source_path, XAHAU_HOOK_PROVIDER_WASM)


@pytest.mark.parametrize("value", [True, 0, -1, 1 << 32, "1048576"])
def test_profile_source_rejects_invalid_object_limit(tmp_path: Path, value: object):
    source = json.loads(SOURCE.read_text())
    source["limits"]["serialized_object_max_bytes"] = value
    source_path = tmp_path / "drifted.source.json"
    _write_json(source_path, source)

    with pytest.raises(ValueError, match="must be a positive uint32"):
        build_runtime_profile_lock(source_path, XAHAU_HOOK_PROVIDER_WASM)


def test_profile_verification_rejects_stale_companion_source(tmp_path: Path):
    lock = build_runtime_profile_lock(SOURCE, XAHAU_HOOK_PROVIDER_WASM)
    lock_path = tmp_path / "profile.lock.json"
    _write_json(lock_path, lock)
    stale_source = copy.deepcopy(lock["source"])
    stale_source["limits"]["serialized_object_max_fields"] += 1
    _write_json(tmp_path / "profile.source.json", stale_source)

    with pytest.raises(ValueError, match="does not match its companion source"):
        verify_runtime_profile_lock(lock_path, XAHAU_HOOK_PROVIDER_WASM)


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
    assert (
        lock["javascript_surface"]["sha256"]
        == hashlib.sha256(surface_path.read_bytes()).hexdigest()
    )


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
    assert 'XAHAU_QUICKJS_PROVIDER_IMPORT_COUNT "17"' in cmake_manifest
    assert 'XAHAU_QUICKJS_PROVIDER_EXPORT_COUNT "22"' in cmake_manifest
    cmake_object_limits = {
        "XAHAU_QUICKJS_SERIALIZED_OBJECT_MAX_BYTES": 1_048_576,
        "XAHAU_QUICKJS_SERIALIZED_OBJECT_MAX_FIELDS": 32_768,
        "XAHAU_QUICKJS_SERIALIZED_OBJECT_MAX_SCOPES": 32_769,
        "XAHAU_QUICKJS_SERIALIZED_OBJECT_MAX_DEPTH": 10,
    }
    for name, value in cmake_object_limits.items():
        assert f'{name} "{value}"' in cmake_manifest
    assert (
        'XAHAU_QUICKJS_NATIVE_ABI_FILE "jshookz_provider.native-abi.json"'
        in cmake_manifest
    )
    assert hashlib.sha256(native_abi_path.read_bytes()).hexdigest() in cmake_manifest
    assert (
        json.loads(native_abi_path.read_text())["source"]["macro_function_count"] == 75
    )


def test_native_projection_rejects_duplicate_provider_import():
    imports = build_runtime_profile_lock(SOURCE, XAHAU_HOOK_PROVIDER_WASM)["provider"][
        "imports"
    ]
    with pytest.raises(ValueError, match="pinned raw Hook ABI"):
        _validate_native_abi([*imports, imports[0]])


def test_profile_source_rejects_fourteenth_float_sum_import(tmp_path: Path):
    source = copy.deepcopy(json.loads(SOURCE.read_text()))
    source["provider"]["imports"].append(
        {
            "module": "env",
            "name": "float_sum",
            "params": ["i64", "i64"],
            "results": ["i64"],
        }
    )
    source_path = tmp_path / "float-sum.source.json"
    _write_json(source_path, source)

    with pytest.raises(ValueError, match="provider import surface differs"):
        build_runtime_profile_lock(source_path, XAHAU_HOOK_PROVIDER_WASM)


def test_profile_source_rejects_existing_import_name_swap(tmp_path: Path):
    source = copy.deepcopy(json.loads(SOURCE.read_text()))
    source["provider"]["imports"][0]["name"] = "float_sum"
    source_path = tmp_path / "swapped-import.source.json"
    _write_json(source_path, source)

    with pytest.raises(ValueError, match="provider import surface differs"):
        build_runtime_profile_lock(source_path, XAHAU_HOOK_PROVIDER_WASM)


@pytest.mark.parametrize("mutation", ["append", "swap"])
def test_native_projection_rejects_float_sum_import_mutations(mutation: str):
    imports = copy.deepcopy(
        build_runtime_profile_lock(SOURCE, XAHAU_HOOK_PROVIDER_WASM)["provider"][
            "imports"
        ]
    )
    float_sum = {
        "module": "env",
        "name": "float_sum",
        "params": ["i64", "i64"],
        "results": ["i64"],
    }
    if mutation == "append":
        imports.append(float_sum)
    else:
        imports[0] = float_sum

    with pytest.raises(ValueError, match="pinned raw Hook ABI"):
        _validate_native_abi(imports)


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
      if (member === "[Symbol.iterator]"
            ? typeof receiver[Symbol.iterator] !== "function"
            : kind === "function" ? typeof receiver[member] !== "function"
            : kind === "optional-value" ? false
                                        : !(member in receiver))
        failures.push(`prototype ${{name}}.${{member}}: expected ${{kind}}`);
    const prototypeNames = Object.getOwnPropertyNames(prototype);
    if (Object.hasOwn(prototype, Symbol.iterator))
      prototypeNames.push("[Symbol.iterator]");
    compareNames(
      `prototype ${{name}}`,
      prototypeNames.sort(),
      [...profile.own].sort(),
    );
    if (profile.exotic) {{
      compareNames(
        `exotic ${{name}} own`,
        Reflect.ownKeys(receiver).map(String).sort(),
        [...profile.exotic.own].sort(),
      );
      compareNames(
        `exotic ${{name}} enumerable`,
        Object.keys(receiver).sort(),
        [...profile.exotic.enumerable].sort(),
      );
      if (Object.isExtensible(receiver) !== profile.exotic.extensible)
        failures.push(`exotic ${{name}}: extensibility differs`);
      if (Array.isArray(receiver) !== profile.exotic.array)
        failures.push(`exotic ${{name}}: Array.isArray differs`);
    }}
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
    probe = "globalThis.smuggledProviderRoot = {};\n" + _surface_probe_javascript(
        surface
    )
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
