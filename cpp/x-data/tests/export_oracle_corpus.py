#!/usr/bin/env python3
"""Export 0069 locate/certify corpus from xahau-codec. Never confuses empty VL
with the 20-byte all-zero AccountID."""

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


def classify_hex(blob: str, codec_type: str = "stobject") -> tuple[str, str]:
    args = ["debug-json"]
    if codec_type != "stobject":
        args += ["--codec-type", codec_type]
    args.append(blob)
    code, out, err = run_codec(args)
    if code == 0:
        return "accept", out
    if "Too many NOPS" in err:
        return "reject", err
    if "self-check failed" in err:
        # decode succeeded; envelope omitted NOP or resorted fields
        return "accept", err
    if err.startswith("Error decoding:") or "Error decoding:" in err:
        return "reject", err
    return "reject", err or out


def case(cid: str, blob: str, *, expect: str | None = None, codec_type: str = "stobject", notes: str = "", json_src=None):
    got, detail = classify_hex(blob, codec_type)
    if expect is not None and got != expect:
        raise RuntimeError(f"{cid}: expected {expect} got {got}: {detail}")
    item = {
        "id": cid,
        "expect": got,
        "codec_type": codec_type,
        "blob": blob.upper(),
        "notes": notes,
    }
    if json_src is not None:
        item["json"] = json_src
    if got == "reject":
        item["oracle_error"] = detail.split("\n")[0]
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

    # sorted is 24.... then 81.... ; swap halves of known layout
    if not seq_acct.upper().startswith("24"):
        raise RuntimeError("unexpected Sequence header")
    unsorted = seq_acct[10:] + seq_acct[:10]
    cases.append(case("stobject-seq-account-out-of-order", unsorted, expect="accept",
                      notes="Out-of-order nonduplicate fields: xahaud set() accepts."))

    cases.append(case("stobject-duplicate-account", acct20 + acct20, expect="reject",
                      notes="Duplicate Account after sort."))

    cases.append(case("stobject-trailing-ff", acct20 + "FF", expect="reject",
                      notes="Trailing bytes after a complete object."))

    cases.append(case("stobject-nop-0", acct20, expect="accept", notes="Zero NOP headers."))
    cases.append(case("stobject-nop-1", "99" + acct20, expect="accept",
                      notes="One NOP 0x99; omitted from material index."))
    cases.append(case("stobject-nop-63", ("99" * 63) + acct20, expect="accept",
                      notes="63 NOPs accepted (throw is ++counter == 64)."))
    cases.append(case("stobject-nop-64", ("99" * 64) + acct20, expect="reject",
                      notes="64th NOP: Too many NOPS."))

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

    cases.append(case("pathset-empty", "00", expect="reject", codec_type="pathset",
                      notes="Empty PathSet (lone 0x00)."))

    fee = encode(json.dumps({"Fee": "10"}))
    cases.append(case("stobject-fee", fee, expect="accept", json_src={"Fee": "10"}))

    payload = {
        "oracle_repo": "xahaud-worktrees/xahaud-hookz-test-vectors",
        "oracle_commit": "4e08bd2c7",
        "notes": (
            "Account empty VL (8100) is defaulted STAccount, 0 payload bytes. "
            "Account 20 zero bytes is an explicit all-zero AccountID "
            "(base58 rrr...rhoLvTp). Not empty VL."
        ),
        "cases": cases,
    }
    OUT.write_text(json.dumps(payload, indent=2) + "\n")
    accepts = sum(1 for c in cases if c["expect"] == "accept")
    rejects = sum(1 for c in cases if c["expect"] == "reject")
    print(f"wrote {OUT} ({len(cases)} cases, {accepts} accept, {rejects} reject)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
