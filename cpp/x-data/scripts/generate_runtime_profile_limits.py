#!/usr/bin/env python3
"""Generate provider-static constants from the authoritative runtime profile."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path


LIMITS = (
    "serialized_object_max_bytes",
    "serialized_object_max_fields",
    "serialized_object_max_scopes",
    "serialized_object_max_depth",
)
SCHEMA = "xahau.quickjs.runtime-profile-source.v1"
UINT32_MAX = (1 << 32) - 1
PROFILE_CODES = (
    ("none", "xfl_arithmetic_profile_none"),
    ("xahauFloatV1", "xfl_arithmetic_profile_xahau_float_v1"),
    ("nearestEvenV1", "xfl_arithmetic_profile_nearest_even_v1"),
)
VALIDATION_UINT32_FIELDS = (
    "layout_version",
    "main_bit",
    "callback_bit",
    "entry_mask",
    "reserved_mask",
    "profile_mask",
    "profile_shift",
    "version_mask",
    "version_shift",
)
VALIDATION_FAILURE_SENTINEL = -1
MEMBER_PATH = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\.[A-Za-z_][A-Za-z0-9_]*")


def _uint32(value: object, description: str, *, positive: bool = False) -> int:
    minimum = 1 if positive else 0
    if (
        not isinstance(value, int)
        or isinstance(value, bool)
        or value < minimum
        or value > UINT32_MAX
    ):
        qualification = "positive " if positive else ""
        raise ValueError(f"{description} must be a {qualification}uint32")
    return value


def _cpp_identifier(member: str) -> str:
    separated = re.sub(r"(.)([A-Z][a-z]+)", r"\1_\2", member.replace(".", "_"))
    return re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", separated).lower()


def _profile_document(source: bytes) -> tuple[dict[str, object], dict[str, object]]:
    document = json.loads(source)
    if not isinstance(document, dict) or document.get("schema") != SCHEMA:
        raise ValueError(f"runtime profile must use schema {SCHEMA!r}")
    artifact = document.get("artifact")
    if not isinstance(artifact, dict):
        raise ValueError("runtime profile has no artifact object")
    return document, artifact


def profile_implementations(source: bytes) -> dict[str, tuple[str, ...]]:
    """Return the one checked profile-to-operation capability table."""
    _, artifact = _profile_document(source)
    implementations = artifact.get("xfl_arithmetic_profile_implementations")
    expected_profiles = [source_name for source_name, _ in PROFILE_CODES]
    if not isinstance(implementations, dict):
        raise ValueError("runtime profile has no XFL arithmetic implementation set")
    if set(implementations) != set(expected_profiles):
        raise ValueError(
            "runtime profile XFL arithmetic implementation profiles differ"
        )

    selected: dict[str, tuple[str, ...]] = {}
    for profile_name in expected_profiles:
        members = implementations[profile_name]
        if not isinstance(members, list) or any(
            not isinstance(member, str) or MEMBER_PATH.fullmatch(member) is None
            for member in members
        ):
            raise ValueError(
                f"runtime profile XFL arithmetic implementations for {profile_name!r} "
                "must be member paths"
            )
        if members != sorted(set(members)):
            raise ValueError(
                f"runtime profile XFL arithmetic implementations for {profile_name!r} "
                "must be unique and sorted"
            )
        selected[profile_name] = tuple(members)
    if selected["none"]:
        raise ValueError("runtime profile 'none' cannot implement XFL arithmetic")
    return selected


def profile_constants(source: bytes) -> dict[str, int]:
    document, artifact = _profile_document(source)
    selected: dict[str, int] = {
        "xqjs_envelope_version": _uint32(
            artifact.get("envelope_version"),
            "runtime profile XQJS envelope version",
            positive=True,
        )
    }
    if selected["xqjs_envelope_version"] != 1:
        raise ValueError("runtime profile XQJS envelope version must be exactly 1")
    codes = artifact.get("xfl_arithmetic_profile_codes")
    if not isinstance(codes, dict):
        raise ValueError("runtime profile has no XFL arithmetic profile codes")
    if set(codes) != {source_name for source_name, _ in PROFILE_CODES}:
        raise ValueError("runtime profile XFL arithmetic profile code names differ")

    for source_name, constant_name in PROFILE_CODES:
        selected[constant_name] = _uint32(
            codes[source_name],
            f"runtime profile XFL arithmetic profile code {source_name!r}",
        )
    if [selected[name] for _, name in PROFILE_CODES] != [0, 1, 2]:
        raise ValueError("runtime profile XFL arithmetic profile codes must be 0/1/2")
    profile_implementations(source)

    provider = document.get("provider")
    if not isinstance(provider, dict):
        raise ValueError("runtime profile has no provider object")
    validation = provider.get("module_validation_result")
    if not isinstance(validation, dict):
        raise ValueError("runtime profile has no module-validation result layout")
    expected_validation_fields = {
        *VALIDATION_UINT32_FIELDS,
        "failure_sentinel",
    }
    if set(validation) != expected_validation_fields:
        raise ValueError("runtime profile module-validation result fields differ")
    for name in VALIDATION_UINT32_FIELDS:
        selected[f"module_validation_{name}"] = _uint32(
            validation[name], f"module-validation result {name!r}"
        )
    if validation["failure_sentinel"] != VALIDATION_FAILURE_SENTINEL:
        raise ValueError("module-validation failure sentinel must be exactly -1")
    selected["module_validation_failure_sentinel"] = VALIDATION_FAILURE_SENTINEL

    expected_validation = {
        "module_validation_layout_version": 1,
        "module_validation_main_bit": 0x00000001,
        "module_validation_callback_bit": 0x00000002,
        "module_validation_entry_mask": 0x00000003,
        "module_validation_reserved_mask": 0x800000FC,
        "module_validation_profile_mask": 0x00FFFF00,
        "module_validation_profile_shift": 8,
        "module_validation_version_mask": 0x7F000000,
        "module_validation_version_shift": 24,
    }
    if any(selected[name] != value for name, value in expected_validation.items()):
        raise ValueError(
            "runtime profile module-validation result layout differs from v1"
        )

    limits = document.get("limits")
    if not isinstance(limits, dict):
        raise ValueError("runtime profile has no limits object")
    for name in LIMITS:
        selected[name] = _uint32(
            limits.get(name), f"runtime profile limit {name!r}", positive=True
        )
    return selected


def render(source: bytes, source_name: str) -> str:
    values = profile_constants(source)
    implementations = profile_implementations(source)
    digest = hashlib.sha256(source).hexdigest()
    limit_constants = "\n".join(
        f"inline constexpr std::uint32_t {name} = {values[name]}u;" for name in LIMITS
    )
    rendered_profile_constants = "\n".join(
        f"inline constexpr std::uint32_t {name} = {values[name]}u;"
        for _, name in PROFILE_CODES
    )
    rendered_profile_constants = (
        "inline constexpr std::uint32_t xqjs_envelope_version = "
        f"{values['xqjs_envelope_version']}u;\n" + rendered_profile_constants
    )
    validation_constants = "\n".join(
        f"inline constexpr std::uint32_t module_validation_{name} = "
        f"{values[f'module_validation_{name}']}u;"
        for name in VALIDATION_UINT32_FIELDS
    )
    operations = sorted(
        {member for members in implementations.values() for member in members}
    )
    cpp_names: dict[str, str] = {}
    for member in operations:
        identifier = _cpp_identifier(member)
        if identifier in cpp_names.values():
            raise ValueError("XFL arithmetic member paths collide as C++ identifiers")
        cpp_names[member] = identifier
    operation_entries = "\n".join(
        f"    {cpp_names[member]} = {index}u,"
        for index, member in enumerate(operations)
    )
    implementation_rows: list[str] = []
    for profile_name, constant_name in PROFILE_CODES:
        members = implementations[profile_name]
        if not members:
            continue
        predicates = " ||\n            ".join(
            "operation == xfl_arithmetic_operation::" + cpp_names[member]
            for member in members
        )
        implementation_rows.append(
            f"    if (profile_code == {constant_name})\n        return {predicates};"
        )
    implementation_body = "\n".join(implementation_rows)
    return f"""// Auto-generated file. DO NOT EDIT.
