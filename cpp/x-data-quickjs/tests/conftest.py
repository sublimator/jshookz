"""Native runner and shared fixtures for x-data-quickjs tests."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import tempfile
import textwrap
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parent.parent.parent.parent
CODEC = REPO / "cpp" / "x-data-quickjs"
XDATA = REPO / "cpp" / "x-data"
QJS = REPO / "cpp" / "quickjs"
QJS_CPP = REPO / "cpp" / "quickjs-cpp"
FIXTURE_DIR = Path(
    os.environ.get("JSHOOKZ_CODEC_FIXTURE_DIR", CODEC / "tests" / "fixtures")
)
RUNNER = REPO / "build" / "x-data-quickjs" / "qjs_runner"


def _config_version() -> str:
    return (QJS / "VERSION").read_text(encoding="utf-8").strip()


def _cxx_includes() -> list[str]:
    # Do not put cpp/quickjs on -I: its VERSION file shadows <version>.
    return [
        f"-I{CODEC}",
        f"-I{QJS_CPP / 'include'}",
        f"-I{XDATA / 'includes'}",
        f"-I{XDATA / 'core/includes'}",
        f"-I{XDATA / 'base58/includes'}",
        f"-I{XDATA / 'generated'}",
        f"-I{XDATA / 'stubs'}",
    ]


# Cache only these five engine TUs. Headers + VERSION are the cache key so a
# QuickJS header edit rebuilds them; qjs.c / tests/ do not.
_QJS_C_SOURCES = (
    QJS / "quickjs.c",
    QJS / "cutils.c",
    QJS / "dtoa.c",
    QJS / "libregexp.c",
    QJS / "libunicode.c",
)

# Small, and they include headers (qjs_visitor.h, generated tables) that are
# not the .cpp. Always rebuild so a definitions refresh cannot stay green.
_CXX_SOURCES = (
    QJS_CPP / "qjs.cpp",
    CODEC / "bridge_xdata.cpp",
    XDATA / "src" / "protocol.cpp",
    XDATA / "src" / "embedded_protocol.cpp",
    XDATA / "base58" / "src" / "base58.cpp",
    XDATA / "core" / "src" / "types.cpp",
    XDATA / "stubs" / "digest_stub.cpp",
    CODEC / "tests" / "qjs_runner.cpp",
)


def _qjs_c_stamp() -> float:
    inputs = [QJS / "VERSION", *QJS.glob("*.c"), *QJS.glob("*.h")]
    return max(path.stat().st_mtime for path in inputs)


def _compile(cmd: list[str], src: Path) -> None:
    proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        pytest.fail(f"compile {src.name} failed:\n{proc.stderr}{proc.stdout}")


def build_qjs_runner() -> Path:
    cxx = shutil.which("c++") or shutil.which("clang++")
    cc = shutil.which("cc") or shutil.which("clang")
    if cxx is None or cc is None:
        pytest.fail("host cc and c++ required to build qjs_runner")

    missing = [str(src) for src in (*_QJS_C_SOURCES, *_CXX_SOURCES) if not src.exists()]
    if missing:
        pytest.fail("qjs_runner sources missing: " + ", ".join(missing))

    objdir = RUNNER.parent / "obj"
    objdir.mkdir(parents=True, exist_ok=True)
    version = _config_version()
    qjs_stamp = _qjs_c_stamp()
    objs: list[Path] = []

    for src in _QJS_C_SOURCES:
        obj = objdir / f"{src.name}.o"
        objs.append(obj)
        if obj.exists() and obj.stat().st_mtime >= qjs_stamp:
            continue
        _compile(
            [
                cc,
                "-c",
                "-std=gnu11",
                "-O2",
                "-fwrapv",
                "-funsigned-char",
                f'-DCONFIG_VERSION="{version}"',
                "-D_GNU_SOURCE",
                f"-I{QJS}",
                "-o",
                str(obj),
                str(src),
            ],
            src,
        )

    for src in _CXX_SOURCES:
        obj = objdir / f"{src.name}.o"
        objs.append(obj)
        _compile(
            [
                cxx,
                "-c",
                "-std=c++23",
                "-O2",
                "-DCATL_XDATA_NO_BOOST_JSON",
                "-DCODEC_BACKEND_XDATA",
                *_cxx_includes(),
                "-o",
                str(obj),
                str(src),
            ],
            src,
        )

    link = [cxx, "-o", str(RUNNER), *(str(obj) for obj in objs), "-lm"]
    proc = subprocess.run(link, capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        pytest.fail(f"link qjs_runner failed:\n{proc.stderr}{proc.stdout}")
    return RUNNER


def run_js(runner: Path, js_code: str, timeout: int = 30) -> dict:
    """Eval JS in the native library runner. Returns parsed host lines."""
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
    result_lines = [line.split("] ", 1)[1] for line in lines if line.startswith("[result] ")]
    error_lines = [line.split("] ", 1)[1] for line in lines if line.startswith("[error] ")]
    return {
        "ok": proc.returncode == 0,
        "returncode": proc.returncode,
        "result": result_lines[0] if result_lines else None,
        "error": error_lines[0] if error_lines else None,
        "raw": output,
    }


def assert_result(r, msg=""):
    """Assert that the JS runner returned a result (not an error)."""
    if r["result"] is None:
        err = r.get("error", "no error info")
        raw = r.get("raw", "")[:500]
        raise AssertionError(f"JS execution failed: {err}\n{msg}\nRaw: {raw}")


@pytest.fixture(scope="session")
def qjs_runner_path():
    return build_qjs_runner()


@pytest.fixture(scope="session")
def js_runner(qjs_runner_path):
    def runner(js_code: str, timeout: int = 30) -> dict:
        return run_js(qjs_runner_path, js_code, timeout)

    return runner


@pytest.fixture(scope="session")
def sle_fixtures():
    with open(FIXTURE_DIR / "sle-fixtures.json") as f:
        return json.load(f)
