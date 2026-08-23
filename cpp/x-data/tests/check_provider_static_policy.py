#!/usr/bin/env python3
"""Enforce the sealed provider's non-allocating x-data source closure."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

INVENTORY_NAME = "JSHOOKZ_XDATA_PROVIDER_STATIC_SOURCES"
FORBIDDEN_SOURCE_SUFFIXES = (
    "src/protocol.cpp",
    "src/embedded_protocol.cpp",
    "src/protocol_json.cpp",
    "boost_json_src.cpp",
    "core/src/types.cpp",
)
FORBIDDEN_SOURCE_PATTERNS = {
    "dynamic Protocol include": r"catl/xdata/protocol\.h",
    "allocating FieldTypes include": r"catl/xdata/types\.h",
    "allocating FieldTypes global": r"\bFieldTypes::",
    "vector ownership": r"\bstd::vector\b",
    "string ownership": r"\bstd::(?:basic_)?string\b",
    "map ownership": r"\bstd::(?:unordered_)?map\b",
    "set ownership": r"\bstd::(?:unordered_)?set\b",
    "smart-pointer ownership": r"\bstd::(?:unique_ptr|shared_ptr|make_unique|make_shared)\b",
    "ordinary new expression": r"(?:^|[^A-Za-z0-9_])new\s+",
    "ordinary delete expression": r"(?:^|[^A-Za-z0-9_])delete\s+",
    "throw expression": r"(?:^|[^A-Za-z0-9_])throw(?:\s|;)",
    "RTTI typeid": r"\btypeid\s*\(",
    "RTTI dynamic_cast": r"\bdynamic_cast\s*<",
}
FORBIDDEN_SYMBOL_PATTERNS = {
    "operator new": r"(?:^|\s)_*Zn(?:w|a)",
    "operator delete": r"(?:^|\s)_*Zd(?:l|a)",
    "C++ ABI runtime": r"__cxa_(?:atexit|guard_|throw|allocate_exception)",
    "RTTI typeinfo": r"(?:^|\s)_*ZT[ISV]",
    "dynamic Protocol": r"catl.*xdata.*Protocol(?!View)",
    "allocating FieldTypes": r"FieldTypes",
}


def inventory_sources(text: str) -> list[str]:
    match = re.search(
        rf"set\(\s*{INVENTORY_NAME}\s+(.*?)\)", text, re.DOTALL
    )
    if match is None:
        raise ValueError(f"missing {INVENTORY_NAME} set()")
    body = re.sub(r"#[^\n]*", "", match.group(1))
    tokens = body.split()
    prefix = "${CMAKE_CURRENT_LIST_DIR}/"
    if not tokens or any(not token.startswith(prefix) for token in tokens):
        raise ValueError("provider-static inventory must contain only literal local paths")
    sources = [token[len(prefix) :] for token in tokens]
    if len(sources) != len(set(sources)):
        raise ValueError("provider-static inventory contains duplicate sources")
    if any(not source.endswith(".cpp") for source in sources):
        raise ValueError("provider-static inventory contains a non-C++ source")
    return sources


def source_violations(name: str, text: str) -> list[str]:
    errors: list[str] = []
    if any(name.endswith(suffix) for suffix in FORBIDDEN_SOURCE_SUFFIXES):
        errors.append(f"{name}: prohibited provider-static translation unit")
    for label, pattern in FORBIDDEN_SOURCE_PATTERNS.items():
        if re.search(pattern, text, re.MULTILINE):
            errors.append(f"{name}: {label}")
    return errors


def validate_inventory(path: Path) -> list[str]:
    xdata = path.parent
    try:
        sources = inventory_sources(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        return [str(error)]
    errors: list[str] = []
    for source in sources:
        source_path = xdata / source
        try:
            text = source_path.read_text(encoding="utf-8")
        except OSError as error:
            errors.append(f"{source}: {error}")
            continue
        errors.extend(source_violations(source, text))
    return errors


def symbol_violations(text: str) -> list[str]:
    return [
        f"linked provider-static closure contains {label}"
        for label, pattern in FORBIDDEN_SYMBOL_PATTERNS.items()
        if re.search(pattern, text)
    ]


def inspect_symbols(nm: str, binary: Path) -> list[str]:
    result = subprocess.run(
        [nm, str(binary)], capture_output=True, text=True, check=False
    )
    if result.returncode != 0:
        return [f"nm failed for {binary}: {result.stderr.strip()}"]
    return symbol_violations(result.stdout)


def run_red_controls(inventory: Path) -> list[str]:
    failures: list[str] = []
    original = inventory.read_text(encoding="utf-8")
    for suffix in FORBIDDEN_SOURCE_SUFFIXES:
        poison = original.replace(
            f"set({INVENTORY_NAME}",
            f"set({INVENTORY_NAME}\n    ${{CMAKE_CURRENT_LIST_DIR}}/{suffix}",
            1,
        )
        try:
            sources = inventory_sources(poison)
        except ValueError:
            continue
        if not any(source_violations(source, "") for source in sources):
            failures.append(f"source poison stayed green: {suffix}")
    for label, pattern in FORBIDDEN_SOURCE_PATTERNS.items():
        sample = {
            "dynamic Protocol include": '#include "catl/xdata/protocol.h"',
            "allocating FieldTypes include": '#include "catl/xdata/types.h"',
            "allocating FieldTypes global": "auto x = FieldTypes::ALL;",
            "vector ownership": "std::vector<int> x;",
            "string ownership": "std::string x;",
            "map ownership": "std::map<int, int> x;",
            "set ownership": "std::unordered_set<int> x;",
            "smart-pointer ownership": "std::unique_ptr<int> x;",
            "ordinary new expression": "auto x = new int;",
            "ordinary delete expression": "delete value;",
            "throw expression": "throw value;",
            "RTTI typeid": "typeid(value);",
            "RTTI dynamic_cast": "dynamic_cast<T*>(value);",
        }[label]
        if re.search(pattern, sample) is None or not source_violations("poison.cpp", sample):
            failures.append(f"source-pattern poison stayed green: {label}")
    symbol_samples = {
        "operator new": "                 U __Znwm",
        "operator delete": "                 U __ZdlPv",
        "C++ ABI runtime": "                 U ___cxa_guard_acquire",
        "RTTI typeinfo": "00000000 D __ZTI4Test",
        "dynamic Protocol": "__ZN4catl5xdata8ProtocolC1Ev",
        "allocating FieldTypes": "__ZN4catl5xdata10FieldTypes3ALLE",
    }
    for label, sample in symbol_samples.items():
        if not any(label in error for error in symbol_violations(sample)):
            failures.append(f"symbol poison stayed green: {label}")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--inventory", type=Path, required=True)
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--nm", default="nm")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    errors = (
        run_red_controls(args.inventory)
        if args.self_test
        else validate_inventory(args.inventory)
    )
    if not args.self_test:
        if args.binary is None:
            errors.append("--binary is required outside --self-test")
        else:
            errors.extend(inspect_symbols(args.nm, args.binary))
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print(
        "provider-static policy red controls pass"
        if args.self_test
        else "provider-static source and symbol closure pass"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
