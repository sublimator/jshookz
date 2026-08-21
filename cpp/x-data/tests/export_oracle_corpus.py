#!/usr/bin/env python3
"""Export 0069 locate/certify corpus from xahau-codec.

Never confuses empty VL with the 20-byte all-zero AccountID.
Retains debug-json envelopes (fields, canonical_blob, json, commit).
"""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
CODEC = Path.home() / "projects/xahaud-worktrees/xahaud-hookz-test-vectors/build/xahau-codec"
OUT = Path(__file__).with_name("oracle_corpus.json")
GENESIS = "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh"
ZERO_B58 = "rrrrrrrrrrrrrrrrrrrrrhoLvTp"
ORACLE_COMMIT = "cb829d7657607643f0bdc29c65f9a41fbd86a688"
XCHAIN_NATIVE = (
    "011914B5F762798A53D543A014CAF8B297CFF8F2F937E8"
    "0000000000000000000000000000000000000000"
    "14B5F762798A53D543A014CAF8B297CFF8F2F937E8"
    "0000000000000000000000000000000000000000"
)
PATHSET_TRUNCATED = "011201" + ("00" * 20)
USD_CURRENCY = "0000000000000000000000005553440000000000"
ZERO20 = "00" * 20
GENESIS_HEX = "B5F762798A53D543A014CAF8B297CFF8F2F937E8"
# USD currency + all-zero native account. Oracle rejects native mismatch.
XCHAIN_ISSUE_MISMATCH = (
    "011914" + GENESIS_HEX + USD_CURRENCY + ZERO20
    + "14" + GENESIS_HEX + ZERO20
)
ISSUE_USD_ZERO_ACCOUNT = "0118" + USD_CURRENCY + ZERO20


def run_codec(args: list[str], input_text: str | None = None) -> tuple[int, str, str]:
    proc = subprocess.run(
        [str(CODEC), *args],
        input=input_text,
        capture_output=True,
        text=True,
        check=False,
    )
    return proc.returncode, proc.stdout.strip(), proc.stderr.strip()


def encode(json_text: str, codec_type: str = "stobject") -> str:
    args = ["encode"]
    if codec_type != "stobject":
        args += ["--codec-type", codec_type]
    args.append(json_text)
    code, out, err = run_codec(args)
    if code != 0:
        raise RuntimeError(f"encode failed {json_text}: {err or out}")
    return "".join(out.split())


def classify_hex(blob: str, codec_type: str = "stobject") -> tuple[str, dict | str]:
    args = ["debug-json"]
    if codec_type != "stobject":
        args += ["--codec-type", codec_type]
    args.append(blob)
    code, out, err = run_codec(args)
    if code == 0:
        try:
            envelope = json.loads(out)
        except json.JSONDecodeError as exc:
            raise RuntimeError(f"debug-json not JSON for {blob}: {out}") from exc
        commit = envelope.get("commit")
        if commit and not str(commit).startswith(ORACLE_COMMIT[:9]):
            raise RuntimeError(
                f"oracle commit {commit} does not match pin {ORACLE_COMMIT}"
            )
        return "accept", envelope
    if "Too many NOPS" in err:
        return "reject", err
    if err.startswith("Error decoding:") or "Error decoding:" in err:
        return "reject", err
    return "reject", err or out


def case(
    cid: str,
    blob: str,
    *,
    expect: str | None = None,
    codec_type: str = "stobject",
    notes: str = "",
    json_src=None,
    trailing_ok: bool = False,
):
    got, detail = classify_hex(blob, codec_type)
    if expect is not None and got != expect:
        raise RuntimeError(f"{cid}: expected {expect} got {got}: {detail}")
    item = {
        "id": cid,
        "expect": got,
        "codec_type": codec_type,
        "blob": blob.upper(),
        "notes": notes,
        "oracle_commit": ORACLE_COMMIT,
    }
    if trailing_ok:
        item["trailing_ok"] = True
    if json_src is not None:
        item["json"] = json_src
    if got == "accept" and isinstance(detail, dict):
        item["canonical_blob"] = str(detail.get("canonical_blob", "")).upper()
        item["fields"] = detail.get("fields", [])
        if "json" in detail and "json" not in item:
            item["json"] = detail["json"]
        item["oracle_branch"] = detail.get("branch")
        item["oracle_codec_type"] = detail.get("codec_type")
    elif got == "reject":
        err0 = str(detail).split("\n")[0]
        item["oracle_error"] = err0
        if "is required but missing" in err0:
            # SOTemplate required fields are not CertifyWire representation.
            item["wire_ok"] = True
    return item


