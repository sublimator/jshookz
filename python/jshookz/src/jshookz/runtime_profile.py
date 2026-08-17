"""Canonical QuickJS Hook runtime-profile manifests and lock verification."""

from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from wasmtime import Engine, FuncType, MemoryType, Module

from .paths import (
    SOURCE_CHECKOUT,
    XAHAU_V1_HOOKS_API_DECLARATIONS,
    XAHAU_V1_JAVASCRIPT_SURFACE,
)
from .schema import HOOK_API


SOURCE_SCHEMA = "xahau.quickjs.runtime-profile-source.v1"
LOCK_SCHEMA = "xahau.quickjs.runtime-profile-lock.v1"
SURFACE_SCHEMA = "jshookz.javascript-surface.v1"


def canonical_json_bytes(value: Any) -> bytes:
    """Return the byte-for-byte JSON representation used for profile IDs."""
    return (
        json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
        + "\n"
    ).encode("ascii")


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _load_object(path: str | Path) -> dict[str, Any]:
    value = json.loads(Path(path).read_text())
    if not isinstance(value, dict):
        raise ValueError(f"{path}: expected a JSON object")
    return value


def _wasm_surface(wasm_path: str | Path) -> dict[str, Any]:
    path = Path(wasm_path)
    wasm = path.read_bytes()
    module = Module.from_file(Engine(), str(path))

    imports = []
    for item in module.imports:
        if not isinstance(item.type, FuncType):
            raise ValueError(
                f"provider import {item.module}.{item.name} is not a function"
            )
        imports.append(
            {
                "module": item.module,
                "name": item.name,
                "params": [str(value) for value in item.type.params],
                "results": [str(value) for value in item.type.results],
            }
        )
    imports.sort(key=lambda item: (item["module"], item["name"]))

    exports = []
    for item in module.exports:
        item_type = item.type
        if isinstance(item_type, FuncType):
            surface = {
                "kind": "function",
                "params": [str(value) for value in item_type.params],
                "results": [str(value) for value in item_type.results],
            }
        elif isinstance(item_type, MemoryType):
            surface = {
                "kind": "memory",
                "minimum_pages": item_type.limits.min,
                "maximum_pages": item_type.limits.max,
                "memory64": item_type.is_64,
                "shared": item_type.is_shared,
            }
        else:
            raise ValueError(
                f"provider export {item.name} has unsupported type "
                f"{type(item_type).__name__}"
            )
        exports.append({"name": item.name, **surface})
    exports.sort(key=lambda item: item["name"])
    return {
        "sha256": _sha256(wasm),
        "size": len(wasm),
        "imports": imports,
        "exports": exports,
    }


def _identity(domain: str, manifest: Any) -> str:
    return _sha256(canonical_json_bytes({"domain": domain, "manifest": manifest}))


def _selected_artifact(
    document_path: str | Path,
    selected: Any,
    *,
    override: str | Path | None,
    packaged_relative: str,
    packaged_path: Path,
    label: str,
) -> Path:
    """Resolve a profile-selected repository path without ignoring its policy."""
    if not isinstance(selected, str) or not selected:
        raise ValueError(f"runtime profile must select a {label} path")
    relative = Path(selected)
    if relative.is_absolute() or ".." in relative.parts:
        raise ValueError(f"runtime profile {label} path must be repository-relative")
    if override is not None:
        return Path(override)

    document = Path(document_path).resolve()
    repo = SOURCE_CHECKOUT.resolve() if SOURCE_CHECKOUT is not None else None
    for ancestor in (document.parent, *document.parents):
        candidate = ancestor / relative
        if candidate.is_file():
            return candidate
        if repo is not None and ancestor == repo:
            break
    # Canonical package data remains usable when a checked lock is copied away
    # from its source checkout or loaded from an installed wheel.
    if selected == packaged_relative and packaged_path.is_file():
        return packaged_path
    raise ValueError(f"selected runtime-profile {label} does not exist: {selected}")


