#!/usr/bin/env python3
"""Validate or explicitly regenerate the pinned xahauFloatV1 add/sub oracle."""

from __future__ import annotations

import argparse
import copy
import json
import re
import subprocess
from pathlib import Path
from typing import Any

SOURCE_SCHEMA = "xahau.xfl-arithmetic-oracle.v1"
FIXTURE_SCHEMA = "jshookz.xahau-float-v1-add-subtract-oracle.v1"
PROFILE = "xahauFloatV1"
XAHAUD_SOURCE_COMMIT = "bb244ef7729503a0317bcff0f8fdaa93ca5cb7d2"
XAHAUD_VECTORS_COMMIT = "c49c784213c83963c5c7b49f8589b9bf86bea58e"
ORACLE_REPOSITORY = "https://github.com/Xahau/xahaud.git"
ORACLE_SUITE = "ripple.app.HookzFloatVectors"
BEGIN = "---HOOKZ_FLOAT_VECTORS_BEGIN---"
END = "---HOOKZ_FLOAT_VECTORS_END---"

REQUIRED_CASE_IDS = frozenset(
    {
        "add.zero-zero",
        "add.zero-positive",
        "add.positive-zero",
        "add.zero-negative",
        "add.negative-zero",
        "add.same-exponent-positive",
        "add.same-exponent-negative",
        "add.exact-cancellation",
        "add.renormalizing-cancellation",
        "add.min-exponent-dust-zero",
        "add.align-delta-1",
        "add.align-delta-15",
        "add.align-delta-16-half-odd",
        "add.align-delta-16-six-tenths",
        "add.align-delta-17",
        "add.align-delta-176",
        "add.carry-positive",
        "add.carry-negative",
        "add.overflow-positive",
        "add.overflow-negative",
        "add.max-exponent-zero",
        "add.last-digit-negative",
        "subtract.zero-zero",
        "subtract.zero-positive",
        "subtract.positive-zero",
        "subtract.self",
        "subtract.same-exponent-positive",
        "subtract.same-exponent-negative",
        "subtract.align-delta-15",
        "subtract.align-delta-16-half-odd",
        "subtract.min-exponent-dust-zero",
        "subtract.overflow-positive",
        "subtract.overflow-negative",
        "subtract.min-exponent-sign",
    }
)

_UNSIGNED = re.compile(r"(?:0|[1-9][0-9]*)\Z")
_SIGNED = re.compile(r"(?:0|-?[1-9][0-9]*)\Z")


class OracleError(ValueError):
    """The committed or captured oracle violates its frozen schema."""


