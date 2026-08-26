#!/usr/bin/env python3
"""Red controls for the pinned XFL arithmetic oracle reader."""

from __future__ import annotations

import copy
import importlib.util
import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "scripts" / "xfl_arithmetic_oracle.py"
FIXTURE = Path(__file__).with_name("xahau_float_v1_add_subtract_oracle.json")
SPEC = importlib.util.spec_from_file_location("xfl_arithmetic_oracle", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
oracle = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(oracle)


class XFLArithmeticOracleReaderTest(unittest.TestCase):
    def setUp(self) -> None:
        self.fixture = json.loads(FIXTURE.read_text())

    def assert_rejected(self, value: object, message: str) -> None:
        with self.assertRaisesRegex(oracle.OracleError, message):
            oracle.validate_fixture(value)

    def test_committed_fixture_is_valid(self) -> None:
        self.assertEqual(
            len(oracle.validate_fixture(self.fixture)["cases"]),
            len(oracle.REQUIRED_CASE_IDS),
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

    def test_noncanonical_integer_spelling_is_rejected(self) -> None:
        value = copy.deepcopy(self.fixture)
        value["cases"][0]["a"]["val"] = "00"
        self.assert_rejected(value, "non-canonical integer spelling")

    def test_stale_vector_pin_is_rejected(self) -> None:
        value = copy.deepcopy(self.fixture)
        value["oracle"]["commit"] = "0" * 40
        self.assert_rejected(value, "stale or unexpected oracle identity")

    def test_stale_source_pin_is_rejected(self) -> None:
        value = copy.deepcopy(self.fixture)
        value["xahaud_source_commit"] = "0" * 40
        self.assert_rejected(value, "stale xahaud source identity")


if __name__ == "__main__":
    unittest.main()
