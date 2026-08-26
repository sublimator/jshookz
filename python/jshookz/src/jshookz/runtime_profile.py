"""Canonical QuickJS Hook runtime-profile manifests and lock verification."""

from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from wasmtime import Engine, FuncType, MemoryType, Module

from . import _runtime_profile_constants as generated
from .paths import (
    SOURCE_CHECKOUT,
    XAHAU_V1_HOOKS_API_DECLARATIONS,
    XAHAU_V1_JAVASCRIPT_SURFACE,
)
from .schema import HOOK_API


SOURCE_SCHEMA = "xahau.quickjs.runtime-profile-source.v1"
LOCK_SCHEMA = "xahau.quickjs.runtime-profile-lock.v1"
SURFACE_SCHEMA = "jshookz.javascript-surface.v1"
_OBJECT_LIMIT_NAMES = (
    "serialized_object_max_bytes",
    "serialized_object_max_fields",
    "serialized_object_max_scopes",
    "serialized_object_max_depth",
)
_UINT32_MAX = (1 << 32) - 1


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


def _validated_object_limits(source: dict[str, Any]) -> dict[str, int]:
    """Return the checked object limits carried by a profile source."""
    limits = source.get("limits")
    if not isinstance(limits, dict):
        raise ValueError("runtime-profile source has no limits object")

    values: dict[str, int] = {}
    for name in _OBJECT_LIMIT_NAMES:
        value = limits.get(name)
        if (
            not isinstance(value, int)
            or isinstance(value, bool)
            or value <= 0
            or value > _UINT32_MAX
        ):
            raise ValueError(
                f"runtime-profile object limit {name!r} must be a positive uint32"
            )
        values[name] = value
    return values


def _validate_xfl_activation_contract(source: dict[str, Any]) -> None:
    """Join source, generated Python constants, and provider validation ABI."""
    expected_codes = {
        "none": generated.XFL_ARITHMETIC_PROFILE_NONE,
        "xahauFloatV1": generated.XFL_ARITHMETIC_PROFILE_XAHAU_FLOAT_V1,
        "nearestEvenV1": generated.XFL_ARITHMETIC_PROFILE_NEAREST_EVEN_V1,
    }
    artifact = source.get("artifact")
    if not isinstance(artifact, dict) or artifact.get(
        "xfl_arithmetic_profile_codes"
    ) != expected_codes:
        raise ValueError("runtime-profile XFL arithmetic profile codes differ")

    expected_validation = {
        "layout_version": generated.MODULE_VALIDATION_LAYOUT_VERSION,
        "failure_sentinel": generated.MODULE_VALIDATION_FAILURE_SENTINEL,
        "main_bit": generated.MODULE_VALIDATION_MAIN_BIT,
        "callback_bit": generated.MODULE_VALIDATION_CALLBACK_BIT,
        "entry_mask": generated.MODULE_VALIDATION_ENTRY_MASK,
        "reserved_mask": generated.MODULE_VALIDATION_RESERVED_MASK,
        "profile_mask": generated.MODULE_VALIDATION_PROFILE_MASK,
        "profile_shift": generated.MODULE_VALIDATION_PROFILE_SHIFT,
        "version_mask": generated.MODULE_VALIDATION_VERSION_MASK,
        "version_shift": generated.MODULE_VALIDATION_VERSION_SHIFT,
    }
    provider = source.get("provider")
    if not isinstance(provider, dict) or provider.get(
        "module_validation_result"
    ) != expected_validation:
        raise ValueError("runtime-profile module-validation result layout differs")


def _read_leb128(data: bytes, position: int, *, signed: bool) -> tuple[int, int]:
    """Read one bounded WebAssembly integer from *data*."""
    value = 0
    shift = 0
    while True:
        if position >= len(data):
            raise ValueError("truncated WebAssembly LEB128")
        byte = data[position]
        position += 1
        value |= (byte & 0x7F) << shift
        shift += 7
        if byte < 0x80:
            if signed and (byte & 0x40):
                value |= -(1 << shift)
            return value, position
        if shift >= 70:
            raise ValueError("oversized WebAssembly LEB128")


