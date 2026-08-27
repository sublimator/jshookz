#!/usr/bin/env python3
"""Red controls for the pinned XFL arithmetic oracle reader."""

from __future__ import annotations

import copy
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "scripts" / "xfl_arithmetic_oracle.py"
ADD_SUBTRACT_FIXTURE = Path(__file__).with_name(
    "xahau_float_v1_add_subtract_oracle.json"
)
MULTIPLY_DIVIDE_FIXTURE = Path(__file__).with_name(
    "xahau_float_v1_multiply_divide_oracle.json"
)
SPEC = importlib.util.spec_from_file_location("xfl_arithmetic_oracle", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
oracle = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(oracle)


class XFLArithmeticOracleReaderTest(unittest.TestCase):
    def setUp(self) -> None:
        self.add_subtract = json.loads(ADD_SUBTRACT_FIXTURE.read_text())
        self.multiply_divide = json.loads(MULTIPLY_DIVIDE_FIXTURE.read_text())
        self.fixture = self.add_subtract

    def assert_rejected(self, value: object, message: str) -> None:
        with self.assertRaisesRegex(oracle.OracleError, message):
            oracle.validate_fixture(value)

    def test_committed_fixture_is_valid(self) -> None:
        self.assertEqual(
            len(oracle.fixture_cases(self.add_subtract)),
            len(oracle.REQUIRED_CASE_IDS),
        )
        self.assertEqual(
            len(oracle.fixture_cases(self.multiply_divide)),
            len(oracle.MULTIPLY_DIVIDE_REQUIRED_CATEGORIES),
        )

    def test_multiply_divide_fixture_has_exact_family_partition(self) -> None:
        fixture = oracle.validate_fixture(self.multiply_divide)
        self.assertEqual(len(fixture["multiply_cases"]), 25)
        self.assertEqual(len(fixture["divide_cases"]), 36)
        self.assertEqual(
            {case["operation"] for case in fixture["multiply_cases"]},
            {"multiply"},
        )
        self.assertEqual(
            {case["operation"] for case in fixture["divide_cases"]},
            {"divide"},
        )

    def test_wrong_schema_is_rejected(self) -> None:
        value = copy.deepcopy(self.fixture)
        value["schema"] = "jshookz.xahau-float-v1-add-subtract-oracle.v0"
        self.assert_rejected(value, "unsupported fixture schema")

    def test_missing_cases_are_rejected(self) -> None:
        value = copy.deepcopy(self.fixture)
        del value["cases"]
        self.assert_rejected(value, "fixture keys")

    def test_duplicate_ids_are_rejected(self) -> None:
        value = copy.deepcopy(self.fixture)
        value["cases"][1]["id"] = value["cases"][0]["id"]
        self.assert_rejected(value, "duplicate case id")

    def test_multiply_divide_duplicate_ids_are_rejected(self) -> None:
        value = copy.deepcopy(self.multiply_divide)
        value["divide_cases"][0] = copy.deepcopy(value["multiply_cases"][0])
        self.assert_rejected(value, "duplicate case id")

    def test_multiply_divide_missing_ids_are_rejected(self) -> None:
        value = copy.deepcopy(self.multiply_divide)
        value["divide_cases"].pop()
        self.assert_rejected(value, "operation matrix mismatch")

    def test_multiply_divide_extra_ids_are_rejected(self) -> None:
        value = copy.deepcopy(self.multiply_divide)
        extra = copy.deepcopy(value["multiply_cases"][0])
        extra["id"] = "multiply.unratified-extra"
        value["multiply_cases"].append(extra)
        self.assert_rejected(value, "operation matrix mismatch")

    def test_multiply_divide_wrong_category_is_rejected(self) -> None:
        value = copy.deepcopy(self.multiply_divide)
        value["multiply_cases"][0]["category"] = "sign"
        self.assert_rejected(value, "category is not the pinned category")

    def test_multiply_divide_wrong_operation_partition_is_rejected(self) -> None:
        value = copy.deepcopy(self.multiply_divide)
        value["multiply_cases"][0]["operation"] = "divide"
        self.assert_rejected(value, "operation is unsupported")

    def test_noncanonical_integer_spelling_is_rejected(self) -> None:
        value = copy.deepcopy(self.fixture)
        value["cases"][0]["a"]["val"] = "00"
        self.assert_rejected(value, "non-canonical integer spelling")

    def test_json_number_integer_leaf_is_rejected(self) -> None:
        value = copy.deepcopy(self.multiply_divide)
        value["multiply_cases"][0]["a"]["val"] = 0
        self.assert_rejected(value, "must be a typed u64 string")

    def test_stale_vector_pin_is_rejected(self) -> None:
        value = copy.deepcopy(self.fixture)
        value["oracle"]["commit"] = "0" * 40
        self.assert_rejected(value, "stale or unexpected oracle identity")

    def test_multiply_divide_stale_vector_pin_is_rejected(self) -> None:
        value = copy.deepcopy(self.multiply_divide)
        value["oracle"]["commit"] = "0" * 40
        self.assert_rejected(value, "stale or unexpected oracle identity")

    def test_stale_source_pin_is_rejected(self) -> None:
        value = copy.deepcopy(self.fixture)
        value["xahaud_source_commit"] = "0" * 40
        self.assert_rejected(value, "stale xahaud source identity")

    def test_multiply_divide_stale_build_pin_is_rejected(self) -> None:
        value = copy.deepcopy(self.multiply_divide)
        value["build"]["commit_hash"] = "0" * 40
        self.assert_rejected(value, "oracle build identity")

    def test_multiply_divide_dirty_build_is_rejected(self) -> None:
        value = copy.deepcopy(self.multiply_divide)
        value["build"]["commit"] += "-dirty"
        value["build"]["dirty"] = True
        self.assert_rejected(value, "oracle build identity")

    def test_multiply_divide_wrong_mode_is_rejected(self) -> None:
        value = copy.deepcopy(self.multiply_divide)
        value["fix_float_divide"] = False
        self.assert_rejected(value, "must be captured with fixFloatDivide")

    def test_multiply_divide_unguarded_capture_is_rejected(self) -> None:
        value = copy.deepcopy(self.multiply_divide)
        value["guard_fix_float_divide"]["fix_false"] = copy.deepcopy(
            value["guard_fix_float_divide"]["fix_true"]
        )
        self.assert_rejected(value, "does not distinguish arithmetic paths")

    def test_multiply_divide_wrong_public_error_mapping_is_rejected(self) -> None:
        value = copy.deepcopy(self.multiply_divide)
        overflow = next(
            case
            for case in value["multiply_cases"]
            if case["id"] == "multiply.overflow"
        )
        overflow["result"]["error"]["val"] = "-25"
        self.assert_rejected(value, "not accepted for this operation")

    def test_public_error_mapping_is_operation_specific(self) -> None:
        cases = {
            case["id"]: case for case in oracle.fixture_cases(self.multiply_divide)
        }
        self.assertEqual(oracle.public_error(cases["multiply.overflow"]), "overflow")
        self.assertEqual(
            oracle.public_error(cases["divide.denominator-zero"]),
            "division-by-zero",
        )
        self.assertEqual(
            oracle.public_error(cases["divide.max-exponent-numerator-generic"]),
            "invalid",
        )

    def test_oracle_executable_outside_pinned_checkout_is_rejected(self) -> None:
        with (
            tempfile.TemporaryDirectory() as checkout_text,
            tempfile.TemporaryDirectory() as outside_text,
        ):
            checkout = Path(checkout_text)
            outside = Path(outside_text) / "unrelated-rippled"
            outside.touch()
            with self.assertRaisesRegex(
                oracle.OracleError,
                "oracle executable must be inside the pinned checkout",
            ):
                oracle._capture(checkout, outside)

    def test_oracle_executable_without_build_identity_is_rejected(self) -> None:
        with self.assertRaisesRegex(
            oracle.OracleError,
            "does not report its git build identity",
        ):
            oracle._validate_oracle_build_identity("xahaud version CustomBuild\n")

    def test_oracle_executable_from_wrong_commit_is_rejected(self) -> None:
        with self.assertRaisesRegex(
            oracle.OracleError,
            "was not built from the clean pinned checkout",
        ):
            oracle._validate_oracle_build_identity(
                "Git commit hash: " + "0" * 40 + "\n"
            )

    def test_oracle_executable_from_dirty_checkout_is_rejected(self) -> None:
        with self.assertRaisesRegex(
            oracle.OracleError,
            "was not built from the clean pinned checkout",
        ):
            oracle._validate_oracle_build_identity(
                f"Git commit hash: {oracle.XAHAUD_VECTORS_COMMIT}-dirty\n"
            )

    def test_oracle_executable_from_clean_pinned_checkout_is_accepted(self) -> None:
        oracle._validate_oracle_build_identity(
            f"Git commit hash: {oracle.XAHAUD_VECTORS_COMMIT}\n"
        )

    def test_multiply_divide_clean_build_identity_is_accepted(self) -> None:
        oracle._validate_oracle_build_identity(
            f"Git commit hash: {oracle.MULTIPLY_DIVIDE_VECTORS_COMMIT}\n",
            oracle.MULTIPLY_DIVIDE_VECTORS_COMMIT,
        )

    def test_dirty_current_checkout_is_rejected(self) -> None:
        with self.assertRaisesRegex(oracle.OracleError, "checkout is dirty"):
            oracle._validate_checkout_identity(
                oracle.MULTIPLY_DIVIDE_VECTORS_COMMIT,
                " M uncommitted.cpp",
                oracle.MULTIPLY_DIVIDE_VECTORS_COMMIT,
            )


if __name__ == "__main__":
    unittest.main()
