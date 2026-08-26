#!/usr/bin/env python3
"""Run the independent Xahau fixture through native and QuickJS kernels."""

from __future__ import annotations

import argparse
import importlib.util
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "scripts" / "xfl_arithmetic_oracle.py"
SPEC = importlib.util.spec_from_file_location("xfl_arithmetic_oracle", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
oracle = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(oracle)


def expected(case: dict[str, object]) -> str:
    result = case["result"]
    assert isinstance(result, dict)
    if result["ok"] is True:
        value = result["value"]
        assert isinstance(value, dict)
        return str(value["val"])
    return "error:overflow"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fixture", type=Path)
    parser.add_argument("probe", type=Path)
    args = parser.parse_args()
    fixture = oracle.load_fixture(args.fixture)

    failures: list[str] = []
    for mode in ("native", "quickjs"):
        for case in fixture["cases"]:
            completed = subprocess.run(
                [
                    str(args.probe),
                    mode,
                    case["operation"],
                    case["a"]["val"],
                    case["b"]["val"],
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            actual = completed.stdout.strip()
            wanted = expected(case)
            if completed.returncode != 0 or actual != wanted:
                failures.append(
                    f"{mode}:{case['id']}: rc={completed.returncode} "
                    f"expected={wanted!r} actual={actual!r} stderr={completed.stderr!r}"
                )
    if failures:
        raise AssertionError("\n".join(failures))
    print(f"XFL oracle: {len(fixture['cases'])} rows x native/QuickJS passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