def _wasm_stack_pointer_initial(wasm: bytes) -> int:
    """Return the sole mutable i32 global's initial value."""
    if wasm[:8] != b"\0asm\x01\0\0\0":
        raise ValueError("provider is not a WebAssembly 1 binary")

    position = 8
    mutable_i32: list[int] = []
    while position < len(wasm):
        section_id = wasm[position]
        position += 1
        section_size, position = _read_leb128(wasm, position, signed=False)
        section_end = position + section_size
        if section_end > len(wasm):
            raise ValueError("truncated WebAssembly section")
        if section_id != 6:
            position = section_end
            continue

        count, position = _read_leb128(wasm, position, signed=False)
        for _ in range(count):
            if position + 3 > section_end:
                raise ValueError("truncated WebAssembly global")
            value_type = wasm[position]
            mutable = wasm[position + 1]
            opcode = wasm[position + 2]
            position += 3
            if opcode == 0x41:  # i32.const
                initial, position = _read_leb128(wasm, position, signed=True)
            elif opcode == 0x42:  # i64.const
                _, position = _read_leb128(wasm, position, signed=True)
                initial = 0
            elif opcode == 0x43:  # f32.const
                position += 4
                initial = 0
            elif opcode == 0x44:  # f64.const
                position += 8
                initial = 0
            elif opcode in (0x23, 0xD2):  # global.get / ref.func
                _, position = _read_leb128(wasm, position, signed=False)
                initial = 0
            elif opcode == 0xD0:  # ref.null
                position += 1
                initial = 0
            else:
                raise ValueError(
                    f"unsupported WebAssembly global initializer 0x{opcode:02x}"
                )
            if position >= section_end or wasm[position] != 0x0B:
                raise ValueError("WebAssembly global initializer has no end opcode")
            position += 1
            if value_type == 0x7F and mutable == 1:
                if opcode != 0x41:
                    raise ValueError(
                        "mutable i32 global is not initialized by i32.const"
                    )
                mutable_i32.append(initial)
        if position != section_end:
            raise ValueError("WebAssembly global section has trailing bytes")
        break

    if len(mutable_i32) != 1:
        raise ValueError(
            "provider must contain exactly one mutable i32 stack-pointer global"
        )
    return mutable_i32[0]


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
    memories = [item for item in exports if item["kind"] == "memory"]
    if len(memories) != 1 or memories[0]["maximum_pages"] is None:
        raise ValueError("provider must export exactly one bounded memory")
    return {
        "sha256": _sha256(wasm),
        "size": len(wasm),
        "imports": imports,
        "exports": exports,
        "build": {
            "wasm_stack_bytes": _wasm_stack_pointer_initial(wasm),
            "wasm_memory_max_bytes": memories[0]["maximum_pages"] * 65536,
        },
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
        packaged_relative=("python/jshookz/src/jshookz/types/xahau-quickjs-v1.d.ts"),
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
        raise ValueError(f"runtime-profile source must select {SURFACE_SCHEMA!r}")

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
    """Apply the source manifest's exact import and export contract."""
    policy = source.get("provider")
    if not isinstance(policy, dict):
        raise ValueError("runtime-profile source has no provider policy")

    expected_build = policy.get("build")
    actual_build = provider.get("build")
    if not isinstance(expected_build, dict) or not isinstance(actual_build, dict):
        raise ValueError("runtime-profile provider has no checked build metadata")
    for name in ("wasm_stack_bytes", "wasm_memory_max_bytes"):
        expected = expected_build.get(name)
        actual = actual_build.get(name)
        if expected != actual:
            raise ValueError(
                f"provider build {name} differs from the artifact: "
                f"expected {expected!r}, actual {actual!r}"
            )

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

    expected_exports = policy.get("allowed_exports")
    if not isinstance(expected_exports, list) or not all(
        isinstance(item, dict)
        and isinstance(item.get("name"), str)
        and bool(item["name"])
        for item in expected_exports
    ):
        raise ValueError("provider allowed_exports must be a list of typed exports")

    expected_names = [item["name"] for item in expected_exports]
    if len(expected_names) != len(set(expected_names)):
        raise ValueError("provider allowed_exports contains duplicate names")
    if expected_names != sorted(expected_names):
        raise ValueError("provider allowed_exports must be normalized by export name")

    provider_exports = provider.get("exports")
    if not isinstance(provider_exports, list) or not all(
        isinstance(item, dict)
        and isinstance(item.get("name"), str)
        and bool(item["name"])
        for item in provider_exports
    ):
        raise ValueError("provider exports must be a list of typed exports")
    actual_names = [item["name"] for item in provider_exports]
    if len(actual_names) != len(set(actual_names)):
        raise ValueError("provider exports contain duplicate names")

    expected_name_set = set(expected_names)
    actual_name_set = set(actual_names)
    missing = sorted(expected_name_set - actual_name_set)
    if missing:
        raise ValueError(f"provider is missing allowed exports: {missing!r}")
    extra = sorted(actual_name_set - expected_name_set)
    if extra:
        raise ValueError(f"provider has exports outside allowed_exports: {extra!r}")

    actual_exports = {item["name"]: item for item in provider_exports}
    for expected in expected_exports:
        actual = actual_exports[expected["name"]]
        if actual != expected:
            raise ValueError(
                f"provider export {expected['name']} differs from the profile:\n"
                f"expected {expected!r}\nactual   {actual!r}"
            )

    if provider_exports != expected_exports:
        raise ValueError(
            "provider export order differs from normalized allowed_exports:\n"
            f"expected {expected_exports!r}\nactual   {provider_exports!r}"
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
    _validated_object_limits(source)
    _validate_xfl_activation_contract(source)

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
    serialized_object_max_bytes: int
    serialized_object_max_fields: int
    serialized_object_max_scopes: int
    serialized_object_max_depth: int
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
        source = lock.data["source"]
        limits = source["limits"]
    except (KeyError, TypeError) as error:
        raise ValueError("runtime-profile lock has no execution limits") from error
    object_limits = _validated_object_limits(source)
    if not isinstance(limits, dict):
        raise ValueError("runtime-profile execution limits must be an object")
    if limits.get("host_work_meter") != "base-plus-addressed-byte-v1":
        raise ValueError("unsupported runtime-profile host-work meter")

    integer_names = (
        "quickjs_heap_bytes",
        "quickjs_stack_bytes",
        *_OBJECT_LIMIT_NAMES,
        "wasmtime_fuel_per_initialization",
        "wasmtime_fuel_per_invocation",
        "host_work_budget",
        "host_work_base_per_call",
        "host_work_per_addressed_byte",
    )
    values: dict[str, int] = {}
    for name in integer_names:
        if name in object_limits:
            values[name] = object_limits[name]
            continue
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
                not isinstance(index, int) or isinstance(index, bool) or index < 0
                for index in indices
            )
            or len(indices) != len(set(indices))
        ):
            raise ValueError(
                f"host-work length indices for {name!r} must be unique "
                "non-negative integers"
            )
        if any(index >= function_arities[name] for index in indices):
            raise ValueError(f"host-work length index is outside raw function {name!r}")
        addressed.append((name, tuple(indices)))

    return ProfileExecutionLimits(
        quickjs_heap_bytes=values["quickjs_heap_bytes"],
        quickjs_stack_bytes=values["quickjs_stack_bytes"],
        serialized_object_max_bytes=values["serialized_object_max_bytes"],
        serialized_object_max_fields=values["serialized_object_max_fields"],
        serialized_object_max_scopes=values["serialized_object_max_scopes"],
        serialized_object_max_depth=values["serialized_object_max_depth"],
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

    lock_document = Path(lock_path)
    lock_suffix = ".lock.json"
    if lock_document.name.endswith(lock_suffix):
        companion_source = lock_document.with_name(
            lock_document.name[: -len(lock_suffix)] + ".source.json"
        )
        if companion_source.is_file() and _load_object(companion_source) != source:
            raise ValueError(
                "runtime-profile lock source does not match its companion source; "
                "rebuild the provider bundle"
            )

    _validated_object_limits(source)
    _validate_xfl_activation_contract(source)

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
        raise ValueError("JavaScript surface does not match the runtime-profile lock")

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
