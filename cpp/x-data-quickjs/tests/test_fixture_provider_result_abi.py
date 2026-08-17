"""Combined codec-fixture result-ABI regression tests."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from conftest import REPO, CODEC_WASM, assert_result, run_js


FIXTURE_WASM = REPO / "build" / "codec-fixture" / "jshookz_codec_fixture.wasm"


LARGE_ENCODE_HEX_SCRIPT = """
    var memoData = "AB".repeat(9000);
    var hex = util_hex(encode_object({MemoData: memoData})).toUpperCase();
    JSON.stringify({hex_len: hex.length, hex: hex});
"""

ACCOUNT_ID_BAD_LENGTH_SCRIPT = """
    try {
        var obj = decode_object("8101FF");
        JSON.stringify({threw: false, account: obj.Account});
    } catch (e) {
        JSON.stringify({threw: true, name: e.name, message: e.message});
    }
"""


def _large_encode_result(host_path: Path, wasm_path: Path) -> str:
    r = run_js(host_path, wasm_path, LARGE_ENCODE_HEX_SCRIPT, timeout=60)
    assert_result(r, str(wasm_path))
    return r["result"]


def test_fixture_large_encode_result_matches_standalone(host_path):
    if not CODEC_WASM.exists():
        pytest.fail(f"required standalone codec not found: {CODEC_WASM}")
    if not FIXTURE_WASM.exists():
        pytest.fail(f"required combined codec fixture not found: {FIXTURE_WASM}")

    standalone = _large_encode_result(host_path, CODEC_WASM)
    fixture = _large_encode_result(host_path, FIXTURE_WASM)

    if len(fixture) != len(standalone):
        pytest.fail(
            "large encode result length differs: "
            f"standalone={len(standalone)} fixture={len(fixture)}"
        )
    if fixture != standalone:
        first_diff = next(
            (i for i, (a, b) in enumerate(zip(standalone, fixture)) if a != b),
            None,
        )
        pytest.fail(f"large encode result content differs at offset {first_diff}")

    decoded = json.loads(fixture)
    assert decoded["hex_len"] == len(decoded["hex"])
    assert decoded["hex_len"] > 16 * 1024


def test_accountid_bad_vl_length_is_js_error_in_fixture(host_path):
    if not CODEC_WASM.exists():
        pytest.fail(f"required standalone codec not found: {CODEC_WASM}")
    if not FIXTURE_WASM.exists():
        pytest.fail(f"required combined codec fixture not found: {FIXTURE_WASM}")

    for wasm_path in (CODEC_WASM, FIXTURE_WASM):
        r = run_js(host_path, wasm_path, ACCOUNT_ID_BAD_LENGTH_SCRIPT, timeout=30)
        assert_result(r, str(wasm_path))
        data = json.loads(r["result"])
        assert data["threw"] is True
        assert data["name"] == "TypeError"
        assert "AccountID" in data["message"]
        assert "expected 20 bytes" in data["message"]


# --- RESULT_MAX: the consensus result cap (issue 0009) -----------------------
#
# This is host-visible ABI on a consensus surface, so the value is pinned here
# rather than left as a magic number in C. Both sides of the boundary are
# asserted: a result just under the cap must come back WHOLE, and one over it
# must fail LOUDLY. Asserting only the happy side would let a future edit
# silently reintroduce truncation, which is the exact bug the heap buffer was
# introduced to kill.
RESULT_MAX = 1_048_576

UNDER_CAP_SCRIPT = f'"X".repeat({RESULT_MAX} - 64);'
OVER_CAP_SCRIPT = f'"X".repeat({RESULT_MAX} + 1);'


def test_result_under_cap_returns_whole(host_path):
    if not FIXTURE_WASM.exists():
        pytest.fail(f"required combined codec fixture not found: {FIXTURE_WASM}")
    r = run_js(host_path, FIXTURE_WASM, UNDER_CAP_SCRIPT, timeout=60)
    assert_result(r, str(FIXTURE_WASM))
    # Whole, not clamped: the pre-M7 static slot would have cut this at 16383.
    assert len(r["result"]) == RESULT_MAX - 64


def test_result_over_cap_errors_loudly_not_silently(host_path):
    if not FIXTURE_WASM.exists():
        pytest.fail(f"required combined codec fixture not found: {FIXTURE_WASM}")
    r = run_js(host_path, FIXTURE_WASM, OVER_CAP_SCRIPT, timeout=60)
    # Non-zero status AND a named error — silence or a clamped body is failure.
    assert r["returncode"] != 0, f"over-cap result reported success: {r!r}"
    body = (r.get("result") or r.get("error") or "")
    assert "RangeError" in body, body
    assert str(RESULT_MAX) in body, body
