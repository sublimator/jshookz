"""Generator tests for adjacent raw-string embedding (issue 0054)."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import subprocess
import sys
from pathlib import Path

CODEC = Path(__file__).resolve().parent.parent
SCRIPT = CODEC / "scripts" / "generate_definitions.py"

spec = importlib.util.spec_from_file_location("generate_definitions", SCRIPT)
assert spec is not None and spec.loader is not None
gen = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = gen
spec.loader.exec_module(gen)


def test_small_payload_is_one_literal() -> None:
    payload = json.dumps({"ok": True}, separators=(",", ":"))
    chunks = gen.split_raw_string_chunks(payload)
    assert chunks == [payload]
    header = gen.generate_header(payload, "ns", "tiny.json", "abc")
    body = header.split("EMBEDDED_DEFINITIONS[] =", 1)[1]
    assert body.count('R"json(') == 1


def test_oversized_payload_splits_under_limit() -> None:
    payload = "x" * (gen.LITERAL_CHUNK_LIMIT * 2 + 100)
    chunks = gen.split_raw_string_chunks(payload)
    assert len(chunks) >= 3
    assert all(len(c) <= gen.LITERAL_CHUNK_LIMIT for c in chunks)
    assert "".join(chunks) == payload
    header = gen.generate_header(payload, "ns", "big.json", "def")
    body = header.split("EMBEDDED_DEFINITIONS[] =", 1)[1]
    assert body.count('R"json(') == len(chunks)


def test_delimiter_refusal(tmp_path: Path) -> None:
    bad = tmp_path / "bad.json"
    bad.write_text('{"x":")json"}', encoding="utf-8")
    proc = subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "--input",
            str(bad),
            "--output",
            str(tmp_path / "out.h"),
            "--namespace",
            "ns",
            "--no-clean",
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    assert proc.returncode != 0
    assert ")json" in proc.stderr


def test_header_is_deterministic_and_stamps_sha(tmp_path: Path) -> None:
    src = tmp_path / "defs.json"
    src.write_text('{"FIELDS":[]}', encoding="utf-8")
    expected_sha = hashlib.sha256(src.read_bytes()).hexdigest()
    out1 = tmp_path / "a.h"
    out2 = tmp_path / "b.h"
    for out in (out1, out2):
        proc = subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                "--input",
                str(src),
                "--output",
                str(out),
                "--namespace",
                "catl::xdata::t",
                "--no-clean",
            ],
            check=False,
        )
        assert proc.returncode == 0
    assert out1.read_bytes() == out2.read_bytes()
    assert expected_sha in out1.read_text(encoding="utf-8")
