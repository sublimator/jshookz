#!/usr/bin/env python3
"""Require green liveness and a deliberate red for every hidden object edge."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


EDGES = (
    "object-owner",
    "object-cache-value",
    "array-owner",
    "array-cache-value",
    "iterator-array",
    "field-table-descriptor",
    "blob-owner",
    "hash256-owner",
    "account-id-owner",
    "amount-owner",
    "hash128-owner",
    "hash160-owner",
    "hash192-owner",
    "currency-owner",
    "issue-owner",
    "issue-cache-value",
    "vector256-owner",
    "vector256-cache-value",
    "vector256-iterator",
    "xchain-bridge-owner",
    "xchain-bridge-cache-value",
    "path-set-owner",
    "path-parent",
    "path-hop-parent",
    "path-iterator-parent",
)
RED_EXIT = 23
RADIX_STDOUT = """\
radix one-page B=1 P=1 M=32 bytes=1040 allocations=3
radix one-branch B=1 P=32 M=1024 bytes=16912 allocations=34
radix all-branches B=32 P=32 M=1024 bytes=24848 allocations=65
radix all-pages B=32 P=1024 M=1024 bytes=532752 allocations=1057
radix maximum B=32 P=1024 M=32767 bytes=532752 allocations=1057
radix maximum wire=1048576 fields=32768 scopes=32769 length=32767
"""


def run(probe: Path, *args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(probe), *args],
        check=False,
        capture_output=True,
        text=True,
    )


def require(
    result: subprocess.CompletedProcess[str],
    *,
    returncode: int,
    stdout: str,
    label: str,
) -> bool:
    if (
        result.returncode == returncode
        and result.stdout == stdout
        and not result.stderr
    ):
        return True
    print(
        f"{label} failed: rc={result.returncode} "
        f"stdout={result.stdout!r} stderr={result.stderr!r}",
        file=sys.stderr,
    )
    return False


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True, type=Path)
    args = parser.parse_args()

    if not args.probe.is_file():
        print(f"probe does not exist: {args.probe}", file=sys.stderr)
        return 2

    for edge in EDGES:
        if not require(
            run(args.probe, edge, "enabled"),
            returncode=0,
            stdout=f"collected {edge}\n",
            label=f"green control for {edge}",
        ):
            return 1
        if not require(
            run(args.probe, edge, "disabled"),
            returncode=RED_EXIT,
            stdout=f"retained {edge}\n",
            label=f"red control for {edge}",
        ):
            return 1

    if not require(
        run(args.probe, "lifetime-order"),
        returncode=0,
        stdout="lifetime order: ok\n",
        label="parent/child/iterator finalizer order",
    ):
        return 1

    if not require(
        run(args.probe, "radix-topology"),
        returncode=0,
        stdout=RADIX_STDOUT,
        label="exact STArray 5/5/5 radix topology",
    ):
        return 1
    if not require(
        run(args.probe, "radix-topology-count-poison"),
        returncode=RED_EXIT,
        stdout="radix topology-count poison detected\n",
        label="radix topology-count red control",
    ):
        return 1
    if not require(
        run(args.probe, "radix-hit-allocation-poison"),
        returncode=RED_EXIT,
        stdout="radix hit-allocation poison detected\n",
        label="radix hit-allocation red control",
    ):
        return 1

    print(
        "object GC/lifetime/radix controls: ok "
        "(25 green, 25 GC red, lifetime order, 5 topology banks, 2 radix red)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
