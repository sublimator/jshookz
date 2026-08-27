#!/usr/bin/env python3
"""Validate or explicitly regenerate pinned xahauFloatV1 arithmetic oracles."""

from __future__ import annotations

import argparse
import copy
import json
import re
import subprocess
from pathlib import Path
from typing import Any

PROFILE = "xahauFloatV1"
XAHAUD_SOURCE_COMMIT = "bb244ef7729503a0317bcff0f8fdaa93ca5cb7d2"
ORACLE_REPOSITORY = "https://github.com/Xahau/xahaud.git"
ORACLE_SUITE = "ripple.app.HookzFloatVectors"
BEGIN = "---HOOKZ_FLOAT_VECTORS_BEGIN---"
END = "---HOOKZ_FLOAT_VECTORS_END---"

ADD_SUBTRACT_SOURCE_SCHEMA = "xahau.xfl-arithmetic-oracle.v1"
ADD_SUBTRACT_FIXTURE_SCHEMA = "jshookz.xahau-float-v1-add-subtract-oracle.v1"
ADD_SUBTRACT_VECTORS_COMMIT = "6569e562818013a94aaaf3de48b2df8ef438e28a"

MULTIPLY_DIVIDE_SOURCE_SCHEMA = "xahau.xfl-multiply-divide-oracle.v1"
MULTIPLY_DIVIDE_FIXTURE_SCHEMA = "jshookz.xahau-float-v1-multiply-divide-oracle.v1"
MULTIPLY_DIVIDE_VECTORS_COMMIT = "b865c6fc554e3a06bbbf4926fbc5e8fb50d2f351"
MULTIPLY_DIVIDE_VECTORS_BRANCH = "hookz-test-vectors"

# Backward-compatible names for the original add/subtract reader surface.
SOURCE_SCHEMA = ADD_SUBTRACT_SOURCE_SCHEMA
FIXTURE_SCHEMA = ADD_SUBTRACT_FIXTURE_SCHEMA
XAHAUD_VECTORS_COMMIT = ADD_SUBTRACT_VECTORS_COMMIT


class OracleFamily:
    __slots__ = (
        "name",
        "source_schema",
        "fixture_schema",
        "vectors_commit",
        "source_key",
        "case_fields",
        "required_categories",
        "public_errors",
    )

    def __init__(
        self,
        *,
        name: str,
        source_schema: str,
        fixture_schema: str,
        vectors_commit: str,
        source_key: str,
        case_fields: tuple[tuple[str, frozenset[str]], ...],
        required_categories: dict[str, str],
        public_errors: dict[str, dict[int, str]],
    ) -> None:
        self.name = name
        self.source_schema = source_schema
        self.fixture_schema = fixture_schema
        self.vectors_commit = vectors_commit
        self.source_key = source_key
        self.case_fields = case_fields
        self.required_categories = required_categories
        self.public_errors = public_errors


ADD_SUBTRACT_REQUIRED_CATEGORIES = {
    "add.zero-zero": "zero_identity",
    "add.zero-positive": "zero_identity",
    "add.positive-zero": "zero_identity",
    "add.zero-negative": "zero_identity",
    "add.negative-zero": "zero_identity",
    "add.same-exponent-positive": "same_exponent",
    "add.same-exponent-negative": "same_exponent",
    "add.exact-cancellation": "cancellation",
    "add.renormalizing-cancellation": "borrow",
    "add.legacy-clamp-positive-ten": "legacy_clamp",
    "add.legacy-clamp-negative-ten": "legacy_clamp",
    "add.min-exponent-dust-zero": "underflow_to_zero",
    "add.align-delta-1": "alignment",
    "add.align-delta-15": "alignment",
    "add.align-delta-16-half-odd": "last_digit_guard",
    "add.align-delta-16-six-tenths": "last_digit_guard",
    "add.align-delta-17": "alignment_dust",
    "add.align-delta-176": "maximum_alignment",
    "add.carry-positive": "carry",
    "add.carry-negative": "carry",
    "add.overflow-positive": "overflow",
    "add.overflow-negative": "overflow",
    "add.max-exponent-zero": "zero_precedes_overflow",
    "add.last-digit-negative": "last_digit_guard",
    "subtract.zero-zero": "zero_identity",
    "subtract.zero-positive": "zero_identity",
    "subtract.positive-zero": "zero_identity",
    "subtract.self": "cancellation",
    "subtract.same-exponent-positive": "same_exponent",
    "subtract.same-exponent-negative": "same_exponent",
    "subtract.legacy-clamp-positive-ten": "legacy_clamp",
    "subtract.legacy-clamp-negative-ten": "legacy_clamp",
    "subtract.align-delta-15": "alignment",
    "subtract.align-delta-16-half-odd": "last_digit_guard",
    "subtract.min-exponent-dust-zero": "underflow_to_zero",
    "subtract.overflow-positive": "overflow",
    "subtract.overflow-negative": "overflow",
    "subtract.min-exponent-sign": "minimum_exponent",
}

