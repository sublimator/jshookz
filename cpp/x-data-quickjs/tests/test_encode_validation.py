"""Encode-path validation regressions (M6).

After M6 the separate prevalidation pass was deleted and validation moved into
the single encode walk. The M6 fan-out review confirmed the rejections below
previously lived only in prevalidation (or, for the length guard, were
re-tightened in M6) — pin them here against the plain-literal encode path,
which the STObject-view mutation tests do not exercise.
"""

import json

from conftest import assert_result

ACCT = "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh"
MPT48 = "0" * 48


def _encode_outcomes(js_runner, cases: dict) -> dict:
    r = js_runner(
        "var c=" + json.dumps(cases) + ";"
        "var out={};"
        "for (var k in c) {"
        "  try { out[k] = {ok:true, hex: util_hex(encode_object(c[k])).toUpperCase()}; }"
        "  catch (e) { out[k] = {ok:false, name: e.name}; }"
        "}"
        "JSON.stringify(out);"
    )
    assert_result(r)
    return json.loads(r["result"])


def test_composite_shape_violations_rejected(js_runner):
    """Plain-literal composite malformations whose rejection was previously only
    exercised via the STObject-view mutation path (M6 fan-out, composite lens)."""
    out = _encode_outcomes(js_runner, {
        "scalar_in_array_element": {"Account": ACCT, "Memos": [{"Memo": 1}]},
        "iou_missing_issuer": {"Account": ACCT, "Amount": {"value": "1", "currency": "USD"}},
        "mpt_short_issuance_id": {"Account": ACCT, "Amount": {"value": "5", "mpt_issuance_id": "AB"}},
    })
    for k, v in out.items():
        assert v["ok"] is False, (k, v)
        assert v["name"] == "TypeError", (k, v)


def test_uint64_hex_length_canonicalized(js_runner):
    """UInt64 / MPT hex value must be <=16 chars (canonical). M6 re-tightened
    the length guard the deleted prevalidation held, so an over-length string
    rejects even when the value fits in 64 bits (M6 fan-out, leaf lens)."""
    out = _encode_outcomes(js_runner, {
        "u64_canonical_1": {"OwnerNode": "1"},
        "u64_full_16": {"OwnerNode": "0000000000000001"},
        "u64_padded_18": {"OwnerNode": "000000000000000001"},   # >16 chars, value fits
        "u64_overflow_17": {"OwnerNode": "1FFFFFFFFFFFFFFFF"},   # >16 chars, real overflow
        "mpt_value_padded_18": {"Account": ACCT,
                                "Amount": {"value": "0x000000000000000001",
                                           "mpt_issuance_id": MPT48}},
    })
    assert out["u64_canonical_1"]["ok"] is True, out["u64_canonical_1"]
    assert out["u64_full_16"]["ok"] is True, out["u64_full_16"]
    # canonical "1" and full-width "000…0001" encode to identical 8-byte values
    assert out["u64_canonical_1"]["hex"] == out["u64_full_16"]["hex"]
    assert out["u64_padded_18"]["ok"] is False, out["u64_padded_18"]
    assert out["u64_overflow_17"]["ok"] is False, out["u64_overflow_17"]
    assert out["mpt_value_padded_18"]["ok"] is False, out["mpt_value_padded_18"]
