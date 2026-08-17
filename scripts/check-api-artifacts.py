"""Verify the closed public Hook API artifact set."""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python/jshookz/src"))

from jshookz.api_artifacts import (
    REQUIRED_ARTIFACTS,
    validate_api_artifacts,
)
from jshookz.paths import API_ARTIFACT_MANIFEST


def main() -> int:
    try:
        validate_api_artifacts(ROOT, API_ARTIFACT_MANIFEST)
    except (OSError, TypeError, ValueError) as error:
        print(f"API artifacts: {error}", file=sys.stderr)
        return 1
    for relative in sorted(REQUIRED_ARTIFACTS):
        print(f"{relative}: current")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
