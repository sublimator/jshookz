#!/usr/bin/env python3
"""Generate provider-static constants from the authoritative runtime profile."""

from __future__ import annotations

import argparse
import hashlib
import json
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


def profile_constants(source: bytes) -> dict[str, int]:
    document = json.loads(source)
    if not isinstance(document, dict) or document.get("schema") != SCHEMA:
        raise ValueError(f"runtime profile must use schema {SCHEMA!r}")
    artifact = document.get("artifact")
    if not isinstance(artifact, dict):
        raise ValueError("runtime profile has no artifact object")
    selected: dict[str, int] = {
        "xqjs_envelope_version": _uint32(
            artifact.get("envelope_version"),
            "runtime profile XQJS envelope version",
            positive=True,
        )
    }
    if selected["xqjs_envelope_version"] != 2:
        raise ValueError("runtime profile XQJS envelope version must be exactly 2")
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
        raise ValueError("runtime profile module-validation result layout differs from v1")

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

inline constexpr std::int32_t module_validation_failure_sentinel = -1;
{validation_constants}

}} // namespace catl::xdata::xahau_profile
"""


def render_python(source: bytes, source_name: str) -> str:
    values = profile_constants(source)
    digest = hashlib.sha256(source).hexdigest()
    assignments = [
        f"XQJS_ENVELOPE_VERSION = {values['xqjs_envelope_version']}",
        f"XFL_ARITHMETIC_PROFILE_NONE = {values['xfl_arithmetic_profile_none']}",
        "XFL_ARITHMETIC_PROFILE_XAHAU_FLOAT_V1 = "
        f"{values['xfl_arithmetic_profile_xahau_float_v1']}",
        "XFL_ARITHMETIC_PROFILE_NEAREST_EVEN_V1 = "
        f"{values['xfl_arithmetic_profile_nearest_even_v1']}",
        "MODULE_VALIDATION_FAILURE_SENTINEL = -1",
    ]
    assignments.extend(
        f"MODULE_VALIDATION_{name.upper()} = "
        f"{values[f'module_validation_{name}']}"
        for name in VALIDATION_UINT32_FIELDS
    )
    body = "\n".join(assignments)
    return f'''# Auto-generated file. DO NOT EDIT.
# Generated by cpp/x-data/scripts/generate_runtime_profile_limits.py.
# Source: {source_name}
# SHA-256: {digest}

{body}
'''


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
