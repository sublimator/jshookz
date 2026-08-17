"""Strict integrity checks for the closed, shipped Hook API artifact set."""

from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path
from typing import Any

from .paths import API_ARTIFACT_MANIFEST

MANIFEST_SCHEMA = "jshookz.api-artifacts.v1"
MANIFEST_FIELDS = frozenset({"artifacts", "schema"})
REQUIRED_ARTIFACTS = frozenset(
    {
        "python/jshookz/src/jshookz/types/hooks-api.d.ts",
        "python/jshookz/src/jshookz/types/xahau-quickjs-v1.d.ts",
        "python/jshookz/src/jshookz/types/xahau-quickjs-v1.surface.json",
    }
)
V1_DECLARATION = "python/jshookz/src/jshookz/types/xahau-quickjs-v1.d.ts"
V1_SURFACE = "python/jshookz/src/jshookz/types/xahau-quickjs-v1.surface.json"
_SHA256 = re.compile(r"[0-9a-f]{64}")


def _digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _require_digest(value: Any, label: str) -> str:
    if not isinstance(value, str) or _SHA256.fullmatch(value) is None:
        raise ValueError(f"{label} must be a lowercase SHA-256 digest")
    return value


def validate_api_artifacts(
    repository: str | Path,
    manifest_path: str | Path = API_ARTIFACT_MANIFEST,
) -> dict[str, Any]:
    """Validate the exact artifact set, hashes, and surface/declaration join."""
    root = Path(repository).resolve()
    manifest = Path(manifest_path)
    if not manifest.is_absolute():
        manifest = root / manifest
    manifest = manifest.resolve()
    if not manifest.is_relative_to(root) or not manifest.is_file():
        raise ValueError("API artifact manifest is missing or outside the repository")

    payload = json.loads(manifest.read_text())
    if not isinstance(payload, dict):
        raise TypeError("API artifact manifest must be a JSON object")
    fields = frozenset(payload)
    if fields != MANIFEST_FIELDS:
        missing = sorted(MANIFEST_FIELDS - fields)
        extra = sorted(fields - MANIFEST_FIELDS)
        raise ValueError(
            f"API artifact manifest fields differ: missing={missing}, extra={extra}"
        )
    if payload["schema"] != MANIFEST_SCHEMA:
        raise ValueError(f"unsupported API artifact schema: {payload['schema']!r}")

    artifacts = payload["artifacts"]
    if not isinstance(artifacts, dict):
        raise TypeError("API artifacts must be a JSON object")
    artifact_names = frozenset(artifacts)
    if artifact_names != REQUIRED_ARTIFACTS:
        missing = sorted(REQUIRED_ARTIFACTS - artifact_names)
        extra = sorted(artifact_names - REQUIRED_ARTIFACTS)
        raise ValueError(
            f"API artifact set differs: missing={missing}, extra={extra}"
        )

    for relative in sorted(REQUIRED_ARTIFACTS):
        expected = _require_digest(artifacts[relative], f"artifact {relative}")
        target = (root / relative).resolve()
        if not target.is_relative_to(root) or not target.is_file():
            raise ValueError(f"API artifact is missing or outside repository: {relative}")
        if _digest(target) != expected:
            raise ValueError(f"API artifact hash mismatch: {relative}")

    surface = json.loads((root / V1_SURFACE).read_text())
    if not isinstance(surface, dict):
        raise TypeError("v1 JavaScript surface must be a JSON object")
    declaration = surface.get("declaration")
    if not isinstance(declaration, dict):
        raise TypeError("v1 JavaScript surface has no declaration identity")
    if declaration.get("path") != V1_DECLARATION:
        raise ValueError("v1 JavaScript surface names the wrong declaration")
    if declaration.get("sha256") != artifacts[V1_DECLARATION]:
        raise ValueError("v1 JavaScript surface declaration hash differs from manifest")
    return payload
