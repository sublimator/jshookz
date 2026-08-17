"""Generator tests for native Protocol tables (issue 0064)."""

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


def _write_header(tmp_path: Path, defs: dict, *, no_clean: bool = True) -> Path:
    src = tmp_path / "defs.json"
    src.write_text(json.dumps(defs), encoding="utf-8")
    out = tmp_path / "out.h"
    cmd = [
        sys.executable,
        str(SCRIPT),
        "--input",
        str(src),
        "--output",
        str(out),
        "--namespace",
        "catl::xdata::t",
    ]
    if no_clean:
        cmd.append("--no-clean")
    proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
    assert proc.returncode == 0, proc.stderr
    return out


def test_unwraps_result_wrapper(tmp_path: Path) -> None:
    out = _write_header(
        tmp_path,
        {
            "result": {
                "FIELDS": [
                    [
                        "Account",
                        {
                            "type": "AccountID",
                            "nth": 1,
                            "isSerialized": True,
                            "isSigningField": True,
                            "isVLEncoded": False,
                        },
                    ]
                ]
            }
        },
    )
    text = out.read_text(encoding="utf-8")
    assert "Account" in text
    assert text.count("AccountID") == 1


def test_signed_nth_and_type_codes_are_not_precast(tmp_path: Path) -> None:
    out = _write_header(
        tmp_path,
        {
            "TYPES": {"Unknown": -2, "UInt16": 1},
            "FIELDS": [
                [
                    "Invalid",
                    {
                        "type": "Unknown",
                        "nth": -1,
                        "isSerialized": False,
                        "isSigningField": False,
                        "isVLEncoded": False,
                    },
                ]
            ],
        },
    )
    text = out.read_text(encoding="utf-8")
    assert ", -1, false, false, false}" in text
    assert '{"Unknown", -2}' in text
    assert "65535" not in text
    assert "65534" not in text


def test_absent_permissions_is_size_zero_array(tmp_path: Path) -> None:
    out = _write_header(tmp_path, {"FIELDS": []})
    text = out.read_text(encoding="utf-8")
    assert (
        "inline constexpr std::array<catl::xdata::ProtocolTableNameCode, 0> "
        "PERMISSIONS{};"
    ) in text


def test_permission_codes_stay_uint32_width(tmp_path: Path) -> None:
    tables = gen.tables_from_defs(
        {
            "FIELDS": [],
            "PERMISSIONS": {"AccountDomainSet": 65540},
        }
    )
    assert tables["PERMISSIONS"] == [{"name": "AccountDomainSet", "code": 65540}]
    header = gen.generate_header(tables, "ns")
    assert '{"AccountDomainSet", 65540}' in header
    assert "4}" not in header.split("PERMISSIONS")[1]


def test_star_slash_name_is_refused(tmp_path: Path) -> None:
    src = tmp_path / "bad.json"
    src.write_text(
        json.dumps(
            {
                "FIELDS": [
                    [
                        "evil*/x",
                        {
                            "type": "UInt32",
                            "nth": 1,
                            "isSerialized": True,
                            "isSigningField": True,
                            "isVLEncoded": False,
                        },
                    ]
                ]
            }
        ),
        encoding="utf-8",
    )
    proc = subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "--input",
            str(src),
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
    assert "*/" in proc.stderr


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
    assert "EMBEDDED_DEFINITIONS" not in out1.read_text(encoding="utf-8")


def test_real_xahau_tables_match_cleaned_json() -> None:
    src = CODEC / "x-data/definitions/xahau_definitions.json"
    defs = gen.clean_definitions(gen.unwrap_definitions(json.loads(src.read_text())))
    tables = gen.tables_from_defs(defs)
    assert len(tables["FIELDS"]) == len(defs["FIELDS"])
    assert tables["FIELDS"][0]["name"] == defs["FIELDS"][0][0]
    assert tables["PERMISSIONS"] == []
    assert tables["TYPES"][0]["name"] == next(iter(defs["TYPES"]))
    unknown = next(row for row in tables["TYPES"] if row["name"] == "Unknown")
    assert unknown["code"] == -2
    invalid = next(row for row in tables["FIELDS"] if row["name"] == "Invalid")
    assert invalid["nth"] == -1
