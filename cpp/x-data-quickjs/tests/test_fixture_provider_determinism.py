"""Combined codec-fixture determinism matrix.

Covers, in ONE instance of the combined codec fixture, the three things that have to
agree for the runtime to be usable on a consensus surface:

  1. codec correctness   — encode_object/decode_object, and encoded bytes pinned
  2. sandbox determinism — Math.random sequence and the frozen Date
  3. init equivalence    — a Wizer-snapshotted module behaves identically to a
                           cold-initialized one

They are deliberately in one script rather than three: the failure mode worth
catching is *interaction* — e.g. seeding perturbing the Date wrapper, or Wizer
capturing PRNG state mid-stream. Split tests would each pass while the
combination was broken.

The C++ fixture host has no `--seed` flag, so this file pins the default-seed
(42) stream.

Note on the PRNG: `Math.random` is a native C function reading a native seed
cell, but its arithmetic intentionally mirrors ECMAScript double semantics
(`seed * 1103515245` exceeds 2^53, so the original JS lost precision before the
mask). The pinned values below are that exact sequence. A "cleaner" integer
implementation changes them, which is a consensus-visible change.
"""

from __future__ import annotations

import json
import shutil
import subprocess

import pytest

from conftest import REPO, assert_result, run_js


FIXTURE_WASM = REPO / "build" / "codec-fixture" / "jshookz_codec_fixture.wasm"

# One script, one instance: codec + PRNG + Date.
MATRIX_SCRIPT = """
    var out = {};

    var obj = { TransactionType: "Payment", Sequence: 42, Fee: "12",
                Account: "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh" };
    var hex = util_hex(encode_object(obj)).toUpperCase();
    out.encode_hex = hex;

    var back = decode_object(hex);
    out.decode_seq = back.Sequence;
    out.decode_acct = back.Account;
    out.decode_tt = back.TransactionType;
    out.roundtrip = (util_hex(encode_object(back)).toUpperCase() === hex);

    out.random = [Math.random(), Math.random(), Math.random()].join(",");

    out.date_now = Date.now();
    out.date_new = (new Date()).getTime();
    out.date_parse_is_fn = (typeof Date.parse === "function");

    JSON.stringify(out);
"""

# Canonical field order: TransactionType(1200) Sequence(24) Fee(6840) Account(8114).
EXPECTED_HEX = (
    "120000240000002A68400000000000000C"
    "8114B5F762798A53D543A014CAF8B297CFF8F2F937E8"
)
# Default seed 42, native PRNG mirroring JS double semantics.
EXPECTED_RANDOM = "0.5823075899771916,0.5198186638391664,0.9149397615878563"


@pytest.fixture(scope="session")
def fixture_wasm():
    if not FIXTURE_WASM.exists():
        pytest.fail(f"required combined codec fixture not built: {FIXTURE_WASM}")
    return FIXTURE_WASM


@pytest.fixture(scope="session")
def wizered_wasm(fixture_wasm, tmp_path_factory):
    """A snapshot built from the CURRENT combined codec fixture, every run.

    Never reuse a checked-in `_wizered.wasm`: a stale snapshot silently compares
    yesterday's code against today's, so the equivalence test passes while
    testing nothing. Building into a tmp dir makes staleness impossible.
    """
    wizer = shutil.which("wizer")
    if not wizer:
        pytest.skip("wizer not on PATH (cargo install wizer)")

    out = tmp_path_factory.mktemp("wizer") / "provider_wizered.wasm"
    r = subprocess.run(
        [wizer, str(fixture_wasm), "-o", str(out), "--allow-wasi", "--wasm-simd=true"],
        capture_output=True, text=True, timeout=300,
    )
    assert r.returncode == 0, f"wizer failed: {r.stderr[:500]}"

    # Guard against a trivially-passing equivalence test. Wizer bakes the
    # post-init QuickJS heap into data segments and grows declared initial
    # memory (measured: 7 -> 17 pages, 2 -> 10000 data segments). If the output
    # were not actually pre-initialized, `qjs_init` would just re-run at
    # startup and "equivalence" would be vacuous. Size is a cheap proxy for
    # "the heap really got baked in".
    grew = out.stat().st_size - fixture_wasm.stat().st_size
    assert grew > 100_000, (
        f"wizer output only grew {grew} bytes — snapshot looks un-initialized, "
        "so the cold/wizer equivalence assertion would be vacuous"
    )
    return out


def _matrix(host_path, wasm_path) -> tuple[dict, str]:
    r = run_js(host_path, wasm_path, MATRIX_SCRIPT, timeout=60)
    assert_result(r, str(wasm_path))
    return json.loads(r["result"]), r["result"]


def test_fixture_matrix_cold(host_path, fixture_wasm):
    """Cold init: codec bytes, PRNG stream and Date are all pinned."""
    m, _ = _matrix(host_path, fixture_wasm)

    assert m["encode_hex"] == EXPECTED_HEX
    assert m["decode_seq"] == 42
    assert m["decode_acct"] == "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh"
    assert m["decode_tt"] == "Payment"
    assert m["roundtrip"] is True

    assert m["random"] == EXPECTED_RANDOM

    assert m["date_now"] == 0
    assert m["date_new"] == 0
    assert m["date_parse_is_fn"] is True


def test_fixture_matrix_wizer_equals_cold(host_path, fixture_wasm, wizered_wasm):
    """A Wizered module is observationally identical to a cold one.

    Compares the raw result strings, not parsed dicts, so key order and number
    formatting are covered too — those are part of what a contract can observe.
    """
    _, cold_raw = _matrix(host_path, fixture_wasm)
    _, wiz_raw = _matrix(host_path, wizered_wasm)
    assert wiz_raw == cold_raw


def test_fixture_matrix_wizer_pins(host_path, wizered_wasm):
    """The Wizered module meets the pins on its own.

    Equivalence alone would still pass if BOTH arms regressed together; this
    anchors the snapshot to the absolute expected values.
    """
    m, _ = _matrix(host_path, wizered_wasm)
    assert m["encode_hex"] == EXPECTED_HEX
    assert m["random"] == EXPECTED_RANDOM
    assert m["date_now"] == 0
