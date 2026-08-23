#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# ///
"""
Generate C++ Protocol tables from server_definitions JSON files.

Emits inline constexpr std::array rows for the maps
Protocol::load_from_json_value actually keeps. Safe for WASM: no heap at
static init, no JSON parse at embed load.

Usage:
  python generate_definitions.py \
    --input definitions/xahau_definitions.json \
    --output generated/embedded_xahau_definitions.h \
    --namespace catl::xdata::xahau
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Any

CONSUMED_NAME_CODE_KEYS = (
    "TYPES",
    "LEDGER_ENTRY_TYPES",
    "TRANSACTION_TYPES",
    "TRANSACTION_RESULTS",
    "PERMISSIONS",
)

PROVIDER_WIRE_TYPES = {
    "UInt8": ("uint8", 1),
    "UInt16": ("uint16", 2),
    "UInt32": ("uint32", 4),
    "UInt64": ("uint64", 8),
    "Hash128": ("hash128", 16),
    "Hash160": ("hash160", 20),
    "Hash192": ("hash192", 24),
    "Hash256": ("hash256", 32),
    "Blob": ("blob", 0),
    "AccountID": ("account_id", 0),
    "Amount": ("amount", 0),
    "Currency": ("currency", 20),
    "Issue": ("issue", 0),
    "Number": ("number", 12),
    "PathSet": ("path_set", 0),
    "Vector256": ("vector256", 0),
    "XChainBridge": ("xchain_bridge", 0),
    "STObject": ("st_object", 0),
    "STArray": ("st_array", 0),
}

PROVIDER_FIELD_OVERRIDES = {
    "TransactionType": "transaction_type",
    "TransactionResult": "transaction_result",
}

FIELD_SENTINELS = {"ObjectEndMarker", "ArrayEndMarker"}
NO_ORDINAL = 0xFFFF


def unwrap_definitions(defs: Any) -> dict:
    """Match Protocol::load_from_json_value's top-level {result: …} unwrap."""
    if isinstance(defs, dict) and isinstance(defs.get("result"), dict):
        return defs["result"]
    if not isinstance(defs, dict):
        raise ValueError("Protocol JSON must be an object")
    return defs


def clean_definitions(defs: dict) -> dict:
    """Clean up known issues in server definitions."""

    if "FIELDS" in defs:
        seen_names = set()
        cleaned = []
        for entry in defs["FIELDS"]:
            name = entry[0]
            if name in seen_names:
                print(f"  Removed duplicate field: {name}", file=sys.stderr)
                continue
            seen_names.add(name)
            cleaned.append(entry)
        defs["FIELDS"] = cleaned

    if "features" in defs:
        n = len(defs["features"])
        del defs["features"]
        print(f"  Stripped {n} feature entries (not needed for codec)", file=sys.stderr)

    return defs


def cpp_string(value: str) -> str:
    if "*/" in value:
        raise ValueError(
            "name contains */ and would terminate a C++ comment if ever inlined"
        )
    return json.dumps(value, ensure_ascii=False)


def cpp_bool(value: bool) -> str:
    return "true" if value else "false"


def cpp_blob_part(value: str) -> str:
    """Emit one NUL-terminated, byte-counted protocol name literal."""
    try:
        value.encode("ascii")
    except UnicodeEncodeError as error:
        raise ValueError(f"provider protocol name is not ASCII: {value!r}") from error
    return cpp_string(value)[:-1] + '\\0"'


def field_code(type_code: int, nth: int) -> int:
    return ((type_code & 0xFFFF) << 16) | (nth & 0xFFFF)


def header_size(type_code: int, nth: int) -> int:
    if type_code < 16 and nth < 16:
        return 1
    if type_code >= 16 and nth >= 16:
        return 3
    return 2


def load_materializer_policy(path: Path | None) -> tuple[dict[str, str], str]:
    if path is None:
        return dict(PROVIDER_FIELD_OVERRIDES), "none"
    policy = json.loads(path.read_text(encoding="utf-8"))
    overrides = policy.get("descriptor_overrides")
    if not isinstance(overrides, dict) or not all(
        isinstance(name, str) and isinstance(kind, str)
        for name, kind in overrides.items()
    ):
        raise ValueError(f"{path} has no valid descriptor_overrides object")
    normalized = {
        name: {
            "TransactionType": "transaction_type",
            "TransactionResult": "transaction_result",
        }.get(kind, kind)
        for name, kind in overrides.items()
    }
    if normalized != PROVIDER_FIELD_OVERRIDES:
        raise ValueError(
            "provider field override closure must be exactly "
            "TransactionType/TransactionResult"
        )
    return normalized, hashlib.sha256(path.read_bytes()).hexdigest()


