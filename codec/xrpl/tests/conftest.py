"""Shared fixtures for the XRPL/Xahau codec WASM tests."""

import json
import subprocess
import tempfile
import os
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parent.parent.parent.parent
CODEC = REPO / "codec" / "xrpl"
HOST = REPO / "build" / "codec-host" / "jshookz_codec_host"
CODEC_WASM = REPO / "build" / "codec" / "jshookz_xrpl_codec.wasm"
FIXTURE_DIR = Path(
    os.environ.get("JSHOOKZ_CODEC_FIXTURE_DIR", CODEC / "tests" / "fixtures")
)


def _get_wasm():
    if not CODEC_WASM.exists():
        pytest.fail(f"required codec WASM not found: {CODEC_WASM}")
    return CODEC_WASM


@pytest.fixture(scope="session")
def wasm_path():
    return _get_wasm()


@pytest.fixture(scope="session")
def host_path():
    if not HOST.exists():
        pytest.fail(f"required codec test host not found: {HOST}")
    return HOST


def run_js(host_path, wasm_path, js_code: str, timeout: int = 30) -> dict:
    """Run JS code through the WASM host. Returns parsed output."""
    # Strip common leading indentation (from triple-quoted Python strings)
    import textwrap
    js_code = textwrap.dedent(js_code).strip() + "\n"

    test_file = CODEC / "tests" / "_test_script.js"
    test_file.write_text(js_code)

    try:
        r = subprocess.run(
            [str(host_path), "--wasm", str(wasm_path), "--script", str(test_file)],
            capture_output=True, text=True, timeout=timeout,
        )
    finally:
        test_file.unlink(missing_ok=True)

    lines = (r.stdout + r.stderr).splitlines()
    contract_lines = [l.split("] ", 1)[1] for l in lines if l.startswith("[contract] ")]
    result_lines = [l.split("] ", 1)[1] for l in lines if l.startswith("[result] ")]
    error_lines = [l.split("] ", 1)[1] for l in lines if l.startswith("[error] ")]

    out = {
        "ok": r.returncode == 0,
        "returncode": r.returncode,
        "contract": contract_lines,
        "result": result_lines[0] if result_lines else None,
        "error": error_lines[0] if error_lines else None,
        "raw": r.stdout + r.stderr,
    }
    return out


def assert_result(r, msg=""):
    """Assert that the JS runner returned a result (not an error)."""
    if r["result"] is None:
        err = r.get("error", "no error info")
        raw = r.get("raw", "")[:500]
        raise AssertionError(f"JS execution failed: {err}\n{msg}\nRaw: {raw}")


@pytest.fixture(scope="session")
def js_runner(host_path, wasm_path):
    """Returns a callable: js_runner(code) → output dict."""
    def runner(js_code: str, timeout: int = 30) -> dict:
        return run_js(host_path, wasm_path, js_code, timeout)
    return runner


# --- Fixture data ---

@pytest.fixture(scope="session")
def codec_fixtures():
    with open(FIXTURE_DIR / "codec-fixtures.json") as f:
        return json.load(f)["stobject"]


@pytest.fixture(scope="session")
def sle_fixtures():
    with open(FIXTURE_DIR / "sle-fixtures.json") as f:
        return json.load(f)


@pytest.fixture(scope="session")
def tx_fixtures():
    with open(FIXTURE_DIR / "tx-type-fixtures.json") as f:
        return json.load(f)