MULTIPLY_DIVIDE_REQUIRED_CATEGORIES = {
    "multiply.zero-left": "zero_order",
    "multiply.zero-right": "zero_order",
    "multiply.zero-zero": "zero_order",
    "multiply.positive-positive": "sign",
    "multiply.negative-positive": "sign",
    "multiply.positive-negative": "sign",
    "multiply.negative-negative": "sign",
    "multiply.min-mantissa-by-one": "mantissa_extrema",
    "multiply.max-mantissa-by-one": "mantissa_extrema",
    "multiply.min-exponent-by-one": "exponent_extrema",
    "multiply.max-exponent-by-one": "exponent_extrema",
    "multiply.normalization-carry": "normalization",
    "multiply.truncation-tail": "last_digit",
    "multiply.negative-truncation-tail": "last_digit",
    "multiply.order-16-below": "decimal_order_transition",
    "multiply.order-16-above": "decimal_order_transition",
    "multiply.order-17-below": "decimal_order_transition",
    "multiply.order-17-above": "decimal_order_transition",
    "multiply.order-17-retained-digit": "decimal_order_transition",
    "multiply.max-by-max": "mantissa_extrema",
    "multiply.underflow-to-zero": "underflow_to_zero",
    "multiply.overflow": "overflow",
    "multiply.invalid-left-zero": "precedence",
    "multiply.zero-invalid-right": "precedence",
    "multiply.invalid-left-valid": "precedence",
    "divide.zero-numerator": "zero_order",
    "divide.denominator-zero": "division_by_zero",
    "divide.zero-zero": "precedence",
    "divide.positive-positive": "sign",
    "divide.negative-positive": "sign",
    "divide.positive-negative": "sign",
    "divide.negative-negative": "sign",
    "divide.min-mantissa-by-one": "mantissa_extrema",
    "divide.max-mantissa-by-one": "mantissa_extrema",
    "divide.min-mantissa-numerator-generic": "mantissa_extrema_generic",
    "divide.max-mantissa-numerator-generic": "mantissa_extrema_generic",
    "divide.min-mantissa-denominator-generic": "mantissa_extrema_generic",
    "divide.max-mantissa-denominator-generic": "mantissa_extrema_generic",
    "divide.min-exponent-by-one": "exponent_extrema",
    "divide.max-exponent-by-one": "exponent_extrema",
    "divide.min-exponent-numerator-generic": "exponent_extrema_generic",
    "divide.min-exponent-denominator-generic": "exponent_extrema_generic",
    "divide.max-exponent-numerator-zero": "input_normalization_precedence",
    "divide.zero-max-exponent-denominator": "input_normalization_precedence",
    "divide.max-exponent-numerator-generic": "input_normalization",
    "divide.max-exponent-denominator-generic": "input_normalization",
    "divide.smaller-denominator": "normalization",
    "divide.larger-denominator": "normalization",
    "divide.order-15-below": "decimal_order_transition",
    "divide.order-15-above": "decimal_order_transition",
    "divide.order-15-inward-correction": "decimal_order_transition",
    "divide.restoring-digit-10": "restoring_loop",
    "divide.restoring-coefficient-18": "restoring_loop",
    "divide.fixed-last-digit": "fixed_divide",
    "divide.repeating-one-third": "last_digit",
    "divide.last-digit-neighbor": "last_digit",
    "divide.underflow-to-zero": "underflow_to_zero",
    "divide.overflow": "overflow",
    "divide.invalid-numerator-zero": "precedence",
    "divide.zero-invalid-denominator": "precedence",
    "divide.valid-invalid-denominator": "precedence",
}

