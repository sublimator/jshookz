"""The exported consumer bundle is exactly what xahaud's build consumes.

xahaud reads the receipt with file(STRINGS), verifies each pinned digest with
file(SHA256), compiles the values file as it is, and embeds the wasm itself.
These tests hold the producer to that contract.
"""

from __future__ import annotations

import dataclasses
import hashlib
import json
import re
from pathlib import Path

import pytest

from jshookz import build, consumer_bundle


BUNDLE_FILES = frozenset(
    {
        "jshookz_provider.receipt",
        "jshookz_provider.values.cpp",
        "jshookz_provider.wasm",
        "jshookz_provider.manifest.json",
        "jshookz_provider.native-abi.json",
        "api-artifacts.json",
        "hooks-api.d.ts",
        "xahau-quickjs-v1-consensus-entropy.d.ts",
        "xahau-quickjs-v1-consensus-entropy.surface.json",
        "xahau-quickjs-v1.d.ts",
        "xahau-quickjs-v1.surface.json",
        "xfl-profile-ledger.ts",
    }
)
RECEIPT_KEYS = frozenset(
    {
        "api_artifacts_file",
        "api_artifacts_sha256",
        "broad_declaration_sha256",
        "bytecode_abi_id",
        "exact_v1_declaration_sha256",
        "heap_bytes",
        "hook_api_version",
        "host_adapter_policy",
        "host_work_base_per_call",
        "host_work_budget",
        "host_work_meter",
        "host_work_per_addressed_byte",
        "initialization_fuel",
        "invocation_fuel",
        "manifest_file",
        "manifest_schema",
        "manifest_sha256",
        "native_abi_file",
        "native_abi_sha256",
        "product",
        "provider_export_count",
        "provider_file",
        "provider_import_count",
        "provider_memory_maximum_pages",
        "provider_memory_minimum_pages",
        "provider_sha256",
        "provider_size",
        "runtime_profile_id",
        "schema",
        "selected_surface_sha256",
        "serialized_object_max_bytes",
        "serialized_object_max_depth",
        "serialized_object_max_fields",
        "serialized_object_max_scopes",
        "stack_bytes",
        "values_file",
        "values_sha256",
        "wasm_stack_bytes",
        "wasmtime_version",
        "xfl_profile_ledger_sha256",
    }
)
RECEIPT_LINE = re.compile(r"^[a-z0-9_]+ \S+$")


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


@pytest.fixture(scope="module")
def exported(tmp_path_factory: pytest.TempPathFactory) -> Path:
    return consumer_bundle.export(
        build.BASELINE_PRODUCT, tmp_path_factory.mktemp("consumer-bundle")
    )


@pytest.fixture(scope="module")
def receipt(exported: Path) -> dict[str, str]:
    return consumer_bundle.parse_receipt(
        (exported / "jshookz_provider.receipt").read_text()
    )


def test_bundle_is_exactly_the_consumer_file_set(exported: Path) -> None:
    assert {path.name for path in exported.iterdir()} == BUNDLE_FILES
    assert BUNDLE_FILES == consumer_bundle.BUNDLE_FILES


def test_receipt_is_flat_sorted_and_complete(exported: Path) -> None:
    text = (exported / "jshookz_provider.receipt").read_text()
    lines = text.split("\n")
    assert lines[-1] == "" and "" not in lines[:-1]
    assert all(RECEIPT_LINE.match(line) for line in lines[:-1]), lines
    keys = [line.split(" ", 1)[0] for line in lines[:-1]]
    assert keys == sorted(keys) and len(keys) == len(set(keys))
    assert set(keys) == RECEIPT_KEYS
    assert "\t" not in text and "#" not in text


