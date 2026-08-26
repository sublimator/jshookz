"""Shared acceptance-test fixtures."""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

import pytest

from jshookz.paths import REPO_ROOT, WASI_TOOLCHAIN


@pytest.fixture(scope="session")
def resource_probe_wasm(tmp_path_factory: pytest.TempPathFactory) -> Path:
    """Build the non-product heap-ledger variant from candidate sources."""
    build = tmp_path_factory.mktemp("xahau-provider-resource-probe")
    unwizered = build / "jshookz_provider.unwizered.wasm"
    sealed = build / "jshookz_provider.resource-probe.wasm"
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
            "-DJSHOOKZ_RESOURCE_PROBE=ON",
        ),
        ("cmake", "--build", str(build), "--parallel", "4"),
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