ADD_SUBTRACT_FAMILY = OracleFamily(
    name="add-subtract",
    source_schema=ADD_SUBTRACT_SOURCE_SCHEMA,
    fixture_schema=ADD_SUBTRACT_FIXTURE_SCHEMA,
    vectors_commit=ADD_SUBTRACT_VECTORS_COMMIT,
    source_key="xfl_arithmetic",
    case_fields=(("cases", frozenset({"add", "subtract"})),),
    required_categories=ADD_SUBTRACT_REQUIRED_CATEGORIES,
    public_errors={
        "add": {-30: "overflow"},
        "subtract": {-30: "overflow"},
    },
)

MULTIPLY_DIVIDE_FAMILY = OracleFamily(
    name="multiply-divide",
    source_schema=MULTIPLY_DIVIDE_SOURCE_SCHEMA,
    fixture_schema=MULTIPLY_DIVIDE_FIXTURE_SCHEMA,
    vectors_commit=MULTIPLY_DIVIDE_VECTORS_COMMIT,
    source_key="xfl_multiply_divide",
    case_fields=(
        ("multiply_cases", frozenset({"multiply"})),
        ("divide_cases", frozenset({"divide"})),
    ),
    required_categories=MULTIPLY_DIVIDE_REQUIRED_CATEGORIES,
    public_errors={
        "multiply": {-30: "overflow", -10024: "invalid"},
        "divide": {
            -30: "overflow",
            -25: "division-by-zero",
            -10024: "invalid",
        },
    },
)

FAMILIES_BY_NAME = {
    ADD_SUBTRACT_FAMILY.name: ADD_SUBTRACT_FAMILY,
    MULTIPLY_DIVIDE_FAMILY.name: MULTIPLY_DIVIDE_FAMILY,
}
FAMILIES_BY_FIXTURE_SCHEMA = {
    family.fixture_schema: family for family in FAMILIES_BY_NAME.values()
}

# Backward-compatible completeness constant for existing add/subtract users.
REQUIRED_CASE_IDS = frozenset(ADD_SUBTRACT_REQUIRED_CATEGORIES)

_UNSIGNED = re.compile(r"(?:0|[1-9][0-9]*)\Z")
_SIGNED = re.compile(r"(?:0|-?[1-9][0-9]*)\Z")
_BUILD_COMMIT = re.compile(
    r"^Git commit hash: (?P<commit>[0-9a-f]{40})(?P<dirty>-dirty)?$",
    re.MULTILINE,
)


class OracleError(ValueError):
    """The committed or captured oracle violates its frozen schema."""