def test_receipt_pins_match_the_bundle_bytes(
    exported: Path, receipt: dict[str, str]
) -> None:
    manifest = json.loads((exported / "jshookz_provider.manifest.json").read_text())
    assert receipt["schema"] == consumer_bundle.RECEIPT_SCHEMA
    assert receipt["product"] == "provider"
    for key in ("provider_file", "manifest_file", "native_abi_file", "values_file"):
        assert (exported / receipt[key]).is_file(), key
    assert receipt["provider_sha256"] == _sha256(exported / receipt["provider_file"])
    assert (
        int(receipt["provider_size"])
        == (exported / "jshookz_provider.wasm").stat().st_size
    )
    assert receipt["provider_sha256"] == manifest["provider"]["sha256"]
    assert receipt["manifest_sha256"] == _sha256(exported / receipt["manifest_file"])
    assert receipt["manifest_schema"] == manifest["schema"]
    assert receipt["native_abi_sha256"] == _sha256(
        exported / receipt["native_abi_file"]
    )
    assert receipt["api_artifacts_sha256"] == _sha256(exported / "api-artifacts.json")
    assert receipt["values_sha256"] == _sha256(exported / receipt["values_file"])
    assert receipt["broad_declaration_sha256"] == _sha256(exported / "hooks-api.d.ts")
    assert receipt["exact_v1_declaration_sha256"] == _sha256(
        exported / "xahau-quickjs-v1.d.ts"
    )
    assert receipt["selected_surface_sha256"] == _sha256(
        exported / "xahau-quickjs-v1.surface.json"
    )
    assert receipt["xfl_profile_ledger_sha256"] == _sha256(
        exported / "xfl-profile-ledger.ts"
    )
    assert receipt["bytecode_abi_id"] == manifest["bytecode_abi_id"]
    assert receipt["runtime_profile_id"] == manifest["runtime_profile_id"]
    assert receipt["wasmtime_version"] == manifest["source"]["engine"]["version"]
    assert int(receipt["provider_import_count"]) == len(manifest["provider"]["imports"])
    assert int(receipt["provider_export_count"]) == len(manifest["provider"]["exports"])
    limits = manifest["source"]["limits"]
    assert (
        int(receipt["initialization_fuel"])
        == limits["wasmtime_fuel_per_initialization"]
    )
    assert int(receipt["invocation_fuel"]) == limits["wasmtime_fuel_per_invocation"]
    assert int(receipt["heap_bytes"]) == limits["quickjs_heap_bytes"]
    assert (
        int(receipt["wasm_stack_bytes"])
        == manifest["provider"]["build"]["wasm_stack_bytes"]
    )
    assert (
        int(receipt["provider_memory_maximum_pages"]) * 65536
        == (manifest["provider"]["build"]["wasm_memory_max_bytes"])
    )


def test_values_file_defines_the_consumer_profile_without_the_wasm(
    exported: Path, receipt: dict[str, str]
) -> None:
    values = (exported / "jshookz_provider.values.cpp").read_text()
    manifest = json.loads((exported / "jshookz_provider.manifest.json").read_text())
    sha = receipt["provider_sha256"]
    first_bytes = ", ".join(f"0x{sha[i : i + 2]}" for i in range(0, 8, 2))
    assert first_bytes in values
    assert 'std::string_view const providerProduct = "provider";' in values
    assert f"std::size_t const providerSize = {receipt['provider_size']};" in values
    assert f'providerManifestSHA256 = "{receipt["manifest_sha256"]}"' in values
    assert f'nativeABISHA256 = "{receipt["native_abi_sha256"]}"' in values
    names = ", ".join(f'"{row["name"]}"' for row in manifest["provider"]["imports"])
    assert names in values
    assert "nativeABICatalogueCount = 75;" in values
    assert "namespace hook::artifact::generated {" in values
    assert "embeddedQuickJSProvider" not in values
    assert "sealedProvider" not in values
    assert "\\x" not in values


def test_api_artifacts_are_the_tracked_files_byte_for_byte(exported: Path) -> None:
    for name, (source, _) in consumer_bundle.API_ARTIFACTS.items():
        assert (exported / name).read_bytes() == source.read_bytes(), name


def test_export_is_deterministic(exported: Path, tmp_path: Path) -> None:
    again = consumer_bundle.export(build.BASELINE_PRODUCT, tmp_path / "again")
    for name in BUNDLE_FILES:
        assert (again / name).read_bytes() == (exported / name).read_bytes(), name


def test_export_retires_the_previous_contract_files(tmp_path: Path) -> None:
    target = tmp_path / "pin"
    target.mkdir()
    (target / "jshookz_provider.lock.json").write_text("{}")
    (target / "jshookz_provider.manifest.cmake").write_text("set(X 1)\n")
    consumer_bundle.export(build.BASELINE_PRODUCT, target)
    assert not (target / "jshookz_provider.lock.json").exists()
    assert not (target / "jshookz_provider.manifest.cmake").exists()
    assert {path.name for path in target.iterdir()} == BUNDLE_FILES


def test_export_refuses_a_destination_holding_strangers(tmp_path: Path) -> None:
    target = tmp_path / "pin"
    target.mkdir()
    (target / "README.md").write_text("stale\n")
    (target / "jshookz_provider.wasm.orig").write_bytes(b"\0")
    with pytest.raises(RuntimeError, match="README.md, jshookz_provider.wasm.orig"):
        consumer_bundle.export(build.BASELINE_PRODUCT, target)
    assert {path.name for path in target.iterdir()} == {
        "README.md",
        "jshookz_provider.wasm.orig",
    }


def test_export_refuses_the_product_build_directory_as_destination() -> None:
    baseline = build.PRODUCTS[build.BASELINE_PRODUCT]
    with pytest.raises(RuntimeError, match="outside the bundle"):
        consumer_bundle.export(build.BASELINE_PRODUCT, baseline.build_dir)
    assert (baseline.build_dir / "jshookz_provider.unwizered.wasm").is_file()


