"""Property tests for generated JSON -> binary -> JSON codec roundtrips."""

from __future__ import annotations

import json
import os

import pytest
from hypothesis import given, settings, strategies as st

from conftest import REPO, assert_result


DEFINITIONS = REPO / "cpp" / "x-data" / "definitions" / "xahau_definitions.json"
PROPERTY_MAX_EXAMPLES = int(os.environ.get("XAHAUD_PROPERTY_MAX_EXAMPLES", "12"))
ACCOUNT_IDS = (
    "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
    "rPT1Sjq2YGrBMTttX4GZHjKu9dyfzbpAYe",
    "rfkE1aSy9G8Upk4JssnwBxhEv5p4mn2KTy",
    "r9cZA1mLK5R5Am25ArfXFmqgNwjZgnfk59",
)
SIMPLE_TYPES = {
    "UInt8",
    "UInt16",
    "UInt32",
    "UInt64",
    "Hash128",
    "Hash160",
    "Hash256",
    "Amount",
    "AccountID",
    "Blob",
}
ENUM_FIELDS = {
    "LedgerEntryType",
    "PermissionValue",
    "TransactionResult",
    "TransactionType",
}


def _field_defs():
    with open(DEFINITIONS) as f:
        definitions = json.load(f)
    fields = []
    for name, meta in definitions["FIELDS"]:
        if (
            meta["isSerialized"]
            and meta["type"] in SIMPLE_TYPES
            and name not in ENUM_FIELDS
            and name[:1].isupper()
        ):
            fields.append((name, meta["type"]))
    return fields


def _hex_string(num_bytes):
    return st.binary(min_size=num_bytes, max_size=num_bytes).map(
        lambda data: data.hex().upper()
    )


def _value_strategy(field_type):
    if field_type == "UInt8":
        return st.integers(min_value=0, max_value=2**8 - 1)
    if field_type == "UInt16":
        return st.integers(min_value=0, max_value=2**16 - 1)
    if field_type == "UInt32":
        return st.integers(min_value=0, max_value=2**32 - 1)
    if field_type == "UInt64":
        return st.integers(min_value=0, max_value=2**64 - 1).map(
            lambda value: f"{value:016X}"
        )
    if field_type == "Hash128":
        return _hex_string(16)
    if field_type == "Hash160":
        return _hex_string(20)
    if field_type == "Hash256":
        return _hex_string(32)
    if field_type == "Amount":
        return st.integers(min_value=0, max_value=10**12).map(str)
    if field_type == "AccountID":
        return st.sampled_from(ACCOUNT_IDS)
    if field_type == "Blob":
        return st.binary(min_size=0, max_size=32).map(lambda data: data.hex().upper())
    raise AssertionError(f"unsupported generated field type: {field_type}")


def _stobject_strategy():
    field_defs = _field_defs()
    return st.lists(
        st.sampled_from(field_defs),
        min_size=1,
        max_size=8,
        unique_by=lambda field: field[0],
    ).flatmap(
        lambda fields: st.fixed_dictionaries(
            {name: _value_strategy(field_type) for name, field_type in fields}
        )
    )


@pytest.mark.property
@settings(max_examples=PROPERTY_MAX_EXAMPLES, deadline=None, database=None)
@given(obj=_stobject_strategy())
def test_generated_simple_field_subset_roundtrips(js_runner, obj):
    payload = json.dumps(obj, sort_keys=True)
    r = js_runner(f"""
        var input = {payload};
        var encoded = encode_object(input);
        var decoded_view = decode_object(encoded);
        var decoded = JSON.parse(JSON.stringify(decoded_view));
        var hex = util_hex(encoded).toUpperCase();
        var rehex = util_hex(encode_object(decoded_view)).toUpperCase();
        JSON.stringify({{
            decoded: decoded,
            hex: hex,
            rehex: rehex
        }});
    """)
    assert_result(r, payload)
    data = json.loads(r["result"])
    assert data["decoded"] == obj
    assert data["rehex"] == data["hex"]