def tables_from_defs(defs: dict) -> dict[str, list[dict[str, Any]]]:
    if "FIELDS" not in defs:
        raise ValueError("Protocol JSON must contain FIELDS array")

    fields: list[dict[str, Any]] = []
    for entry in defs["FIELDS"]:
        if not isinstance(entry, list) or len(entry) != 2:
            raise ValueError("Field definition must be a 2-element array")
        name, meta = entry
        fields.append(
            {
                "name": str(name),
                "type_name": str(meta["type"]),
                "nth": int(meta["nth"]),
                "is_serialized": bool(meta["isSerialized"]),
                "is_signing_field": bool(meta["isSigningField"]),
                "is_vl_encoded": bool(meta["isVLEncoded"]),
            }
        )

    tables: dict[str, list[dict[str, Any]]] = {"FIELDS": fields}
    for key in CONSUMED_NAME_CODE_KEYS:
        obj = defs.get(key)
        if not obj:
            tables[key] = []
            continue
        if not isinstance(obj, dict):
            raise ValueError(f"{key} must be an object")
        tables[key] = [{"name": str(k), "code": int(v)} for k, v in obj.items()]
    return tables


def emit_array(type_name: str, name: str, rows: list[str]) -> str:
    n = len(rows)
    if n == 0:
        return f"inline constexpr std::array<{type_name}, 0> {name}{{}};\n"
    body = ",\n".join(f"    {row}" for row in rows)
    return f"inline constexpr std::array<{type_name}, {n}> {name}{{{{\n{body}\n}}}};\n"


def field_row(row: dict[str, Any]) -> str:
    return (
        f"{{{cpp_string(row['name'])}, {cpp_string(row['type_name'])}, "
        f"{row['nth']}, {cpp_bool(row['is_serialized'])}, "
        f"{cpp_bool(row['is_signing_field'])}, {cpp_bool(row['is_vl_encoded'])}}}"
    )


def name_code_row(row: dict[str, Any]) -> str:
    return f"{{{cpp_string(row['name'])}, {row['code']}}}"


def generate_header(
    tables: dict[str, list[dict[str, Any]]],
    namespace: str,
    source_name: str = "unknown",
    source_sha: str = "unknown",
) -> str:
    """Deterministic header: identical tables give a byte-identical file."""
    parts = [
        emit_array(
            "catl::xdata::ProtocolTableField",
            "FIELDS",
            [field_row(r) for r in tables["FIELDS"]],
        ),
        emit_array(
            "catl::xdata::ProtocolTableNameCode",
            "TYPES",
            [name_code_row(r) for r in tables["TYPES"]],
        ),
        emit_array(
            "catl::xdata::ProtocolTableNameCode",
            "LEDGER_ENTRY_TYPES",
            [name_code_row(r) for r in tables["LEDGER_ENTRY_TYPES"]],
        ),
        emit_array(
            "catl::xdata::ProtocolTableNameCode",
            "TRANSACTION_TYPES",
            [name_code_row(r) for r in tables["TRANSACTION_TYPES"]],
        ),
        emit_array(
            "catl::xdata::ProtocolTableNameCode",
            "TRANSACTION_RESULTS",
            [name_code_row(r) for r in tables["TRANSACTION_RESULTS"]],
        ),
        emit_array(
            "catl::xdata::ProtocolTableNameCode",
            "PERMISSIONS",
            [name_code_row(r) for r in tables["PERMISSIONS"]],
        ),
    ]
    body = "\n".join(parts)
    return f"""// Auto-generated file. DO NOT EDIT.
// Generated from server definitions JSON by scripts/generate_definitions.py.
// The build regenerates this -- see x-data/definitions.cmake.
// Source: {source_name}
// SHA-256: {source_sha}
//
// Native Protocol tables — no JSON parse at embed load.
// Safe for WASM where _initialize must not fragment the heap.

#pragma once

#include "catl/xdata/protocol_tables.h"
#include <array>

namespace {namespace} {{

{body}
}} // namespace {namespace}
"""