def _selected_javascript_artifacts(
    source: dict[str, Any],
    document_path: str | Path,
    surface_path: str | Path | None,
    declaration_path: str | Path | None,
) -> tuple[Path, Path]:
    policy = source.get("javascript_surface")
    if not isinstance(policy, dict) or policy.get("schema") != SURFACE_SCHEMA:
        raise ValueError(f"runtime-profile source must select {SURFACE_SCHEMA!r}")
    surface = _selected_artifact(
        document_path,
        policy.get("manifest"),
        override=surface_path,
        packaged_relative=(
            "python/jshookz/src/jshookz/types/xahau-quickjs-v1.surface.json"
        ),
        packaged_path=XAHAU_V1_JAVASCRIPT_SURFACE,
        label="JavaScript surface manifest",
    )
    declaration = _selected_artifact(
        document_path,
        policy.get("declaration"),
        override=declaration_path,
        packaged_relative=(
            "python/jshookz/src/jshookz/types/xahau-quickjs-v1.d.ts"
        ),
        packaged_path=XAHAU_V1_HOOKS_API_DECLARATIONS,
        label="JavaScript declaration",
    )
    return surface, declaration


def _javascript_surface(
    source: dict[str, Any],
    surface_path: str | Path,
    declaration_path: str | Path,
) -> dict[str, str]:
    policy = source.get("javascript_surface")
    if not isinstance(policy, dict) or policy.get("schema") != SURFACE_SCHEMA:
        raise ValueError(
            f"runtime-profile source must select {SURFACE_SCHEMA!r}"
        )

    surface_bytes = Path(surface_path).read_bytes()
    surface = json.loads(surface_bytes)
    if not isinstance(surface, dict):
        raise ValueError("JavaScript surface must be a JSON object")
    if surface.get("schema") != SURFACE_SCHEMA:
        raise ValueError(
            f"unsupported JavaScript surface schema: {surface.get('schema')!r}"
        )

    declaration_bytes = Path(declaration_path).read_bytes()
    declaration = surface.get("declaration")
    if not isinstance(declaration, dict):
        raise ValueError("JavaScript surface has no declaration identity")
    declaration_sha256 = _sha256(declaration_bytes)
    if declaration.get("sha256") != declaration_sha256:
        raise ValueError(
            "JavaScript surface declaration identity does not match the selected "
            "declaration"
        )
    if declaration.get("path") != policy.get("declaration"):
        raise ValueError(
            "JavaScript surface declaration path differs from the runtime profile"
        )

    return {
        "schema": SURFACE_SCHEMA,
        "manifest": policy["manifest"],
        "sha256": _sha256(surface_bytes),
        "declaration": policy["declaration"],
        "declaration_sha256": declaration_sha256,
    }


def _validate_provider_policy(source: dict[str, Any], provider: dict[str, Any]) -> None:
    """Apply the source manifest's provider allow-list and export contract."""
    policy = source.get("provider")
    if not isinstance(policy, dict):
        raise ValueError("runtime-profile source has no provider policy")

    expected_imports = sorted(
        policy.get("imports", []), key=lambda item: (item["module"], item["name"])
    )
    if provider["imports"] != expected_imports:
        raise ValueError(
            "provider import surface differs from the profile:\n"
            f"expected {expected_imports!r}\nactual   {provider['imports']!r}"
        )

    forbidden_modules = set(policy.get("forbidden_import_modules", []))
    forbidden = [
        item for item in provider["imports"] if item["module"] in forbidden_modules
    ]
    if forbidden:
        raise ValueError(f"provider retains forbidden imports: {forbidden!r}")

    expected_exports = policy.get("required_exports", [])
    if not isinstance(expected_exports, list) or not all(
        isinstance(item, dict) and isinstance(item.get("name"), str)
        for item in expected_exports
    ):
        raise ValueError("provider required_exports must be a list of typed exports")
    actual_exports = {item["name"]: item for item in provider["exports"]}
    for expected in expected_exports:
        actual = actual_exports.get(expected["name"])
        if actual is None:
            raise ValueError(f"provider is missing required export: {expected['name']}")
        if actual != expected:
            raise ValueError(
                f"provider export {expected['name']} differs from the profile:\n"
                f"expected {expected!r}\nactual   {actual!r}"
            )


