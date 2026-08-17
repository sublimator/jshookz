"""Shared paths for native x-data-quickjs tests."""

from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent.parent
CODEC = REPO / "cpp" / "x-data-quickjs"
