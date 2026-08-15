"""Canonical QuickJS Hook runtime-profile manifests and lock verification."""

from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from wasmtime import Engine, FuncType, MemoryType, Module

from .paths import XAHAU_V1_HOOKS_API_DECLARATIONS, XAHAU_V1_JAVASCRIPT_SURFACE


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
        "sha256": _sha256(surface_bytes),
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
    surface_path: str | Path = XAHAU_V1_JAVASCRIPT_SURFACE,
    declaration_path: str | Path = XAHAU_V1_HOOKS_API_DECLARATIONS,
) -> dict[str, Any]:
    """Resolve a source manifest against an exact provider WASM binary."""
    source = _load_object(source_path)
    if source.get("schema") != SOURCE_SCHEMA:
        raise ValueError(
            f"unsupported runtime-profile source schema: {source.get('schema')!r}"
        )

    provider = _wasm_surface(wasm_path)
    _validate_provider_policy(source, provider)
    javascript_surface = _javascript_surface(
        source, surface_path, declaration_path
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
    surface_path: str | Path = XAHAU_V1_JAVASCRIPT_SURFACE,
    declaration_path: str | Path = XAHAU_V1_HOOKS_API_DECLARATIONS,
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
    javascript_surface = _javascript_surface(
        source, surface_path, declaration_path
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