def build_runtime_profile_lock(
    source_path: str | Path,
    wasm_path: str | Path,
    surface_path: str | Path | None = None,
    declaration_path: str | Path | None = None,
) -> dict[str, Any]:
    """Resolve a source manifest against an exact provider WASM binary."""
    source = _load_object(source_path)
    if source.get("schema") != SOURCE_SCHEMA:
        raise ValueError(
            f"unsupported runtime-profile source schema: {source.get('schema')!r}"
        )

    provider = _wasm_surface(wasm_path)
    _validate_provider_policy(source, provider)
    selected_surface, selected_declaration = _selected_javascript_artifacts(
        source, source_path, surface_path, declaration_path
    )
    javascript_surface = _javascript_surface(
        source, selected_surface, selected_declaration
    )

    bytecode_abi = source.get("bytecode_abi")
    if not isinstance(bytecode_abi, dict):
        raise ValueError("runtime-profile source has no bytecode_abi object")
    bytecode_abi_id = _identity("xahau.quickjs.bytecode-abi.v1", bytecode_abi)

    identity_manifest = {
        "source": source,
        "bytecode_abi_id": bytecode_abi_id,
        "javascript_surface": javascript_surface,
        "provider_wasm_sha256": provider["sha256"],
        "provider_imports": provider["imports"],
        "provider_exports": provider["exports"],
    }
    runtime_profile_id = _identity(
        "xahau.quickjs.runtime-profile.v1", identity_manifest
    )
    return {
        "schema": LOCK_SCHEMA,
        "bytecode_abi_id": bytecode_abi_id,
        "runtime_profile_id": runtime_profile_id,
        "source": source,
        "javascript_surface": javascript_surface,
        "provider": provider,
    }


@dataclass(frozen=True)
class RuntimeProfileLock:
    bytecode_abi_id: bytes
    runtime_profile_id: bytes
    data: dict[str, Any]


@dataclass(frozen=True)
class ProfileExecutionLimits:
    """Closed execution-budget projection from a runtime-profile lock."""

    quickjs_heap_bytes: int
    quickjs_stack_bytes: int
    initialization_fuel: int
    invocation_fuel: int
    host_work_budget: int
    host_work_base_per_call: int
    host_work_per_addressed_byte: int
    host_work_addressed_length_indices: tuple[tuple[str, tuple[int, ...]], ...]

    def addressed_length_indices(self, function_name: str) -> tuple[int, ...]:
        for name, indices in self.host_work_addressed_length_indices:
            if name == function_name:
                return indices
        return ()


def profile_execution_limits(lock: RuntimeProfileLock) -> ProfileExecutionLimits:
    """Validate and project the executable limits from a verified lock."""
    try:
        limits = lock.data["source"]["limits"]
    except (KeyError, TypeError) as error:
        raise ValueError("runtime-profile lock has no execution limits") from error
    if not isinstance(limits, dict):
        raise ValueError("runtime-profile execution limits must be an object")
    if limits.get("host_work_meter") != "base-plus-addressed-byte-v1":
        raise ValueError("unsupported runtime-profile host-work meter")

    integer_names = (
        "quickjs_heap_bytes",
        "quickjs_stack_bytes",
        "wasmtime_fuel_per_initialization",
        "wasmtime_fuel_per_invocation",
        "host_work_budget",
        "host_work_base_per_call",
        "host_work_per_addressed_byte",
    )
    values: dict[str, int] = {}
    for name in integer_names:
        value = limits.get(name)
        if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
            raise ValueError(
                f"runtime-profile execution limit {name!r} must be positive"
            )
        values[name] = value

    raw_indices = limits.get("host_work_addressed_length_indices")
    if not isinstance(raw_indices, dict):
        raise ValueError(
            "runtime-profile host-work addressed-length map must be an object"
        )
    addressed: list[tuple[str, tuple[int, ...]]] = []
    function_arities = {
        function.name: len(function.wasm_params) for function in HOOK_API.functions
    }
    for name, indices in sorted(raw_indices.items()):
        if not isinstance(name, str) or not name:
            raise ValueError("host-work function names must be non-empty strings")
        if name not in function_arities:
            raise ValueError(f"host-work function {name!r} is not in the raw ABI")
        if (
            not isinstance(indices, list)
            or not indices
            or any(
                not isinstance(index, int)
                or isinstance(index, bool)
                or index < 0
                for index in indices
            )
            or len(indices) != len(set(indices))
        ):
            raise ValueError(
                f"host-work length indices for {name!r} must be unique "
                "non-negative integers"
            )
        if any(index >= function_arities[name] for index in indices):
            raise ValueError(
                f"host-work length index is outside raw function {name!r}"
            )
        addressed.append((name, tuple(indices)))

    return ProfileExecutionLimits(
        quickjs_heap_bytes=values["quickjs_heap_bytes"],
        quickjs_stack_bytes=values["quickjs_stack_bytes"],
        initialization_fuel=values["wasmtime_fuel_per_initialization"],
        invocation_fuel=values["wasmtime_fuel_per_invocation"],
        host_work_budget=values["host_work_budget"],
        host_work_base_per_call=values["host_work_base_per_call"],
        host_work_per_addressed_byte=values["host_work_per_addressed_byte"],
        host_work_addressed_length_indices=tuple(addressed),
    )


