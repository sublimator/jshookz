#!/usr/bin/env python3
"""Prove each hidden QuickJS edge's no-op gc_mark control goes red."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


EDGES = ("facade", "pathset", "path", "pathhop")
RED_EXIT = 23


def run(probe: Path, edge: str, mode: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(probe), edge, mode],
        check=False,
        capture_output=True,
        text=True,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True, type=Path)
    args = parser.parse_args()

    if not args.probe.is_file():
        print(f"probe does not exist: {args.probe}", file=sys.stderr)
        return 2

    for edge in EDGES:
        green = run(args.probe, edge, "enabled")
        expected_green = f"collected {edge}\n"
        if green.returncode != 0 or green.stdout != expected_green or green.stderr:
            print(
                f"green control failed for {edge}: rc={green.returncode} "
                f"stdout={green.stdout!r} stderr={green.stderr!r}",
                file=sys.stderr,
            )
            return 1

        red = run(args.probe, edge, "disabled")
        expected_red = f"retained {edge}\n"
        if red.returncode != RED_EXIT or red.stdout != expected_red or red.stderr:
            print(
                f"red control failed for {edge}: rc={red.returncode} "
                f"stdout={red.stdout!r} stderr={red.stderr!r}",
                file=sys.stderr,
            )
            return 1

    print("pathset gc red controls: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
