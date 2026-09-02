"""The exported consumer bundle is exactly what xahaud's cmake and generator take."""

from __future__ import annotations

import dataclasses
import hashlib
import json
from pathlib import Path

import pytest

from jshookz import build


# Mirror of xahaud cmake/QuickJSProvider.cmake's required bundle files plus the
# consumer lock its GenerateQuickJSProviderBundle.py parses. Changing either
# side without the other is a consumer break, so keep this list literal.
CONSUMER_BUNDLE_FILES = frozenset(
    {
        "jshookz_provider.lock.json",
        "jshookz_provider.manifest.json",
        "jshookz_provider.native-abi.json",
        "jshookz_provider.wasm",
        "api-artifacts.json",
        "hooks-api.d.ts",
        "xahau-quickjs-v1-consensus-entropy.d.ts",
        "xahau-quickjs-v1-consensus-entropy.surface.json",
        "xahau-quickjs-v1.d.ts",
        "xahau-quickjs-v1.surface.json",
        "xfl-profile-ledger.ts",
    }
)
CONSUMER_LOCK_KEYS = frozenset(
    {
        "api_artifacts",
        "bytecode_abi_id",
        "manifest",
        "native_abi",
        "product",
        "provider",
        "runtime_profile_id",
        "schema",
        "wasmtime_version",
    }
)
CONSUMER_LOCK_SECTIONS = {
    "manifest": frozenset({"file", "schema", "sha256"}),
    "provider": frozenset({"file", "sha256", "size"}),
    "native_abi": frozenset({"file", "sha256"}),
    "api_artifacts": frozenset({"file", "sha256"}),
}


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


@pytest.fixture(scope="module")
def exported(tmp_path_factory: pytest.TempPathFactory) -> Path:
    return build.export_consumer_bundle(
        destination=tmp_path_factory.mktemp("consumer-bundle")
    )


def test_bundle_is_exactly_the_consumer_file_set(exported: Path) -> None:
    assert {path.name for path in exported.iterdir()} == CONSUMER_BUNDLE_FILES


def test_lock_matches_the_consumer_contract_and_the_bundle_bytes(
    exported: Path,
) -> None:
    lock = json.loads((exported / "jshookz_provider.lock.json").read_text())
    manifest = json.loads((exported / "jshookz_provider.manifest.json").read_text())

    assert set(lock) == CONSUMER_LOCK_KEYS
    assert lock["schema"] == build.CONSUMER_LOCK_SCHEMA
    for key, expected in CONSUMER_LOCK_SECTIONS.items():
        assert set(lock[key]) == expected, key
        assert Path(lock[key]["file"]).name == lock[key]["file"]
        assert (exported / lock[key]["file"]).is_file()

    assert lock["product"] == "provider"
    assert lock["bytecode_abi_id"] == manifest["bytecode_abi_id"]
    assert lock["runtime_profile_id"] == manifest["runtime_profile_id"]
    assert lock["wasmtime_version"] == manifest["source"]["engine"]["version"]
    assert lock["manifest"]["schema"] == manifest["schema"]

    wasm = exported / "jshookz_provider.wasm"
    assert lock["provider"]["sha256"] == _sha256(wasm) == manifest["provider"]["sha256"]
    assert (
        lock["provider"]["size"] == wasm.stat().st_size == manifest["provider"]["size"]
    )
    for key in ("manifest", "native_abi", "api_artifacts"):
        assert lock[key]["sha256"] == _sha256(exported / lock[key]["file"]), key


def test_api_artifacts_are_the_tracked_files_byte_for_byte(exported: Path) -> None:
    for name, source in build.CONSUMER_BUNDLE_API_ARTIFACTS.items():
        assert (exported / name).read_bytes() == source.read_bytes(), name


def test_export_is_deterministic(exported: Path, tmp_path: Path) -> None:
    again = build.export_consumer_bundle(destination=tmp_path / "again")
    for name in CONSUMER_BUNDLE_FILES:
        assert (again / name).read_bytes() == (exported / name).read_bytes(), name


def test_default_bundle_roots_are_disjoint_per_product() -> None:
    roots = {product.bundle_dir.resolve() for product in build.PRODUCTS.values()}
    assert len(roots) == len(build.PRODUCTS)
    for product in build.PRODUCTS.values():
        bundle = product.bundle_dir.resolve()
        for other in build.PRODUCTS.values():
            assert not bundle.is_relative_to(other.build_dir.resolve())
            assert not other.build_dir.resolve().is_relative_to(bundle)


def _fake_product(monkeypatch: pytest.MonkeyPatch, build_dir: Path) -> None:
    baseline = build.PRODUCTS[build.BASELINE_PRODUCT]
    monkeypatch.setitem(
        build.PRODUCTS,
        build.BASELINE_PRODUCT,
        dataclasses.replace(
            baseline, build_dir=build_dir, bundle_dir=build_dir / "bundle"
        ),
    )


def test_export_refuses_an_unbuilt_product(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    _fake_product(monkeypatch, tmp_path / "empty")
    with pytest.raises(RuntimeError, match="jshookz build provider"):
        build.export_consumer_bundle(destination=tmp_path / "out")


def test_export_refuses_a_seal_that_does_not_match_its_manifest(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    baseline = build.PRODUCTS[build.BASELINE_PRODUCT]
    stale = tmp_path / "stale"
    stale.mkdir()
    for name in build.SEALED_BUNDLE_FILES:
        (stale / name).write_bytes((baseline.build_dir / name).read_bytes())
    wasm = stale / "jshookz_provider.wasm"
    wasm.write_bytes(wasm.read_bytes() + b"\0")
    _fake_product(monkeypatch, stale)
    with pytest.raises(RuntimeError, match="does not match its manifest"):
        build.export_consumer_bundle(destination=tmp_path / "out")
    assert not (tmp_path / "out").exists()
