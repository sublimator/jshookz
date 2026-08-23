#!/usr/bin/env python3
"""Start one real poison closure and prove the production gate rejects it."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--python", required=True)
    parser.add_argument("--checker", type=Path, required=True)
    parser.add_argument("--inventory", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--nm", required=True)
    parser.add_argument("--expect", action="append", default=[])
    args = parser.parse_args()

    started = subprocess.run(
        [str(args.binary)], capture_output=True, text=True, check=False
    )
    if started.returncode != 0:
        print(
            f"poison executable did not start cleanly ({started.returncode}): "
            f"{started.stderr}",
            file=sys.stderr,
        )
        return 1

    checked = subprocess.run(
        [
            args.python,
            str(args.checker),
            "--inventory",
            str(args.inventory),
            "--binary",
            str(args.binary),
            "--nm",
            args.nm,
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    report = checked.stdout + checked.stderr
    if checked.returncode == 0:
        print("production provider-static gate accepted poison", file=sys.stderr)
        return 1
    missing = [label for label in args.expect if label not in report]
    if missing:
        print(
            f"gate failed for the wrong reason; missing {missing}:\n{report}",
            file=sys.stderr,
        )
        return 1
    print(f"compiled poison rejected after successful start: {args.binary.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