def _object(value: Any, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise OracleError(f"{label} must be an object")
    return value


def _exact_keys(value: dict[str, Any], keys: set[str], label: str) -> None:
    if set(value) != keys:
        raise OracleError(f"{label} keys must be exactly {sorted(keys)}")


def _typed_integer(value: Any, expected_type: str, label: str) -> int:
    leaf = _object(value, label)
    _exact_keys(leaf, {"type", "val"}, label)
    if leaf["type"] != expected_type or not isinstance(leaf["val"], str):
        raise OracleError(f"{label} must be a typed {expected_type} string")
    pattern = _UNSIGNED if expected_type == "u64" else _SIGNED
    if pattern.fullmatch(leaf["val"]) is None:
        raise OracleError(f"{label} has non-canonical integer spelling")
    integer = int(leaf["val"])
    if expected_type == "u64" and not 0 <= integer <= (1 << 64) - 1:
        raise OracleError(f"{label} is outside u64")
    if expected_type == "error" and integer != -30:
        raise OracleError(f"{label} is not the ratified XFL overflow code")
    return integer


def _result(value: Any, label: str) -> tuple[bool, int]:
    result = _object(value, label)
    if result.get("ok") is True:
        _exact_keys(result, {"ok", "value"}, label)
        return True, _typed_integer(result["value"], "u64", f"{label}.value")
    if result.get("ok") is False:
        _exact_keys(result, {"ok", "error"}, label)
        return False, _typed_integer(result["error"], "error", f"{label}.error")
    raise OracleError(f"{label}.ok must be boolean")


def validate_fixture(value: Any) -> dict[str, Any]:
    """Return *value* after strict schema, completeness, and pin checks."""

    root = _object(value, "fixture")
    _exact_keys(
        root,
        {
            "schema",
            "oracle",
            "profile",
            "xahaud_source_commit",
            "fix_universal_number",
            "number_so",
            "cases",
            "guard_drop_control",
        },
        "fixture",
    )
    if root["schema"] != FIXTURE_SCHEMA:
        raise OracleError("unsupported fixture schema")
    oracle = _object(root["oracle"], "oracle")
    _exact_keys(
        oracle,
        {"repository", "commit", "suite", "source_schema"},
        "oracle",
    )
    if oracle != {
        "repository": ORACLE_REPOSITORY,
        "commit": XAHAUD_VECTORS_COMMIT,
        "suite": ORACLE_SUITE,
        "source_schema": SOURCE_SCHEMA,
    }:
        raise OracleError("stale or unexpected oracle identity")
    if root["profile"] != PROFILE:
        raise OracleError("unexpected arithmetic profile")
    if root["xahaud_source_commit"] != XAHAUD_SOURCE_COMMIT:
        raise OracleError("stale xahaud source identity")
    if root["fix_universal_number"] is not False or root["number_so"] is not False:
        raise OracleError("xahauFloatV1 must be captured under NumberSO(false)")

    cases = root["cases"]
    if not isinstance(cases, list):
        raise OracleError("cases must be an array")
    ids: set[str] = set()
    for index, value_case in enumerate(cases):
        case = _object(value_case, f"cases[{index}]")
        _exact_keys(
            case,
            {"id", "category", "operation", "a", "b", "result"},
            f"cases[{index}]",
        )
        case_id = case["id"]
        if not isinstance(case_id, str) or not case_id:
            raise OracleError(f"cases[{index}].id must be a non-empty string")
        if case_id in ids:
            raise OracleError(f"duplicate case id {case_id}")
        ids.add(case_id)
        if not isinstance(case["category"], str) or not case["category"]:
            raise OracleError(f"{case_id}.category must be a non-empty string")
        if case["operation"] not in {"add", "subtract"}:
            raise OracleError(f"{case_id}.operation is unsupported")
        if not case_id.startswith(f"{case['operation']}."):
            raise OracleError(f"{case_id} disagrees with its operation")
        _typed_integer(case["a"], "u64", f"{case_id}.a")
        _typed_integer(case["b"], "u64", f"{case_id}.b")
        _result(case["result"], f"{case_id}.result")
    missing = REQUIRED_CASE_IDS - ids
    extra = ids - REQUIRED_CASE_IDS
    if missing or extra:
        raise OracleError(
            f"operation matrix mismatch: missing={sorted(missing)} extra={sorted(extra)}"
        )

    guard = _object(root["guard_drop_control"], "guard_drop_control")
    _exact_keys(
        guard,
        {"id", "a", "b", "number_so_false", "number_so_true"},
        "guard_drop_control",
    )
    if guard["id"] != "guard.number-so-odd-half-ulp":
        raise OracleError("unexpected guard case")
    _typed_integer(guard["a"], "u64", "guard.a")
    _typed_integer(guard["b"], "u64", "guard.b")
    false_ok, false_value = _result(guard["number_so_false"], "guard.false")
    true_ok, true_value = _result(guard["number_so_true"], "guard.true")
    if not false_ok or not true_ok or false_value == true_value:
        raise OracleError("NumberSO guard does not distinguish arithmetic paths")
    return root


def load_fixture(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise OracleError(f"cannot read fixture {path}: {error}") from error
    return validate_fixture(value)


def _capture(checkout: Path, rippled: Path) -> dict[str, Any]:
    commit = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=checkout,
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    if commit != XAHAUD_VECTORS_COMMIT:
        raise OracleError(
            f"xahaud-vectors checkout is {commit}, expected {XAHAUD_VECTORS_COMMIT}"
        )
    completed = subprocess.run(
        [
            str(rippled.resolve()),
            f"--unittest={ORACLE_SUITE}",
            "--unittest-log",
            "--quiet",
        ],
        cwd=checkout,
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        raise OracleError(
            "xahaud oracle suite failed:\n" + completed.stdout + completed.stderr
        )
    try:
        payload = completed.stdout.split(BEGIN, 1)[1].split(END, 1)[0]
        source = _object(json.loads(payload)["xfl_arithmetic"], "xfl_arithmetic")
    except (IndexError, KeyError, json.JSONDecodeError) as error:
        raise OracleError("xahaud oracle markers or payload are malformed") from error
    if source.get("schema") != SOURCE_SCHEMA:
        raise OracleError("captured source schema is unsupported")
    fixture = {
        "schema": FIXTURE_SCHEMA,
        "oracle": {
            "repository": ORACLE_REPOSITORY,
            "commit": XAHAUD_VECTORS_COMMIT,
            "suite": ORACLE_SUITE,
            "source_schema": source["schema"],
        },
        "profile": source.get("profile"),
        "xahaud_source_commit": source.get("xahaud_source_commit"),
        "fix_universal_number": source.get("fix_universal_number"),
        "number_so": source.get("number_so"),
        "cases": copy.deepcopy(source.get("cases")),
        "guard_drop_control": copy.deepcopy(source.get("guard_drop_control")),
    }
    return validate_fixture(fixture)


def canonical_json(value: dict[str, Any]) -> str:
    return json.dumps(value, indent=2, sort_keys=True) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fixture", type=Path)
    parser.add_argument("--checkout", type=Path)
    parser.add_argument("--rippled", type=Path)
    parser.add_argument(
        "--update",
        action="store_true",
        help="explicitly replace fixture from the exact pinned oracle",
    )
    args = parser.parse_args()
    if (args.checkout is None) != (args.rippled is None):
        parser.error("--checkout and --rippled must be provided together")
    if args.update and args.checkout is None:
        parser.error("--update requires --checkout and --rippled")

    if args.checkout is None:
        load_fixture(args.fixture)
        return 0

    captured = _capture(args.checkout.resolve(), args.rippled.resolve())
    rendered = canonical_json(captured)
    if args.update:
        args.fixture.write_text(rendered)
        return 0
    if canonical_json(load_fixture(args.fixture)) != rendered:
        raise OracleError("committed fixture differs from the pinned oracle")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