def load_runtime_profile_lock(path: str | Path) -> RuntimeProfileLock:
    data = _load_object(path)
    if data.get("schema") != LOCK_SCHEMA:
        raise ValueError(
            f"unsupported runtime-profile lock schema: {data.get('schema')!r}"
        )
    try:
        bytecode_abi_id = bytes.fromhex(data["bytecode_abi_id"])
        runtime_profile_id = bytes.fromhex(data["runtime_profile_id"])
    except (KeyError, TypeError, ValueError) as error:
        raise ValueError("runtime-profile lock contains invalid identities") from error
    if len(bytecode_abi_id) != 32 or len(runtime_profile_id) != 32:
        raise ValueError("runtime-profile identities must each be 32 bytes")
    return RuntimeProfileLock(bytecode_abi_id, runtime_profile_id, data)


def verify_runtime_profile_lock(
    lock_path: str | Path,
    wasm_path: str | Path,
    surface_path: str | Path | None = None,
    declaration_path: str | Path | None = None,
) -> RuntimeProfileLock:
    """Verify the lock and exact provider, returning its deployment IDs."""
    lock = load_runtime_profile_lock(lock_path)
    source = lock.data.get("source")
    if not isinstance(source, dict):
        raise ValueError("runtime-profile lock has no source manifest")

    # Rebuild without relying on an ambient source path.
    provider = _wasm_surface(wasm_path)
    expected_provider = lock.data.get("provider")
    if provider != expected_provider:
        raise ValueError(
            "provider binary/surface does not match the runtime-profile lock"
        )
    _validate_provider_policy(source, provider)
    selected_surface, selected_declaration = _selected_javascript_artifacts(
        source, lock_path, surface_path, declaration_path
    )
    javascript_surface = _javascript_surface(
        source, selected_surface, selected_declaration
    )
    if javascript_surface != lock.data.get("javascript_surface"):
        raise ValueError(
            "JavaScript surface does not match the runtime-profile lock"
        )

    bytecode_abi_id = _identity("xahau.quickjs.bytecode-abi.v1", source["bytecode_abi"])
    identity_manifest = {
        "source": source,
        "bytecode_abi_id": bytecode_abi_id,
        "javascript_surface": javascript_surface,
        "provider_wasm_sha256": provider["sha256"],
        "provider_imports": provider["imports"],
        "provider_exports": provider["exports"],
    }
    runtime_profile_id = _identity(
        "xahau.quickjs.runtime-profile.v1", identity_manifest
    )
    if bytecode_abi_id != lock.data.get("bytecode_abi_id"):
        raise ValueError("bytecode ABI identity does not match the lock contents")
    if runtime_profile_id != lock.data.get("runtime_profile_id"):
        raise ValueError("runtime profile identity does not match the lock contents")
    return lock
