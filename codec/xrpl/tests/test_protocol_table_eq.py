"""Host-side Protocol table equality (issue 0064)."""

from __future__ import annotations

import importlib.util
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

CODEC = Path(__file__).resolve().parent.parent
XDATA = CODEC / "x-data"
SRC = CODEC / "tests" / "protocol_table_eq.cpp"
SCRIPT = CODEC / "scripts" / "generate_definitions.py"

spec = importlib.util.spec_from_file_location("generate_definitions", SCRIPT)
assert spec is not None and spec.loader is not None
gen = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = gen
spec.loader.exec_module(gen)


def _cleaned_json(src: Path, dest: Path) -> None:
    raw = json.loads(src.read_text(encoding="utf-8"))
    cleaned = gen.clean_definitions(gen.unwrap_definitions(raw))
    dest.write_text(json.dumps(cleaned), encoding="utf-8")


def _boost_include() -> Path | None:
    env = os.environ.get("BOOST_INCLUDE_DIR")
    if env:
        return Path(env)
    brew = Path("/opt/homebrew/include")
    if (brew / "boost/json.hpp").exists():
        return brew
    usr = Path("/usr/include")
    if (usr / "boost/json.hpp").exists():
        return usr
    return None


def _no_boost_includes() -> list[str]:
    # Do not put engine/quickjs on -I: its VERSION file shadows <version>.
    return [
        f"-I{CODEC}",
        f"-I{XDATA / 'includes'}",
        f"-I{XDATA / 'core/includes'}",
        f"-I{XDATA / 'base58/includes'}",
        f"-I{XDATA / 'generated'}",
        f"-I{CODEC / 'stubs'}",
    ]


def test_wasm_headers_do_not_include_boost_json(tmp_path: Path) -> None:
    compiler = shutil.which("c++") or shutil.which("clang++")
    if compiler is None:
        pytest.skip("host c++ required for no-boost header probe")

    probe = CODEC / "tests" / "no_boost_json_probe.cpp"
    out = tmp_path / "no_boost_json_probe.o"
    cmd = [
        compiler,
        "-std=c++23",
        "-c",
        "-o",
        str(out),
        *_no_boost_includes(),
        str(probe),
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
    assert proc.returncode == 0, proc.stderr + proc.stdout


def test_wasm_tus_compile_without_boost_json(tmp_path: Path) -> None:
    """Compile the wasm C++ TUs with the flag and no Boost -I."""
    compiler = shutil.which("c++") or shutil.which("clang++")
    if compiler is None:
        pytest.skip("host c++ required for no-boost TU probe")

    tus = [
        XDATA / "src" / "protocol.cpp",
        XDATA / "src" / "embedded_protocol.cpp",
        XDATA / "base58" / "src" / "base58.cpp",
        XDATA / "core" / "src" / "types.cpp",
        CODEC / "stubs" / "digest_stub.cpp",
        CODEC / "bridge_xdata.cpp",
    ]
    for src in tus:
        out = tmp_path / (src.name + ".o")
        cmd = [
            compiler,
            "-std=c++23",
            "-c",
            "-o",
            str(out),
            "-DCATL_XDATA_NO_BOOST_JSON",
            "-DCODEC_BACKEND_XDATA",
            *_no_boost_includes(),
            str(src),
        ]
        proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
        assert proc.returncode == 0, f"{src.name}\n{proc.stderr}{proc.stdout}"


def test_embedded_tables_match_json_load(tmp_path: Path) -> None:
    compiler = shutil.which("c++") or shutil.which("clang++")
    boost = _boost_include()
    if compiler is None or boost is None:
        pytest.skip("host c++ and Boost headers required for Protocol equality")

    binary = tmp_path / "protocol_table_eq"
    cmd = [
        compiler,
        "-std=c++23",
        "-O0",
        f"-I{XDATA / 'includes'}",
        f"-I{XDATA / 'core/includes'}",
        f"-I{XDATA / 'base58/includes'}",
        f"-I{XDATA / 'generated'}",
        f"-I{CODEC / 'stubs'}",
        f"-I{boost}",
        str(XDATA / "src/protocol.cpp"),
        str(XDATA / "src/embedded_protocol.cpp"),
        str(XDATA / "src/protocol_json.cpp"),
        str(XDATA / "boost_json_src.cpp"),
        str(XDATA / "base58/src/base58.cpp"),
        str(CODEC / "stubs/digest_stub.cpp"),
        str(SRC),
        "-o",
        str(binary),
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
    assert proc.returncode == 0, proc.stderr + proc.stdout

    xahau = tmp_path / "xahau.cleaned.json"
    xrpl = tmp_path / "xrpl.cleaned.json"
    _cleaned_json(XDATA / "definitions/xahau_definitions.json", xahau)
    _cleaned_json(XDATA / "definitions/xrpl_definitions.json", xrpl)

    run = subprocess.run(
        [str(binary), str(xahau), str(xrpl)],
        capture_output=True,
        text=True,
        check=False,
    )
    assert run.returncode == 0, run.stderr + run.stdout
