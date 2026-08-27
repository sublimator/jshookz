"""Generator tests for native Protocol tables (issue 0064)."""

from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
import subprocess
import sys
from pathlib import Path

import pytest

XDATA = Path(__file__).resolve().parent.parent
SCRIPT = XDATA / "scripts" / "generate_definitions.py"
PROVIDER_POLICY = XDATA / "definitions/provider_static_policy.json"

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
    src = XDATA / "definitions/xahau_definitions.json"
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


@pytest.mark.parametrize("network", ["xahau", "xrpl"])
def test_reserved_hash_surplus_is_validated_and_rendered(network: str) -> None:
    src = XDATA / f"definitions/{network}_definitions.json"
    defs = gen.unwrap_definitions(json.loads(src.read_text()))
    tables = gen.tables_from_defs(defs)
    header = gen.generate_header(tables, f"catl::xdata::{network}")
    for name, code in gen.RESERVED_SURPLUS_TYPES.items():
        assert (
            f'{{"{name}", {code}}}  /* Protocol-reserved surplus: '
            "zero serialized fields. */"
        ) in header


@pytest.mark.parametrize("network", ["xahau", "xrpl"])
@pytest.mark.parametrize("name", ["Hash384", "Hash512"])
def test_reserved_hash_wrong_code_turns_generation_red(network: str, name: str) -> None:
    src = XDATA / f"definitions/{network}_definitions.json"
    defs = copy.deepcopy(gen.unwrap_definitions(json.loads(src.read_text())))
    defs["TYPES"][name] += 1
    with pytest.raises(ValueError, match="must be exactly"):
        gen.tables_from_defs(defs)


@pytest.mark.parametrize("network", ["xahau", "xrpl"])
@pytest.mark.parametrize("name", ["Hash384", "Hash512"])
def test_reserved_hash_field_use_turns_generation_red(network: str, name: str) -> None:
    src = XDATA / f"definitions/{network}_definitions.json"
    defs = copy.deepcopy(gen.unwrap_definitions(json.loads(src.read_text())))
    defs["FIELDS"].append(
        [
            f"Reserved{name}Probe",
            {
                "type": name,
                "nth": 1,
                "isSerialized": True,
                "isSigningField": True,
                "isVLEncoded": False,
            },
        ]
    )
    with pytest.raises(ValueError, match="zero field uses"):
        gen.tables_from_defs(defs)


def test_real_xahau_provider_closure_is_exact_and_total() -> None:
    src = XDATA / "definitions/xahau_definitions.json"
    defs = gen.clean_definitions(gen.unwrap_definitions(json.loads(src.read_text())))
    type_materializers, overrides, policy_sha = gen.load_materializer_policy(
        PROVIDER_POLICY
    )
    tables = gen.provider_tables_from_defs(defs, type_materializers, overrides)

    assert len(tables["names"]) == 337
    assert len(tables["fields"]) == 327
    assert len(tables["material"]) == 325
    assert len(tables["types"]) == 19
    assert tables["fallback"] == []
    assert tables["duplicate_word_count"] == 6
    assert len(tables["fast"]) * len(tables["fast"][0]) * 2 == 8192
    assert policy_sha != "none"

    admitted = tables["fields"]
    for ordinal, row in enumerate(tables["material"]):
        assert row["admission_ordinal"] < len(admitted)
        field = admitted[row["admission_ordinal"]]
        assert field["material_ordinal"] == ordinal
        assert field["code"] == row["field_code"]
        assert field["materializer"] == row["materializer"]
        assert row["materializer"] != "invalid"

    source_fields = defs["FIELDS"]
    by_name = {source_fields[row["name_ordinal"]][0]: row for row in admitted}
    assert by_name["TransactionType"]["materializer"] == "transaction_type"
    assert by_name["TransactionResult"]["materializer"] == "transaction_result"
    assert by_name["Number"]["materializer"] == "number"
    assert tables["max_type_code"] == 26
    assert tables["max_nth"] == 100


def _mutated_policy(tmp_path: Path, mutate: object) -> Path:
    policy = json.loads(PROVIDER_POLICY.read_text(encoding="utf-8"))
    mutate(policy)
    path = tmp_path / "policy.json"
    path.write_text(json.dumps(policy), encoding="utf-8")
    return path


def test_provider_static_cli_requires_materializer_policy(tmp_path: Path) -> None:
    proc = subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "--input",
            str(XDATA / "definitions/xahau_definitions.json"),
            "--output",
            str(tmp_path / "out.h"),
            "--namespace",
            "catl::xdata::test",
            "--provider-static",
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    assert proc.returncode != 0
    assert "requires --materializer-policy" in proc.stderr


@pytest.mark.parametrize("type_name", sorted(gen.PROVIDER_WIRE_WIDTHS))
@pytest.mark.parametrize("mode", ["delete", "wrong-valid-kind"])
def test_every_wire_type_policy_mutation_turns_red(
    tmp_path: Path, type_name: str, mode: str
) -> None:
    def mutate(policy: dict) -> None:
        mappings = policy["wire_type_materializers"]
        if mode == "delete":
            del mappings[type_name]
        else:
            current = mappings[type_name]
            mappings[type_name] = next(
                kind for kind in mappings.values() if kind != current
            )

    with pytest.raises(ValueError, match="materializer"):
        gen.load_materializer_policy(_mutated_policy(tmp_path, mutate))


@pytest.mark.parametrize("field_name", sorted(gen.EXPECTED_PROVIDER_FIELD_OVERRIDES))
@pytest.mark.parametrize("mode", ["delete", "wrong-valid-kind"])
def test_every_field_override_policy_mutation_turns_red(
    tmp_path: Path, field_name: str, mode: str
) -> None:
    def mutate(policy: dict) -> None:
        overrides = policy["descriptor_overrides"]
        if mode == "delete":
            del overrides[field_name]
        else:
            overrides[field_name] = "number"

    with pytest.raises(ValueError, match="override"):
        gen.load_materializer_policy(_mutated_policy(tmp_path, mutate))