def main() -> int:
    if not CODEC.is_file():
        print(f"missing xahau-codec at {CODEC}", file=sys.stderr)
        return 1

    cases = []

    acct20 = encode(json.dumps({"Account": GENESIS}))
    cases.append(case("stobject-account-20", acct20, expect="accept",
                      json_src={"Account": GENESIS},
                      notes="20-byte Account VL payload (genesis)."))

    zero20 = encode(json.dumps({"Account": ZERO_B58}))
    cases.append(case("stobject-account-zero-20", zero20, expect="accept",
                      json_src={"Account": ZERO_B58},
                      notes="20 zero bytes: explicit all-zero AccountID (base58 rrr...rhoLvTp). Not empty VL."))

    cases.append(case("stobject-account-empty-vl", "8100", expect="accept",
                      notes="STAccount VL length 0 (defaulted). Distinct from 20-byte zero AccountID."))

    cases.append(case("stobject-account-vl-1", "8101FF", expect="reject",
                      notes="STAccount VL length 1 is invalid."))

    seq_acct = encode(json.dumps({"Account": GENESIS, "Sequence": 1}))
    cases.append(case("stobject-seq-account-sorted", seq_acct, expect="accept",
                      json_src={"Account": GENESIS, "Sequence": 1}))

    if not seq_acct.upper().startswith("24"):
        raise RuntimeError("unexpected Sequence header")
    unsorted = seq_acct[10:] + seq_acct[:10]
    cases.append(case("stobject-seq-account-out-of-order", unsorted, expect="accept",
                      notes="Out-of-order nonduplicate fields: xahaud set() accepts."))

    cases.append(case("stobject-duplicate-account", acct20 + acct20, expect="reject",
                      notes="Duplicate Account after sort."))

    cases.append(case("stobject-trailing-ff", acct20 + "FF", expect="reject",
                      notes="Unknown field after a complete unterminated object."))

    cases.append(case("stobject-e1", "E1", expect="accept",
                      notes="Top-level object end marker only."))
    cases.append(case("stobject-e1-then-ff", "E1FF", expect="accept",
                      trailing_ok=True,
                      notes="Object end marker stops the scope; leftover is not a constructor reject."))

    cases.append(case("stobject-nop-0", acct20, expect="accept", notes="Zero NOP headers."))
    cases.append(case("stobject-nop-1", "99" + acct20, expect="accept",
                      notes="One NOP 0x99; omitted from material index."))
    cases.append(case("stobject-nop-63", ("99" * 63) + acct20, expect="accept",
                      notes="63 NOPs accepted (throw is ++counter == 64)."))
    cases.append(case("stobject-nop-64", ("99" * 64) + acct20, expect="reject",
                      notes="64th NOP: Too many NOPS."))

    cases.append(case("stobject-array-nop-1", "F999F1", expect="accept",
                      notes="Array-local NOP; canonical_blob drops 0x99."))
    cases.append(case("stobject-array-nop-63", "F9" + ("99" * 63) + "F1", expect="accept",
                      notes="63 array-local NOPs accepted."))
    cases.append(case("stobject-array-nop-64", "F9" + ("99" * 64) + "F1", expect="reject",
                      notes="64th array-local NOP: Too many NOPS."))

    amt_obj = encode(json.dumps({"Account": GENESIS, "Amount": "1000000"}))
    cases.append(case("stobject-native-amount", amt_obj, expect="accept",
                      json_src={"Account": GENESIS, "Amount": "1000000"}))

    iou = {"value": "1", "currency": "USD", "issuer": GENESIS}
    iou_obj = encode(json.dumps({"Account": GENESIS, "Amount": iou}))
    cases.append(case("stobject-iou-amount", iou_obj, expect="accept",
                      json_src={"Account": GENESIS, "Amount": iou}))

    iou0 = {"value": "0", "currency": "USD", "issuer": GENESIS}
    iou0_obj = encode(json.dumps({"Account": GENESIS, "Amount": iou0}))
    cases.append(case("stobject-iou-zero", iou0_obj, expect="accept",
                      json_src={"Account": GENESIS, "Amount": iou0},
                      notes="Canonical IOU zero; not native."))

    bad_iou = encode(json.dumps({"Account": GENESIS, "Amount": {
        "value": "1", "currency": "USD", "issuer": ZERO_B58}}))
    cases.append(case("stobject-iou-zero-issuer", bad_iou, expect="reject",
                      notes="IOU issuer all-zero is native-account, rejected. Not empty VL."))

    mpt = {
        "value": "1",
        "mpt_issuance_id": "000000000000000000000000000000000000000000000000",
    }
    mpt_blob = encode(json.dumps(mpt), "amount")
    cases.append(case("amount-mpt", mpt_blob, expect="accept", codec_type="amount",
                      json_src=mpt))

    native = encode('"1000000"', "amount")
    cases.append(case("amount-native", native, expect="accept", codec_type="amount",
                      json_src="1000000"))

    native0 = encode('"0"', "amount")
    cases.append(case("amount-native-zero", native0, expect="accept", codec_type="amount",
                      json_src="0", notes="Positive native zero."))

    native_neg = encode('"-1000000"', "amount")
    cases.append(case("amount-native-negative", native_neg, expect="accept",
                      codec_type="amount", json_src="-1000000"))

    cases.append(case("amount-native-neg-zero", "0000000000000000", expect="reject",
                      codec_type="amount",
                      notes="Native zero without positive bit is not canonical."))

    cases.append(case("amount-native-trunc", "40000000000F42", expect="reject",
                      codec_type="amount", notes="7-byte native payload."))

    st_neg = encode(json.dumps({"Account": GENESIS, "Amount": "-5"}))
    cases.append(case("stobject-native-negative", st_neg, expect="accept",
                      json_src={"Account": GENESIS, "Amount": "-5"}))

    iou_neg = {"value": "-1", "currency": "USD", "issuer": GENESIS}
    iou_neg_obj = encode(json.dumps({"Account": GENESIS, "Amount": iou_neg}))
    cases.append(case("stobject-iou-negative", iou_neg_obj, expect="accept",
                      json_src={"Account": GENESIS, "Amount": iou_neg}))

    # IOU with XRP currency is native-currency and rejected.
    bad_cur = (
        "61D4838D7EA4C68000" + ZERO20 + GENESIS_HEX
        + "8114" + GENESIS_HEX
    )
    cases.append(case("stobject-iou-native-currency", bad_cur, expect="reject",
                      notes="IOU currency all-zero (native) is rejected."))

    # Mantissa 1 is below cMinValue.
    tiny_mant = (
        "61C000000000000001" + USD_CURRENCY + GENESIS_HEX
        + "8114" + GENESIS_HEX
    )
    cases.append(case("stobject-iou-tiny-mantissa", tiny_mant, expect="reject",
                      notes="IOU mantissa 1 is not canonical."))

    min_mant = 10**15
    max_mant = 10**16 - 1

    def iou_payload(exponent: int, mantissa: int, positive: bool = True) -> str:
        ten = 512 + (256 if positive else 0) + (exponent + 97)
        word = (ten << 54) | mantissa
        return f"{word:016X}{USD_CURRENCY}{GENESIS_HEX}"

    cases.append(case("amount-iou-exp-m96", iou_payload(-96, min_mant),
                      expect="accept", codec_type="amount",
                      notes="IOU exponent -96 inclusive lower bound."))
    cases.append(case("amount-iou-exp-80", iou_payload(80, min_mant),
                      expect="accept", codec_type="amount",
                      notes="IOU exponent 80 inclusive upper bound."))
    cases.append(case("amount-iou-exp-m97", iou_payload(-97, min_mant),
                      expect="reject", codec_type="amount",
                      notes="IOU exponent -97 is out of range."))
    cases.append(case("amount-iou-exp-81", iou_payload(81, min_mant),
                      expect="reject", codec_type="amount",
                      notes="IOU exponent 81 is out of range."))
    cases.append(case("amount-iou-mant-max", iou_payload(-15, max_mant),
                      expect="accept", codec_type="amount",
                      notes="IOU mantissa 9999999999999999 inclusive upper bound."))
    cases.append(case("amount-iou-mant-over", iou_payload(-15, max_mant + 1),
                      expect="reject", codec_type="amount",
                      notes="IOU mantissa 10000000000000000 is out of range."))

    mpt0 = {
        "value": "0",
        "mpt_issuance_id": "000000000000000000000000000000000000000000000000",
    }
    mpt0_blob = encode(json.dumps(mpt0), "amount")
    cases.append(case("amount-mpt-zero", mpt0_blob, expect="accept",
                      codec_type="amount", json_src=mpt0))

    mpt_neg = {
        "value": "-1",
        "mpt_issuance_id": "000000000000000000000000000000000000000000000000",
    }
    mpt_neg_blob = encode(json.dumps(mpt_neg), "amount")
    cases.append(case("amount-mpt-negative", mpt_neg_blob, expect="accept",
                      codec_type="amount", json_src=mpt_neg))

    cases.append(case("amount-mpt-trunc", mpt_blob[:64], expect="reject",
                      codec_type="amount", notes="32-byte truncated MPT."))

    blob_obj = encode(json.dumps({"Account": GENESIS, "Blob": "DEAD"}))
    cases.append(case("stobject-vl-blob", blob_obj, expect="accept",
                      json_src={"Account": GENESIS, "Blob": "DEAD"}))

    memos = encode(json.dumps({
        "Account": GENESIS,
        "Memos": [{"Memo": {"MemoData": "DEAD"}}],
    }))
    cases.append(case("stobject-nested-memos", memos, expect="accept",
                      json_src={"Account": GENESIS, "Memos": [{"Memo": {"MemoData": "DEAD"}}]}))

    paths = encode(json.dumps({
        "Account": GENESIS,
        "Paths": [[{"account": GENESIS}]],
    }))
    cases.append(case("stobject-pathset", paths, expect="accept",
                      json_src={"Account": GENESIS, "Paths": [[{"account": GENESIS}]]}))

    cases.append(case("stobject-pathset-truncated", PATHSET_TRUNCATED, expect="reject",
                      notes="Paths hop without END_BYTE. Framing error in both scan modes."))

    cases.append(case("pathset-empty", "00", expect="reject", codec_type="pathset",
                      notes="Empty PathSet (lone 0x00)."))

    cases.append(case("stobject-xchain-bridge", XCHAIN_NATIVE, expect="accept",
                      notes="XChainBridge: two VL-AccountID + two Issue (native)."))
    cases.append(case(
        "stobject-xchain-issue-native-mismatch", XCHAIN_ISSUE_MISMATCH,
        expect="reject",
        notes=(
            "XChainBridge first Issue is USD plus the all-zero native account. "
            "Oracle rejects currency/account native mismatch. Locate may still "
            "frame; CertifyWire must reject."
        )))
    cases.append(case(
        "stobject-issue-usd-zero-account", ISSUE_USD_ZERO_ACCOUNT,
        expect="reject",
        notes=(
            "Standalone Issue: USD currency with all-zero native account. "
            "Native currency plus a non-native account cannot be framed as one "
            "Issue field: native currency is 20 bytes, leftover is not Issue."
        )))

    fee = encode(json.dumps({"Fee": "10"}))
    cases.append(case("stobject-fee", fee, expect="accept", json_src={"Fee": "10"}))

    # Finite header x scope enumerator vs debug-json. Not story blobs.
    # IDs must match oracle_run::header_enum_ids().
    hdr_ids = []
    for b in range(256):
        blob = f"{b:02X}"
        cid = f"hdr-b-{blob}"
        hdr_ids.append(cid)
        cases.append(case(
            cid, blob,
            notes="header enum: single-byte top-level object"))
    for type_byte in (0, 1, 5, 15, 16, 26, 255):
        for name_nibble in (0x1, 0x5, 0xE):
            blob = f"{name_nibble:02X}{type_byte:02X}"
            cid = f"hdr-t-{type_byte:02X}-n-{name_nibble:X}"
            hdr_ids.append(cid)
            cases.append(case(
                cid, blob,
                notes="header enum: long-form type"))
    for type_nibble in (1, 8, 14):
        for name_byte in (0, 1, 5, 15, 16, 255):
            blob = f"{type_nibble:X}0{name_byte:02X}"
            cid = f"hdr-n-t{type_nibble:X}-f{name_byte:02X}"
            hdr_ids.append(cid)
            cases.append(case(
                cid, blob,
                notes="header enum: long-form name"))
    for inner in ("E1", "E032E1", "E032E1F1", "F1", "99", "0105"):
        blob = "F9" + inner
        cid = f"hdr-arr-{inner}"
        hdr_ids.append(cid)
        cases.append(case(
            cid, blob,
            notes="header enum: array wrap"))
        if inner != "F1" and not inner.endswith("F1"):
            cid_f1 = f"hdr-arr-{inner}-F1"
            hdr_ids.append(cid_f1)
            cases.append(case(
                cid_f1, blob + "F1",
                notes="header enum: array wrap plus end marker"))
    if len(set(hdr_ids)) != 305:
        raise RuntimeError(f"header enum id count {len(set(hdr_ids))} != 305")

    payload = {
        "oracle_repo": "xahaud-worktrees/xahaud-hookz-test-vectors",
        "oracle_commit": ORACLE_COMMIT,
        "notes": (
            "Account empty VL (8100) is defaulted STAccount, 0 payload bytes. "
            "Account 20 zero bytes is an explicit all-zero AccountID "
            "(base58 rrr...rhoLvTp). Not empty VL. "
            "Accepts retain debug-json fields and canonical_blob."
        ),
        "cases": cases,
    }
    OUT.write_text(json.dumps(payload, indent=2) + "\n")
    accepts = sum(1 for c in cases if c["expect"] == "accept")
    rejects = sum(1 for c in cases if c["expect"] == "reject")
    missing = [
        c["id"] for c in cases
        if c["expect"] == "accept" and c.get("codec_type") == "stobject"
        and ("fields" not in c or "canonical_blob" not in c)
    ]
    if missing:
        raise RuntimeError(f"accept missing envelope: {missing}")
    print(f"wrote {OUT} ({len(cases)} cases, {accepts} accept, {rejects} reject)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