def provider_tables_from_defs(
    defs: dict[str, Any], overrides: dict[str, str]
) -> dict[str, Any]:
    types = defs.get("TYPES")
    source_fields = defs.get("FIELDS")
    if not isinstance(types, dict) or not isinstance(source_fields, list):
        raise ValueError("provider protocol requires TYPES and FIELDS")

    serialized_type_names: set[str] = set()
    admitted: list[dict[str, Any]] = []
    material: list[dict[str, Any]] = []
    names: list[dict[str, Any]] = []
    name_parts: list[str] = []
    name_offset = 0

    for name_ordinal, entry in enumerate(source_fields):
        if not isinstance(entry, list) or len(entry) != 2:
            raise ValueError("Field definition must be a 2-element array")
        name, meta = entry
        if not isinstance(name, str) or not isinstance(meta, dict):
            raise ValueError("Field name/metadata have invalid types")
        type_name = meta.get("type")
        nth = meta.get("nth")
        if not isinstance(type_name, str) or not isinstance(nth, int):
            raise ValueError(f"field {name} has invalid type/nth")
        type_code = types.get(type_name)
        if not isinstance(type_code, int):
            raise ValueError(f"field {name} references unknown type {type_name}")
        encoded_name = name.encode("ascii")
        if len(encoded_name) > 0xFF or name_offset > 0xFFFF:
            raise ValueError("provider field-name table exceeds compact layout")
        name_flags = 0
        if bool(meta.get("isSerialized")):
            name_flags |= 1
        if bool(meta.get("isSigningField")):
            name_flags |= 2
        if bool(meta.get("isVLEncoded")):
            name_flags |= 4
        code = field_code(type_code, nth)
        names.append(
            {
                "code": code,
                "offset": name_offset,
                "size": len(encoded_name),
                "flags": name_flags,
            }
        )
        name_parts.append(cpp_blob_part(name))
        name_offset += len(encoded_name) + 1

        if not bool(meta.get("isSerialized")):
            continue
        if type_name not in PROVIDER_WIRE_TYPES:
            raise ValueError(f"serialized type {type_name} has no provider dispatch")
        serialized_type_names.add(type_name)
        if not (0 <= type_code <= 0xFF and 0 <= nth <= 0xFFFF):
            raise ValueError(f"serialized field {name} exceeds provider code layout")

        flags = 0
        if bool(meta.get("isSigningField")):
            flags |= 1
        if bool(meta.get("isVLEncoded")):
            flags |= 2
        if name == "ObjectEndMarker":
            flags |= 4
        if name == "ArrayEndMarker":
            flags |= 8
        default_materializer, fixed_size = PROVIDER_WIRE_TYPES[type_name]
        materializer = overrides.get(name, default_materializer)
        material_ordinal = NO_ORDINAL
        admission_ordinal = len(admitted)
        if name not in FIELD_SENTINELS:
            material_ordinal = len(material)
            material.append(
                {
                    "field_code": code,
                    "admission_ordinal": admission_ordinal,
                    "materializer": materializer,
                }
            )
        admitted.append(
            {
                "code": code,
                "name_ordinal": name_ordinal,
                "material_ordinal": material_ordinal,
                "fixed_size": fixed_size,
                "header_size": header_size(type_code, nth),
                "wire_type": type_code,
                "flags": flags,
                "materializer": "invalid" if name in FIELD_SENTINELS else materializer,
            }
        )

    if serialized_type_names != set(PROVIDER_WIRE_TYPES):
        missing = sorted(set(PROVIDER_WIRE_TYPES) - serialized_type_names)
        extra = sorted(serialized_type_names - set(PROVIDER_WIRE_TYPES))
        raise ValueError(
            f"provider wire closure mismatch; missing={missing}, extra={extra}"
        )
    material_names = {
        source_fields[row["name_ordinal"]][0]
        for row in admitted
        if row["material_ordinal"] != NO_ORDINAL
    }
    if (
        set(overrides) != set(PROVIDER_FIELD_OVERRIDES)
        or not set(overrides) <= material_names
    ):
        raise ValueError("provider field overrides do not join material fields")
    if len(names) != 337 or len(admitted) != 327 or len(material) != 325:
        raise ValueError(
            "provider Xahau closure must contain 337 names, 327 admitted "
            "fields, and 325 material fields"
        )
    admitted_codes = [row["code"] for row in admitted]
    if len(set(admitted_codes)) != len(admitted_codes):
        raise ValueError("provider admitted field codes must be unique")

    fast = [[0] * 128 for _ in range(32)]
    fallback: list[dict[str, int]] = []
    for ordinal, row in enumerate(admitted):
        type_code = row["code"] >> 16
        nth = row["code"] & 0xFFFF
        if type_code < 32 and nth < 128:
            if fast[type_code][nth] != 0:
                raise ValueError(f"duplicate fast field code {row['code']}")
            fast[type_code][nth] = ordinal + 1
        else:
            fallback.append(
                {
                    "field_code": row["code"],
                    "admission_ordinal": ordinal,
                    "flags": row["flags"],
                }
            )
    fallback.sort(key=lambda row: row["field_code"])

    type_rows: list[dict[str, Any]] = []
    type_name_parts: list[str] = []
    type_name_offset = 0
    for type_name, (materializer, fixed_size) in sorted(
        PROVIDER_WIRE_TYPES.items(), key=lambda item: int(types[item[0]])
    ):
        encoded_name = type_name.encode("ascii")
        if len(encoded_name) > 0xFF or type_name_offset > 0xFFFF:
            raise ValueError("provider type-name table exceeds compact layout")
        type_rows.append(
            {
                "code": int(types[type_name]),
                "fixed_size": fixed_size,
                "name_offset": type_name_offset,
                "name_size": len(encoded_name),
                "materializer": materializer,
            }
        )
        type_name_parts.append(cpp_blob_part(type_name))
        type_name_offset += len(encoded_name) + 1

    return {
        "names": names,
        "name_parts": name_parts,
        "name_bytes_size": name_offset,
        "fields": admitted,
        "fallback": fallback,
        "material": material,
        "types": type_rows,
        "type_name_parts": type_name_parts,
        "fast": fast,
        "max_type_code": max(row["wire_type"] for row in admitted),
        "max_nth": max(row["code"] & 0xFFFF for row in admitted),
        "duplicate_word_count": (len(material) + 63) // 64,
    }


