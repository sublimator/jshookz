"""Hypothesis driver for the CMake-built qjs_runner. Does not compile C++."""

from __future__ import annotations

import os
import subprocess
import tempfile
import textwrap
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parent.parent.parent.parent


def _runner_path() -> Path:
    env = os.environ.get("JSHOOKZ_QJS_RUNNER")
    if env:
        return Path(env)
    return REPO / "build" / "cpp" / "x-data-quickjs" / "qjs_runner"


def run_js(runner: Path, js_code: str, timeout: int = 30) -> dict:
    js_code = textwrap.dedent(js_code).strip() + "\n"
    with tempfile.NamedTemporaryFile(
        "w", suffix=".js", delete=False, encoding="utf-8"
    ) as handle:
        handle.write(js_code)
        script_path = Path(handle.name)
    try:
        proc = subprocess.run(
            [str(runner), "--script", str(script_path)],
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
    finally:
        script_path.unlink(missing_ok=True)

    output = proc.stdout + proc.stderr
    lines = output.splitlines()
    result_lines = [
        line.split("] ", 1)[1] for line in lines if line.startswith("[result] ")
    ]
    error_lines = [
        line.split("] ", 1)[1] for line in lines if line.startswith("[error] ")
    ]
    return {
        "ok": proc.returncode == 0,
        "returncode": proc.returncode,
        "result": result_lines[0] if result_lines else None,
        "error": error_lines[0] if error_lines else None,
        "raw": output,
    }


def assert_result(r, msg=""):
    if r["result"] is None:
        err = r.get("error", "no error info")
        raw = r.get("raw", "")[:500]
        raise AssertionError(f"JS execution failed: {err}\n{msg}\nRaw: {raw}")


@pytest.fixture(scope="session")
def qjs_runner_path():
    runner = _runner_path()
    if not runner.is_file():
        pytest.skip(f"qjs_runner not built at {runner}; cmake --build build/cpp")
    return runner


@pytest.fixture(scope="session")
def js_runner(qjs_runner_path):
    def runner(js_code: str, timeout: int = 30) -> dict:
        return run_js(qjs_runner_path, js_code, timeout)

    return runner
