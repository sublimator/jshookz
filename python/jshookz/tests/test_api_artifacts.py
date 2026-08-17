"""Closed public Hook API artifact integrity checks."""

import hashlib
import json
import shutil
from pathlib import Path

import pytest
from jshookz.api_artifacts import (
    REQUIRED_ARTIFACTS,
    V1_SURFACE,
    validate_api_artifacts,
)
from jshookz.paths import API_ARTIFACT_MANIFEST

REPOSITORY = API_ARTIFACT_MANIFEST.parents[5]


def _fixture(tmp_path: Path) -> tuple[Path, Path, dict[str, object]]:
    for relative in REQUIRED_ARTIFACTS:
        target = tmp_path / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(REPOSITORY / relative, target)
    manifest = json.loads(API_ARTIFACT_MANIFEST.read_text())
    manifest_path = tmp_path / "api-artifacts.json"
    manifest_path.write_text(json.dumps(manifest))
    return tmp_path, manifest_path, manifest


def test_checked_api_artifact_manifest_is_complete():
    manifest = validate_api_artifacts(REPOSITORY, API_ARTIFACT_MANIFEST)
    assert frozenset(manifest["artifacts"]) == REQUIRED_ARTIFACTS


def test_shipped_api_artifacts_do_not_name_the_authoring_system():
    forbidden = (".ai-docs", "@publish-v1", "hooks-api.source.d.ts", "jzhookzdev")
    product_source = API_ARTIFACT_MANIFEST.parents[1]
    for path in product_source.rglob("*"):
        if not path.is_file() or not str(path).endswith(
            (".d.ts", ".js", ".json", ".py")
        ):
            continue
        text = path.read_text()
        assert not any(marker in text for marker in forbidden), path


def test_api_artifact_manifest_rejects_truncated_set(tmp_path: Path):
    root, manifest_path, manifest = _fixture(tmp_path)
    del manifest["artifacts"][V1_SURFACE]
    manifest_path.write_text(json.dumps(manifest))

    with pytest.raises(ValueError, match="artifact set differs"):
        validate_api_artifacts(root, manifest_path)


def test_api_artifact_manifest_cross_checks_surface_declaration_hash(tmp_path: Path):
    root, manifest_path, manifest = _fixture(tmp_path)
    surface_path = root / V1_SURFACE
    surface = json.loads(surface_path.read_text())
    surface["declaration"]["sha256"] = "00" * 32
    surface_path.write_text(json.dumps(surface))
    # Keep the surface artifact hash current to isolate the declaration join.
    manifest["artifacts"][V1_SURFACE] = hashlib.sha256(
        surface_path.read_bytes()
    ).hexdigest()
    manifest_path.write_text(json.dumps(manifest))

    with pytest.raises(ValueError, match="declaration hash differs"):
        validate_api_artifacts(root, manifest_path)
