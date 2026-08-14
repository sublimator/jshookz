#!/usr/bin/env python3
"""Project typechecked TypeScript examples into README.md.

The source files remain ordinary compilable examples. This script owns only
the marked README regions, so documentation cannot quietly diverge from the
programs exercised by the TypeScript compiler and product tests.

Usage:
    ./scripts/project-readme-examples.py          # update README.md
    ./scripts/project-readme-examples.py --check  # fail if README.md is stale
"""

from __future__ import annotations

import argparse
import difflib
import json
import sys
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
README = ROOT / "README.md"
TSCONFIG = ROOT / "packages/hostem/tsconfig.xahau-integration.json"


@dataclass(frozen=True)
class Projection:
    marker: str
    source: Path
    language: str


PROJECTIONS = (
    Projection(
        "xahau-accept.hook.ts",
        ROOT / "packages/hostem/examples/xahau-accept.hook.ts",
        "ts",
    ),
    Projection(
        "xahau-state.hook.ts",
        ROOT / "packages/hostem/examples/xahau-state.hook.ts",
        "ts",
    ),
    Projection(
        "xahau-state-batch.hook.ts",
        ROOT / "packages/hostem/examples/xahau-state-batch.hook.ts",
        "ts",
    ),
)


def _assert_typechecked(projection: Projection) -> None:
    config = json.loads(TSCONFIG.read_text())
    included = {
        (TSCONFIG.parent / entry).resolve()
        for entry in config.get("files", ())
    }
    if projection.source.resolve() not in included:
        raise ValueError(
            f"{projection.source.relative_to(ROOT)} is projected into README.md "
            f"but absent from {TSCONFIG.relative_to(ROOT)}"
        )


def _replace_region(document: str, projection: Projection) -> str:
    start = f"<!-- BEGIN GENERATED: {projection.marker} -->"
    end = f"<!-- END GENERATED: {projection.marker} -->"
    if document.count(start) != 1 or document.count(end) != 1:
        raise ValueError(
            f"README.md must contain exactly one {projection.marker!r} marker pair"
        )
    before, remainder = document.split(start, 1)
    _, after = remainder.split(end, 1)
    source = projection.source.read_text().rstrip()
    generated = f"{start}\n```{projection.language}\n{source}\n```\n{end}"
    return before + generated + after


def render() -> str:
    document = README.read_text()
    for projection in PROJECTIONS:
        _assert_typechecked(projection)
        document = _replace_region(document, projection)
    return document


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    current = README.read_text()
    expected = render()
    if args.check:
        if current == expected:
            print("README.md examples: current and typechecked")
            return 0
        sys.stderr.writelines(
            difflib.unified_diff(
                current.splitlines(keepends=True),
                expected.splitlines(keepends=True),
                fromfile="README.md",
                tofile="generated",
            )
        )
        return 1

    if current != expected:
        README.write_text(expected)
        print("updated README.md examples")
    else:
        print("README.md examples: already current")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
