"""Shared acceptance-test fixtures."""

from __future__ import annotations

import json
import shutil
import subprocess
from pathlib import Path
from typing import Any

import pytest

from jshookz.paths import REPO_ROOT, WASI_TOOLCHAIN


RUNTIME_SNAPSHOT = Path(__file__).with_name("runtime-observations.snapshot.json")


class RuntimeSnapshot:
    """Compare deterministic observations, or explicitly refresh their JSON."""

    def __init__(self, path: Path, *, update: bool) -> None:
        self.path = path
        self.update = update

    def assert_match(self, name: str, actual: dict[str, Any]) -> None:
        document = json.loads(self.path.read_text())
        observations = document["observations"]
        if self.update:
            observations[name] = actual
            self.path.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n")
        assert actual == observations[name]


def pytest_addoption(parser: pytest.Parser) -> None:
    parser.getgroup("jshookz snapshots").addoption(
        "--update-runtime-snapshots",
        action="store_true",
        help="replace committed deterministic runtime observations with actual values",
    )


@pytest.fixture
def runtime_snapshot(request: pytest.FixtureRequest) -> RuntimeSnapshot:
    return RuntimeSnapshot(
        RUNTIME_SNAPSHOT,
        update=bool(request.config.getoption("--update-runtime-snapshots")),
    )


def _build_sealed_provider(
    build: Path,
    sealed_name: str,
    *cmake_options: str,
) -> Path:
    unwizered = build / "jshookz_provider.unwizered.wasm"
    sealed = build / sealed_name
    wizer = shutil.which("wizer")
    assert wizer is not None, "wizer is required for the resource acceptance gate"

    commands = (
        (
            "cmake",
            "-B",
            str(build),
            "-S",
            str(REPO_ROOT / "cpp/provider"),
            f"-DCMAKE_TOOLCHAIN_FILE={WASI_TOOLCHAIN}",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DXAHAU_HOOK_PROVIDER=ON",
            *cmake_options,
        ),
        ("cmake", "--build", str(build)),
        (
            wizer,
            "--keep-init-func",
            "true",
            "--rename-func",
            "_initialize=wizer.initialize",
            "-o",
            str(sealed),
            str(unwizered),
        ),
    )
    for command in commands:
        completed = subprocess.run(
            command,
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        assert completed.returncode == 0, completed.stdout + completed.stderr
    assert sealed.is_file()
    return sealed


@pytest.fixture(scope="session")
def resource_probe_wasm(tmp_path_factory: pytest.TempPathFactory) -> Path:
    """Build the non-product heap-ledger variant from candidate sources."""
    return _build_sealed_provider(
        tmp_path_factory.mktemp("xahau-provider-resource-probe"),
        "jshookz_provider.resource-probe.wasm",
        "-DJSHOOKZ_RESOURCE_PROBE=ON",
    )


@pytest.fixture(scope="session")
def xfl_gap_loop_mutant_wasm(tmp_path_factory: pytest.TempPathFactory) -> Path:
    """Build and seal the source-level O(exponent-gap) arithmetic mutant."""
    return _build_sealed_provider(
        tmp_path_factory.mktemp("xahau-provider-xfl-gap-loop-mutant"),
        "jshookz_provider.xfl-gap-loop-mutant.wasm",
        "-DJSHOOKZ_TEST_XFL_GAP_LOOP_MUTANT=ON",
    )


@pytest.fixture(scope="session")
def xfl_semantic_mutant_wasm(
    request: pytest.FixtureRequest,
    tmp_path_factory: pytest.TempPathFactory,
) -> Path:
    """Build and seal one named non-product XFL arithmetic mutant."""
    mutant = str(request.param)
    return _build_sealed_provider(
        tmp_path_factory.mktemp(f"xahau-provider-xfl-{mutant}-mutant"),
        f"jshookz_provider.xfl-{mutant}-mutant.wasm",
        f"-DJSHOOKZ_TEST_XFL_SEMANTIC_MUTANT={mutant}",
    )