def _static_array(type_name: str, name: str, rows: list[str]) -> str:
    return emit_array(type_name, name, rows)


def generate_provider_header(
    tables: dict[str, Any],
    namespace: str,
    source_name: str,
    source_sha: str,
    policy_sha: str,
) -> str:
    name_rows = [
        f"{{0x{row['code']:08x}u, {row['offset']}, {row['size']}, {row['flags']}}}"
        for row in tables["names"]
    ]
    field_rows = [
        "{"
        f"0x{row['code']:08x}u, {row['name_ordinal']}, "
        f"{row['material_ordinal']}, {row['fixed_size']}, "
        f"{row['header_size']}, {row['wire_type']}, {row['flags']}, "
        f"MaterializerKind::{row['materializer']}, 0"
        "}"
        for row in tables["fields"]
    ]
    fallback_rows = [
        f"{{0x{row['field_code']:08x}u, {row['admission_ordinal']}, {row['flags']}}}"
        for row in tables["fallback"]
    ]
    material_rows = [
        "{"
        f"0x{row['field_code']:08x}u, {row['admission_ordinal']}, "
        f"MaterializerKind::{row['materializer']}, 0"
        "}"
        for row in tables["material"]
    ]
    type_rows = [
        "{"
        f"{row['code']}, {row['fixed_size']}, {row['name_offset']}, "
        f"{row['name_size']}, MaterializerKind::{row['materializer']}"
        "}"
        for row in tables["types"]
    ]
    fast_rows = ",\n".join(
        "    {" + ", ".join(str(value) for value in row) + "}" for row in tables["fast"]
    )
    body = "\n".join(
        [
            "inline constexpr char FIELD_NAME_BYTES[] =\n    "
            + "\n    ".join(tables["name_parts"])
            + ";",
            _static_array("StaticFieldName", "FIELD_NAMES", name_rows),
            _static_array("StaticFieldDescriptor", "FIELDS", field_rows),
            "inline constexpr std::uint16_t FAST_ORDINALS[32][128] = {\n"
            + fast_rows
            + "\n};\n",
            _static_array("StaticFallbackField", "FALLBACK_FIELDS", fallback_rows),
            _static_array("StaticMaterialField", "MATERIAL_FIELDS", material_rows),
            "inline constexpr char TYPE_NAME_BYTES[] =\n    "
            + "\n    ".join(tables["type_name_parts"])
            + ";",
            _static_array("StaticTypeDescriptor", "TYPES", type_rows),
        ]
    )
    identity = f"xahau:{source_sha}"
    return f"""// Auto-generated file. DO NOT EDIT.
// Generated by scripts/generate_definitions.py --provider-static.
// Source: {source_name}
// SHA-256: {source_sha}
// Materializer policy SHA-256: {policy_sha}

#pragma once

#include "catl/xdata/static_protocol.h"
#include <array>
#include <cstdint>

namespace {namespace} {{

using namespace catl::xdata;

{body}
inline constexpr char DEFINITIONS_SHA256[] = {cpp_string(source_sha)};
inline constexpr char PROTOCOL_IDENTITY[] = {cpp_string(identity)};

inline constexpr ProtocolView PROTOCOL{{
    FIELDS.data(),
    FIELD_NAMES.data(),
    FALLBACK_FIELDS.data(),
    MATERIAL_FIELDS.data(),
    TYPES.data(),
    FAST_ORDINALS,
    FIELD_NAME_BYTES,
    TYPE_NAME_BYTES,
    DEFINITIONS_SHA256,
    PROTOCOL_IDENTITY,
    {tables["name_bytes_size"]},
    {len(tables["fields"])},
    {len(tables["names"])},
    {len(tables["fallback"])},
    {len(tables["material"])},
    {len(tables["types"])},
    {tables["max_type_code"]},
    {tables["max_nth"]},
    {tables["duplicate_word_count"]},
    0,
}};

static_assert(sizeof(FAST_ORDINALS) == 8192);
static_assert(FIELDS.size() == 327);
static_assert(FIELD_NAMES.size() == 337);
static_assert(MATERIAL_FIELDS.size() == 325);
static_assert(TYPES.size() == 19);
static_assert(PROTOCOL.duplicate_word_count == 6);
static_assert(PROTOCOL.inferred_vl_count == 0);

}} // namespace {namespace}
"""


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Generate C++ Protocol tables from server_definitions JSON"
    )
    parser.add_argument("--input", type=Path, required=True, help="Input JSON file")
    parser.add_argument("--output", type=Path, required=True, help="Output path")
    parser.add_argument("--namespace", default="catl::xdata", help="C++ namespace")
    parser.add_argument(
        "--no-clean",
        action="store_true",
        help="Skip cleanup (duplicates, features)",
    )
    parser.add_argument(
        "--emit-json",
        action="store_true",
        help="Write cleaned protocol JSON instead of a C++ header",
    )
    parser.add_argument(
        "--provider-static",
        action="store_true",
        help="Emit the allocation-free sealed-provider ProtocolView",
    )
    parser.add_argument(
        "--materializer-policy",
        type=Path,
        help="Policy JSON owning exact field-level materializer overrides",
    )
    args = parser.parse_args()

    if not args.input.exists():
        print(f"Error: {args.input} not found", file=sys.stderr)
        sys.exit(1)

    with open(args.input) as f:
        raw = json.load(f)

    try:
        defs = unwrap_definitions(raw)
        if not args.no_clean:
            defs = clean_definitions(defs)
        if args.emit_json:
            if args.provider_static or args.materializer_policy is not None:
                raise ValueError("--emit-json cannot be combined with provider options")
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(defs), encoding="utf-8")
            print(f"  Wrote {args.output}", file=sys.stderr)
            return
        if args.provider_static:
            overrides, policy_sha = load_materializer_policy(args.materializer_policy)
            tables = provider_tables_from_defs(defs, overrides)
        else:
            if args.materializer_policy is not None:
                raise ValueError("--materializer-policy requires --provider-static")
            tables = tables_from_defs(defs)
    except ValueError as error:
        print(f"Error: {error}", file=sys.stderr)
        sys.exit(1)

    print(
        (
            f"Loaded {args.input}: {len(tables['fields'])} admitted fields, "
            f"{len(tables['material'])} material fields"
            if args.provider_static
            else f"Loaded {args.input}: {len(tables['FIELDS'])} fields, "
            f"{len(tables['TRANSACTION_TYPES'])} tx types, "
            f"{len(tables['PERMISSIONS'])} permissions"
        ),
        file=sys.stderr,
    )

    source_sha = hashlib.sha256(args.input.read_bytes()).hexdigest()
    try:
        if args.provider_static:
            header = generate_provider_header(
                tables,
                args.namespace,
                args.input.name,
                source_sha,
                policy_sha,
            )
        else:
            header = generate_header(
                tables, args.namespace, args.input.name, source_sha
            )
    except ValueError as error:
        print(f"Error: {error}", file=sys.stderr)
        sys.exit(1)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(header)
    print(f"  Wrote {args.output}", file=sys.stderr)


if __name__ == "__main__":
    main()