// Generated by scripts/generate_runtime_profile_limits.py.
// Source: {source_name}
// SHA-256: {digest}

#pragma once

#include <cstdint>

namespace catl::xdata::xahau_profile_limits {{

{limit_constants}

}} // namespace catl::xdata::xahau_profile_limits

namespace catl::xdata::xahau_profile {{

{rendered_profile_constants}

enum class xfl_arithmetic_operation : std::uint8_t {{
{operation_entries}
}};

[[nodiscard]] inline constexpr bool
xfl_arithmetic_profile_implements(
    std::uint32_t profile_code,
    xfl_arithmetic_operation operation) noexcept
{{
{implementation_body}
    return false;
}}

inline constexpr std::int32_t module_validation_failure_sentinel = -1;
{validation_constants}

}} // namespace catl::xdata::xahau_profile
"""


def render_python(source: bytes, source_name: str) -> str:
    values = profile_constants(source)
    implementations = profile_implementations(source)
    digest = hashlib.sha256(source).hexdigest()
    assignments = [
        f"XQJS_ENVELOPE_VERSION = {values['xqjs_envelope_version']}",
        f"XFL_ARITHMETIC_PROFILE_NONE = {values['xfl_arithmetic_profile_none']}",
        "XFL_ARITHMETIC_PROFILE_XAHAU_FLOAT_V1 = "
        f"{values['xfl_arithmetic_profile_xahau_float_v1']}",
        "XFL_ARITHMETIC_PROFILE_NEAREST_EVEN_V1 = "
        f"{values['xfl_arithmetic_profile_nearest_even_v1']}",
        "XFL_ARITHMETIC_PROFILE_IMPLEMENTATIONS = " + repr(implementations),
        "MODULE_VALIDATION_FAILURE_SENTINEL = -1",
    ]
    assignments.extend(
        f"MODULE_VALIDATION_{name.upper()} = {values[f'module_validation_{name}']}"
        for name in VALIDATION_UINT32_FIELDS
    )
    body = "\n".join(assignments)
    return f"""# Auto-generated file. DO NOT EDIT.
# Generated by cpp/x-data/scripts/generate_runtime_profile_limits.py.
# Source: {source_name}
# SHA-256: {digest}

{body}
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--python-output", type=Path)
    args = parser.parse_args()
    source = args.input.read_bytes()
    args.output.write_text(render(source, args.input.name))
    if args.python_output is not None:
        args.python_output.write_text(render_python(source, args.input.name))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