def test_export_overwrites_a_previous_export_in_place(tmp_path: Path) -> None:
    target = tmp_path / "pin"
    consumer_bundle.export(build.BASELINE_PRODUCT, target)
    (target / "jshookz_provider.receipt").write_text("schema tampered\n")
    consumer_bundle.export(build.BASELINE_PRODUCT, target)
    receipt = consumer_bundle.parse_receipt(
        (target / "jshookz_provider.receipt").read_text()
    )
    assert receipt["schema"] == consumer_bundle.RECEIPT_SCHEMA


def test_entropy_product_exports_its_own_bundle(tmp_path: Path) -> None:
    exported = consumer_bundle.export(build.CONSENSUS_ENTROPY_PRODUCT, tmp_path / "e")
    receipt = consumer_bundle.parse_receipt(
        (exported / "jshookz_provider.receipt").read_text()
    )
    assert receipt["product"] == build.CONSENSUS_ENTROPY_PRODUCT
    assert int(receipt["provider_import_count"]) == 30
    assert receipt["exact_v1_declaration_sha256"] == _sha256(
        exported / "xahau-quickjs-v1-consensus-entropy.d.ts"
    )
    assert receipt["selected_surface_sha256"] == _sha256(
        exported / "xahau-quickjs-v1-consensus-entropy.surface.json"
    )
    values = (exported / "jshookz_provider.values.cpp").read_text()
    assert 'providerProduct = "provider-consensus-entropy";' in values
    assert '"entropy_cr_dice"' in values


def test_default_bundle_roots_are_disjoint_per_product() -> None:
    roots = {product.bundle_dir.resolve() for product in build.PRODUCTS.values()}
    assert len(roots) == len(build.PRODUCTS)
    for product in build.PRODUCTS.values():
        bundle = product.bundle_dir.resolve()
        for other in build.PRODUCTS.values():
            assert not bundle.is_relative_to(other.build_dir.resolve())
            assert not other.build_dir.resolve().is_relative_to(bundle)


def test_parse_receipt_rejects_anything_but_key_value_lines() -> None:
    parse = consumer_bundle.parse_receipt
    assert parse("a 1\nb x\n") == {"a": "1", "b": "x"}
    for bad in (
        "a 1",
        "a 1\n\nb 2\n",
        "a\n",
        "a 1 2\n",
        "a 1\na 2\n",
        " a 1\n",
        "A 1\n",
        "a-b 1\n",
        "a 1\t\n",
        "# c\n",
    ):
        with pytest.raises(consumer_bundle.BundleError):
            parse(bad)
    rendered = consumer_bundle.render_receipt({"b": "2", "a": "x"})
    assert rendered == "a x\nb 2\n" and parse(rendered) == {"a": "x", "b": "2"}
    with pytest.raises(consumer_bundle.BundleError):
        consumer_bundle.render_receipt({"Bad-Key": "1"})


def _fake_product(monkeypatch: pytest.MonkeyPatch, build_dir: Path) -> None:
    baseline = build.PRODUCTS[build.BASELINE_PRODUCT]
    monkeypatch.setitem(
        build.PRODUCTS,
        build.BASELINE_PRODUCT,
        dataclasses.replace(
            baseline, build_dir=build_dir, bundle_dir=build_dir / "bundle"
        ),
    )


def _copy_build(tmp_path: Path) -> Path:
    baseline = build.PRODUCTS[build.BASELINE_PRODUCT]
    copy = tmp_path / "build"
    copy.mkdir()
    for name in consumer_bundle.SEALED_FILES:
        (copy / name).write_bytes((baseline.build_dir / name).read_bytes())
    return copy


def test_export_refuses_an_unbuilt_product(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    _fake_product(monkeypatch, tmp_path / "empty")
    with pytest.raises(RuntimeError, match="jshookz build provider"):
        consumer_bundle.export(build.BASELINE_PRODUCT, tmp_path / "out")


def test_export_refuses_a_seal_that_does_not_match_its_manifest(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    copy = _copy_build(tmp_path)
    wasm = copy / "jshookz_provider.wasm"
    wasm.write_bytes(wasm.read_bytes() + b"\0")
    _fake_product(monkeypatch, copy)
    with pytest.raises(RuntimeError, match="does not match its manifest"):
        consumer_bundle.export(build.BASELINE_PRODUCT, tmp_path / "out")
    assert not (tmp_path / "out").exists()


def test_export_refuses_import_drift_between_manifest_and_native_abi(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    copy = _copy_build(tmp_path)
    native_path = copy / "jshookz_provider.native-abi.json"
    native = json.loads(native_path.read_text())
    native["products"]["provider"][0]["wasm_signature"] = ["0x7e"]
    native["selected"] = native["products"]["provider"]
    native_path.write_text(json.dumps(native))
    _fake_product(monkeypatch, copy)
    with pytest.raises(RuntimeError, match="signature disagrees with native ABI"):
        consumer_bundle.export(build.BASELINE_PRODUCT, tmp_path / "out")
    assert not (tmp_path / "out").exists()
