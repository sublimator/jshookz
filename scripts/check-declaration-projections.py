#!/usr/bin/env python3
"""Verify the public declaration artifacts against their private-source receipt."""

from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RECEIPT = ROOT / "packages/jshookz/src/jshookz/types/projection-receipt.json"
SCHEMA = "jshookz.declaration-projection-receipt.v1"


def main() -> int:
    payload = json.loads(RECEIPT.read_text())
    if payload.get("schema") != SCHEMA:
        print(f"{RECEIPT.relative_to(ROOT)}: unsupported schema", file=sys.stderr)
        return 1
    artifacts = payload.get("artifacts")
    if not isinstance(artifacts, dict) or not artifacts:
        print(f"{RECEIPT.relative_to(ROOT)}: missing artifacts", file=sys.stderr)
        return 1
    result = 0
    for relative, expected in sorted(artifacts.items()):
        path = (ROOT / relative).resolve()
        if not path.is_relative_to(ROOT) or not path.is_file():
            print(f"{relative}: missing or outside repository", file=sys.stderr)
            result = 1
            continue
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        if actual != expected:
            print(f"{relative}: projection receipt mismatch", file=sys.stderr)
            result = 1
        else:
            print(f"{relative}: receipt current")
    return result


if __name__ == "__main__":
    raise SystemExit(main())
