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
    return (
        f"inline constexpr std::array<{type_name}, {n}> {name}{{{{\n"
        f"{body}\n"
        f"}}}};\n"
    )


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
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(defs), encoding="utf-8")
            print(f"  Wrote {args.output}", file=sys.stderr)
            return
        tables = tables_from_defs(defs)
    except ValueError as error:
        print(f"Error: {error}", file=sys.stderr)
        sys.exit(1)

    print(
        f"Loaded {args.input}: {len(tables['FIELDS'])} fields, "
        f"{len(tables['TRANSACTION_TYPES'])} tx types, "
        f"{len(tables['PERMISSIONS'])} permissions",
        file=sys.stderr,
    )

    source_sha = hashlib.sha256(args.input.read_bytes()).hexdigest()
    try:
        header = generate_header(tables, args.namespace, args.input.name, source_sha)
    except ValueError as error:
        print(f"Error: {error}", file=sys.stderr)
        sys.exit(1)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(header)
    print(f"  Wrote {args.output}", file=sys.stderr)


if __name__ == "__main__":
    main()