def _object(value: Any, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise OracleError(f"{label} must be an object")
    return value


def _array(value: Any, label: str) -> list[Any]:
    if not isinstance(value, list):
        raise OracleError(f"{label} must be an array")
    return value


def _exact_keys(value: dict[str, Any], keys: set[str], label: str) -> None:
    if set(value) != keys:
        raise OracleError(f"{label} keys must be exactly {sorted(keys)}")


def _family_for_root(root: dict[str, Any]) -> OracleFamily:
    schema = root.get("schema")
    if not isinstance(schema, str) or schema not in FAMILIES_BY_FIXTURE_SCHEMA:
        raise OracleError("unsupported fixture schema")
    return FAMILIES_BY_FIXTURE_SCHEMA[schema]


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
    return integer


def _result(
    value: Any,
    label: str,
    public_errors: dict[int, str],
) -> tuple[bool, int]:
    result = _object(value, label)
    if result.get("ok") is True:
        _exact_keys(result, {"ok", "value"}, label)
        return True, _typed_integer(result["value"], "u64", f"{label}.value")
    if result.get("ok") is False:
        _exact_keys(result, {"ok", "error"}, label)
        error = _typed_integer(result["error"], "error", f"{label}.error")
        if error not in public_errors:
            raise OracleError(f"{label}.error is not accepted for this operation")
        return False, error
    raise OracleError(f"{label}.ok must be boolean")


def _validate_oracle_identity(value: Any, family: OracleFamily) -> dict[str, Any]:
    oracle = _object(value, "oracle")
    _exact_keys(
        oracle,
        {"repository", "commit", "suite", "source_schema"},
        "oracle",
    )
    if oracle != {
        "repository": ORACLE_REPOSITORY,
        "commit": family.vectors_commit,
        "suite": ORACLE_SUITE,
        "source_schema": family.source_schema,
    }:
        raise OracleError("stale or unexpected oracle identity")
    return oracle


def _validate_multiply_divide_build(value: Any) -> dict[str, Any]:
    build = _object(value, "build")
    _exact_keys(build, {"branch", "commit", "commit_hash", "dirty"}, "build")
    if build != {
        "branch": MULTIPLY_DIVIDE_VECTORS_BRANCH,
        "commit": MULTIPLY_DIVIDE_VECTORS_COMMIT,
        "commit_hash": MULTIPLY_DIVIDE_VECTORS_COMMIT,
        "dirty": False,
    }:
        raise OracleError("stale, dirty, or unexpected oracle build identity")
    return build


def _case_arrays(
    root: dict[str, Any], family: OracleFamily
) -> list[tuple[str, frozenset[str], list[Any]]]:
    return [
        (field, operations, _array(root[field], field))
        for field, operations in family.case_fields
    ]


def _validate_cases(
    root: dict[str, Any], family: OracleFamily
) -> dict[str, dict[str, Any]]:
    by_id: dict[str, dict[str, Any]] = {}
    for field, field_operations, cases in _case_arrays(root, family):
        for index, value_case in enumerate(cases):
            label = f"{field}[{index}]"
            case = _object(value_case, label)
            _exact_keys(
                case,
                {"id", "category", "operation", "a", "b", "result"},
                label,
            )
            case_id = case["id"]
            if not isinstance(case_id, str) or not case_id:
                raise OracleError(f"{label}.id must be a non-empty string")
            if case_id in by_id:
                raise OracleError(f"duplicate case id {case_id}")
            operation = case["operation"]
            if operation not in field_operations:
                raise OracleError(f"{case_id}.operation is unsupported in {field}")
            if not case_id.startswith(f"{operation}."):
                raise OracleError(f"{case_id} disagrees with its operation")
            expected_category = family.required_categories.get(case_id)
            if expected_category is not None and case["category"] != expected_category:
                raise OracleError(f"{case_id}.category is not the pinned category")
            if not isinstance(case["category"], str) or not case["category"]:
                raise OracleError(f"{case_id}.category must be a non-empty string")
            _typed_integer(case["a"], "u64", f"{case_id}.a")
            _typed_integer(case["b"], "u64", f"{case_id}.b")
            _result(
                case["result"],
                f"{case_id}.result",
                family.public_errors[operation],
            )
            by_id[case_id] = case

    ids = set(by_id)
    required = set(family.required_categories)
    missing = required - ids
    extra = ids - required
    if missing or extra:
        raise OracleError(
            f"operation matrix mismatch: missing={sorted(missing)} extra={sorted(extra)}"
        )
    return by_id


def _validate_add_subtract(root: dict[str, Any], family: OracleFamily) -> None:
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
    if root["fix_universal_number"] is not False or root["number_so"] is not False:
        raise OracleError("xahauFloatV1 must be captured under NumberSO(false)")
    cases = _validate_cases(root, family)

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
    false_ok, false_value = _result(
        guard["number_so_false"], "guard.false", family.public_errors["add"]
    )
    true_ok, true_value = _result(
        guard["number_so_true"], "guard.true", family.public_errors["add"]
    )
    if not false_ok or not true_ok or false_value == true_value:
        raise OracleError("NumberSO guard does not distinguish arithmetic paths")
    if cases["add.align-delta-16-half-odd"]["result"] != guard["number_so_false"]:
        raise OracleError("NumberSO guard does not match the pinned legacy row")


def _validate_multiply_divide(root: dict[str, Any], family: OracleFamily) -> None:
    _exact_keys(
        root,
        {
            "schema",
            "oracle",
            "profile",
            "xahaud_source_commit",
            "fix_float_divide",
            "build",
            "multiply_cases",
            "divide_cases",
            "guard_fix_float_divide",
        },
        "fixture",
    )
    if root["fix_float_divide"] is not True:
        raise OracleError("xahauFloatV1 divide must be captured with fixFloatDivide")
    _validate_multiply_divide_build(root["build"])
    cases = _validate_cases(root, family)

    guard = _object(root["guard_fix_float_divide"], "guard_fix_float_divide")
    _exact_keys(
        guard,
        {"id", "a", "b", "fix_false", "fix_true"},
        "guard_fix_float_divide",
    )
    if guard["id"] != "guard.fix-float-divide-last-digit":
        raise OracleError("unexpected fixed-divide guard case")
    _typed_integer(guard["a"], "u64", "guard.a")
    _typed_integer(guard["b"], "u64", "guard.b")
    false_ok, false_value = _result(
        guard["fix_false"], "guard.fix_false", family.public_errors["divide"]
    )
    true_ok, true_value = _result(
        guard["fix_true"], "guard.fix_true", family.public_errors["divide"]
    )
    if not false_ok or not true_ok or false_value == true_value:
        raise OracleError("fixed-divide guard does not distinguish arithmetic paths")
    if true_value != false_value + 1:
        raise OracleError(
            "fixed-divide guard does not preserve the retained-digit delta"
        )
    fixed_row = cases["divide.fixed-last-digit"]
    if fixed_row["a"] != guard["a"] or fixed_row["b"] != guard["b"]:
        raise OracleError("fixed-divide guard operands do not match the pinned row")
    if fixed_row["result"] != guard["fix_true"]:
        raise OracleError("fixed-divide guard does not match the pinned fixed row")


def validate_fixture(value: Any) -> dict[str, Any]:
    """Return *value* after strict schema, completeness, and pin checks."""

    root = _object(value, "fixture")
    family = _family_for_root(root)
    _validate_oracle_identity(root.get("oracle"), family)
    if root.get("profile") != PROFILE:
        raise OracleError("unexpected arithmetic profile")
    if root.get("xahaud_source_commit") != XAHAUD_SOURCE_COMMIT:
        raise OracleError("stale xahaud source identity")
    if family is ADD_SUBTRACT_FAMILY:
        _validate_add_subtract(root, family)
    else:
        _validate_multiply_divide(root, family)
    return root


def fixture_cases(value: dict[str, Any]) -> list[dict[str, Any]]:
    """Return all operation rows from either validated fixture family."""

    root = validate_fixture(value)
    family = _family_for_root(root)
    return [case for _, _, cases in _case_arrays(root, family) for case in cases]


def public_error(case: dict[str, Any]) -> str:
    """Map a failed oracle row's Hook status to its public XFL issue."""

    operation = case.get("operation")
    for family in FAMILIES_BY_NAME.values():
        if operation not in family.public_errors:
            continue
        ok, status = _result(
            case.get("result"),
            f"{case.get('id', 'case')}.result",
            family.public_errors[operation],
        )
        if ok:
            raise OracleError("successful oracle row has no public error")
        return family.public_errors[operation][status]
    raise OracleError("oracle row has an unsupported operation")


def load_fixture(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise OracleError(f"cannot read fixture {path}: {error}") from error
    return validate_fixture(value)


def _validate_oracle_build_identity(
    version: str,
    expected_commit: str = ADD_SUBTRACT_VECTORS_COMMIT,
) -> None:
    match = _BUILD_COMMIT.search(version)
    if match is None:
        raise OracleError("oracle executable does not report its git build identity")
    if match.group("commit") != expected_commit or match.group("dirty"):
        raise OracleError(
            "oracle executable was not built from the clean pinned checkout"
        )


def _validate_checkout_identity(
    commit: str,
    status: str,
    expected_commit: str,
) -> None:
    if commit != expected_commit:
        raise OracleError(
            f"xahaud-vectors checkout is {commit}, expected {expected_commit}"
        )
    if status:
        raise OracleError("xahaud-vectors checkout is dirty")


def _git(checkout: Path, *arguments: str) -> str:
    completed = subprocess.run(
        ["git", *arguments],
        cwd=checkout,
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        raise OracleError("cannot inspect xahaud-vectors checkout identity")
    return completed.stdout.strip()


def _fixture_from_source(
    source: dict[str, Any],
    family: OracleFamily,
) -> dict[str, Any]:
    common = {
        "schema": family.fixture_schema,
        "oracle": {
            "repository": ORACLE_REPOSITORY,
            "commit": family.vectors_commit,
            "suite": ORACLE_SUITE,
            "source_schema": source["schema"],
        },
        "profile": source.get("profile"),
        "xahaud_source_commit": source.get("xahaud_source_commit"),
    }
    if family is ADD_SUBTRACT_FAMILY:
        _exact_keys(
            source,
            {
                "schema",
                "profile",
                "xahaud_source_commit",
                "fix_universal_number",
                "number_so",
                "cases",
                "guard_drop_control",
            },
            family.source_key,
        )
        common.update(
            {
                "fix_universal_number": source.get("fix_universal_number"),
                "number_so": source.get("number_so"),
                "cases": copy.deepcopy(source.get("cases")),
                "guard_drop_control": copy.deepcopy(source.get("guard_drop_control")),
            }
        )
    else:
        _exact_keys(
            source,
            {
                "schema",
                "profile",
                "xahaud_source_commit",
                "fix_float_divide",
                "build",
                "multiply_cases",
                "divide_cases",
                "guard_fix_float_divide",
            },
            family.source_key,
        )
        common.update(
            {
                "fix_float_divide": source.get("fix_float_divide"),
                "build": copy.deepcopy(source.get("build")),
                "multiply_cases": copy.deepcopy(source.get("multiply_cases")),
                "divide_cases": copy.deepcopy(source.get("divide_cases")),
                "guard_fix_float_divide": copy.deepcopy(
                    source.get("guard_fix_float_divide")
                ),
            }
        )
    return validate_fixture(common)


def _capture(
    checkout: Path,
    rippled: Path,
    family: OracleFamily = ADD_SUBTRACT_FAMILY,
) -> dict[str, Any]:
    checkout = checkout.resolve()
    rippled = rippled.resolve()
    try:
        rippled.relative_to(checkout)
    except ValueError as error:
        raise OracleError(
            "oracle executable must be inside the pinned checkout"
        ) from error
    if not rippled.is_file():
        raise OracleError("oracle executable does not exist inside the pinned checkout")

    commit = _git(checkout, "rev-parse", "HEAD")
    status = _git(checkout, "status", "--porcelain")
    _validate_checkout_identity(commit, status, family.vectors_commit)

    version = subprocess.run(
        [str(rippled), "--version"],
        cwd=checkout,
        check=False,
        capture_output=True,
        text=True,
    )
    if version.returncode != 0:
        raise OracleError("oracle executable could not report its git build identity")
    _validate_oracle_build_identity(
        version.stdout + version.stderr,
        family.vectors_commit,
    )

    completed = subprocess.run(
        [
            str(rippled),
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
    if completed.stdout.count(BEGIN) != 1 or completed.stdout.count(END) != 1:
        raise OracleError("xahaud oracle must contain exactly one marker pair")
    try:
        payload = completed.stdout.split(BEGIN, 1)[1].split(END, 1)[0]
        document = _object(json.loads(payload), "oracle document")
        source = _object(document[family.source_key], family.source_key)
    except (IndexError, KeyError, json.JSONDecodeError) as error:
        raise OracleError("xahaud oracle markers or payload are malformed") from error
    if source.get("schema") != family.source_schema:
        raise OracleError("captured source schema is unsupported")
    return _fixture_from_source(source, family)


def canonical_json(value: dict[str, Any]) -> str:
    return json.dumps(value, indent=2, sort_keys=True) + "\n"


def _family_for_update(path: Path, family_name: str | None) -> OracleFamily:
    if family_name is not None:
        return FAMILIES_BY_NAME[family_name]
    if path.is_file():
        try:
            root = _object(json.loads(path.read_text()), "fixture")
        except (OSError, json.JSONDecodeError) as error:
            raise OracleError(f"cannot read fixture {path}: {error}") from error
        return _family_for_root(root)
    return ADD_SUBTRACT_FAMILY


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fixture", type=Path)
    parser.add_argument("--checkout", type=Path)
    parser.add_argument("--rippled", type=Path)
    parser.add_argument("--family", choices=sorted(FAMILIES_BY_NAME))
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
    if args.family is not None and args.checkout is None:
        parser.error("--family is only used with --checkout and --rippled")

    if args.checkout is None:
        load_fixture(args.fixture)
        return 0

    family = _family_for_update(args.fixture, args.family)
    captured = _capture(
        args.checkout.resolve(),
        args.rippled.resolve(),
        family,
    )
    rendered = canonical_json(captured)
    if args.update:
        args.fixture.write_text(rendered)
        return 0
    if canonical_json(load_fixture(args.fixture)) != rendered:
        raise OracleError("committed fixture differs from the pinned oracle")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
