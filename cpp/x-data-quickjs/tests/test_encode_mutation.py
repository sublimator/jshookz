"""Regression tests for mutable decoded STObject/STArray views."""

import json
import subprocess
import tempfile
import textwrap
from pathlib import Path

from conftest import CODEC, assert_result


MEMO_HEX = (
    "1200002200000000240000006461400000000007A12068400000000000000C"
    "8114B5F762798A53D543A014CAF8B297CFF8F2F937E88314F667B0CA50"
    "CC7709A220B0561B85E53A48461FA8F9EA7C0A746578742F706C61696E"
    "7D0548656C6C6FE1F1"
)

STOBJECT_DESCRIPTOR_MESSAGE = "STObject views do not support descriptor definitions"
STARRAY_DESCRIPTOR_MESSAGE = (
    "STArray views are fixed-shape and do not support descriptor definitions"
)
STARRAY_SET_MESSAGE = (
    "STArray views are fixed-shape; only existing elements can be "
    "replaced, and only with objects"
)
STARRAY_ELEMENT_MESSAGE = "STArray element fields must remain object-valued"
STARRAY_DELETE_MESSAGE = "STArray views are fixed-shape; elements cannot be deleted"


def test_top_nested_and_array_element_mutations_encode_and_stringify(js_runner):
    r = js_runner(f"""
        var hex = "{MEMO_HEX}";

        var top = decode_object(hex);
        top.Fee = "20";
        var top_decoded = decode_object(encode_object(top));

        var nested = decode_object(hex);
        nested.Memos[0].Memo.MemoData = "DEADBEEF";
        var nested_decoded = decode_object(encode_object(nested));

        var element = decode_object(hex);
        element.Memos[0] = {{
            Memo: {{
                MemoType: "746578742F706C61696E",
                MemoData: "ABCD"
            }}
        }};
        var element_decoded = decode_object(encode_object(element));

        JSON.stringify({{
            top_json_fee: JSON.parse(JSON.stringify(top)).Fee,
            top_encoded_fee: top_decoded.Fee,
            nested_json_data: JSON.parse(JSON.stringify(nested)).Memos["0"].Memo.MemoData,
            nested_encoded_data: nested_decoded.Memos[0].Memo.MemoData,
            element_json_data: JSON.parse(JSON.stringify(element)).Memos["0"].Memo.MemoData,
            element_encoded_data: element_decoded.Memos[0].Memo.MemoData
        }});
    """)
    assert_result(r)
    data = json.loads(r["result"])
    assert data == {
        "top_json_fee": "20",
        "top_encoded_fee": "20",
        "nested_json_data": "DEADBEEF",
        "nested_encoded_data": "DEADBEEF",
        "element_json_data": "ABCD",
        "element_encoded_data": "ABCD",
    }


def test_write_undefined_delete_readd_and_added_fields(js_runner):
    r = js_runner(f"""
        var hex = "{MEMO_HEX}";

        var undef = decode_object(hex);
        undef.Fee = undefined;
        var undef_json = JSON.parse(JSON.stringify(undef));
        var undef_redecoded = decode_object(encode_object(undef));

        var deleted = decode_object(hex);
        var delete_result = delete deleted.Fee;
        var delete_keys = Object.keys(deleted);
        var delete_json = JSON.parse(JSON.stringify(deleted));
        var delete_redecoded = decode_object(encode_object(deleted));
        var delete_read_type = typeof deleted.Fee;
        deleted.Fee = "77";
        var readd_redecoded = decode_object(encode_object(deleted));

        var added = decode_object(hex);
        added.SourceTag = 123;
        added.NonProtocol = 99;
        var added_keys = Object.keys(added);
        var added_json = JSON.parse(JSON.stringify(added));
        var added_hex = util_hex(encode_object(added)).toUpperCase();
        var added_redecoded = decode_object(added_hex);

        var absent_delete = decode_object(hex);
        var absent_delete_result = delete absent_delete.NotAProtocolField;
        var absent_delete_hex = util_hex(encode_object(absent_delete)).toUpperCase();

        JSON.stringify({{
            undef_read_type: typeof undef.Fee,
            undef_json_has_fee: Object.prototype.hasOwnProperty.call(undef_json, "Fee"),
            undef_encoded_has_fee: "Fee" in undef_redecoded,
            delete_result: delete_result,
            delete_read_type: delete_read_type,
            delete_keys: delete_keys,
            delete_json_has_fee: Object.prototype.hasOwnProperty.call(delete_json, "Fee"),
            delete_encoded_has_fee: "Fee" in delete_redecoded,
            readd_fee: readd_redecoded.Fee,
            added_keys: added_keys,
            added_json_source: added_json.SourceTag,
            added_json_non_protocol: added_json.NonProtocol,
            added_encoded_source: added_redecoded.SourceTag,
            added_encoded_has_non_protocol: "NonProtocol" in added_redecoded,
            added_source_before_sequence:
                added_hex.indexOf("230000007B") < added_hex.indexOf("2400000064"),
            absent_delete_result: absent_delete_result,
            absent_delete_hex: absent_delete_hex
        }});
    """)
    assert_result(r)
    data = json.loads(r["result"])
    assert data["undef_read_type"] == "undefined"
    assert data["undef_json_has_fee"] is False
    assert data["undef_encoded_has_fee"] is False
    assert data["delete_result"] is True
    assert data["delete_read_type"] == "undefined"
    assert "Fee" not in data["delete_keys"]
    assert data["delete_json_has_fee"] is False
    assert data["delete_encoded_has_fee"] is False
    assert data["readd_fee"] == "77"
    assert data["added_keys"][-2:] == ["SourceTag", "NonProtocol"]
    assert data["added_json_source"] == 123
    assert data["added_json_non_protocol"] == 99
    assert data["added_encoded_source"] == 123
    assert data["added_encoded_has_non_protocol"] is False
    assert data["added_source_before_sequence"] is True
    assert data["absent_delete_result"] is True
    assert data["absent_delete_hex"] == MEMO_HEX


def test_nested_view_encode_does_not_fast_path_root_bytes(js_runner):
    r = js_runner(f"""
        var tx = decode_object("{MEMO_HEX}");
        JSON.stringify({{
            inner_hex: util_hex(encode_object(tx.Memos[0].Memo)).toUpperCase(),
            root_hex: util_hex(encode_object(tx)).toUpperCase()
        }});
    """)
    assert_result(r)
    data = json.loads(r["result"])
    assert data["inner_hex"] == "7C0A746578742F706C61696E7D0548656C6C6F"
    assert data["root_hex"] == MEMO_HEX


def test_proto_assignment_is_ordinary_non_protocol_cache_key(js_runner):
    r = js_runner(f"""
        var tx = decode_object("{MEMO_HEX}");
        tx.__proto__ = {{SourceTag: 123}};
        JSON.stringify({{
            source_tag_type: typeof tx.SourceTag,
            has_source_tag: Object.prototype.hasOwnProperty.call(tx, "SourceTag"),
            is_stobject: tx instanceof STObject,
            has_proto_key: Object.prototype.hasOwnProperty.call(tx, "__proto__"),
            keys: Object.keys(tx),
            proto_value: tx.__proto__,
            rehex: util_hex(encode_object(tx)).toUpperCase()
        }});
    """)
    assert_result(r)
    data = json.loads(r["result"])
    assert data["source_tag_type"] == "undefined"
    assert data["has_source_tag"] is False
    assert data["is_stobject"] is True
    assert data["has_proto_key"] is True
    assert data["keys"][-1] == "__proto__"
    assert data["proto_value"] == {"SourceTag": 123}
    assert data["rehex"] == MEMO_HEX


def test_invalid_mutated_value_reports_clean_validation_error(js_runner):
    r = js_runner(f"""
        var tx = decode_object("{MEMO_HEX}");
        tx.Fee = "bogus";
        var caught = null;
        try {{
            encode_object(tx);
        }} catch (e) {{
            caught = {{name: e.name, message: e.message}};
        }}
        JSON.stringify(caught);
    """)
    assert_result(r)
    data = json.loads(r["result"])
    assert data["name"] == "TypeError"
    assert "encode_object failed" in data["message"]
    assert "Fee" in data["message"]


def test_bad_hash_and_blob_hex_report_clean_encode_errors(js_runner):
    r = js_runner("""
        function capture(value) {
            try {
                return {
                    threw: false,
                    hex: util_hex(encode_object(value)).toUpperCase()
                };
            } catch (e) {
                return {threw: true, name: e.name, message: e.message};
            }
        }

        JSON.stringify({
            hash: capture({PreviousTxnID: "G".repeat(64)}),
            blob: capture({MemoData: "GG"})
        });
    """)
    assert_result(r)
    data = json.loads(r["result"])
    assert data["hash"]["threw"] is True
    assert data["hash"]["name"] == "TypeError"
    assert "encode_object failed" in data["hash"]["message"]
    assert "PreviousTxnID" in data["hash"]["message"]
    assert "hex" in data["hash"]["message"]
    assert data["blob"]["threw"] is True
    assert data["blob"]["name"] == "TypeError"
    assert "encode_object failed" in data["blob"]["message"]
    assert "MemoData" in data["blob"]["message"]
    assert "hex" in data["blob"]["message"]


def test_fixed_shape_arrays_and_descriptors_rejected(js_runner):
    r = js_runner(f"""
        var hex = "{MEMO_HEX}";
        var out = {{}};
        function capture(fn) {{
            try {{ fn(); return {{threw: false}}; }}
            catch (e) {{ return {{threw: true, name: e.name, message: e.message}}; }}
        }}

        var tx = decode_object(hex);
        out.dp_value = capture(function() {{
            Object.defineProperty(tx, "Fee",
                {{value: "99", enumerable: true, configurable: true}});
        }});
        out.dp_accessor = capture(function() {{
            Object.defineProperty(tx, "Fee", {{get: function() {{ return "99"; }}}});
        }});
        out.dp_reflect = capture(function() {{
            Reflect.defineProperty(tx, "Fee", {{value: "99"}});
        }});
        out.dp_array = capture(function() {{
            Object.defineProperty(tx.Memos, "0", {{value: null}});
        }});
        var w = tx.Memos[0];
        out.wrap_dp = capture(function() {{
            Object.defineProperty(w, "Memo", {{value: null}});
        }});

        out.length_write = capture(function() {{ tx.Memos.length = 2; }});
        out.append = capture(function() {{
            tx.Memos[tx.Memos.length] = {{Memo: {{MemoData: "ABCD"}}}};
        }});
        out.named_prop = capture(function() {{ tx.Memos.Extra = 1; }});
        out.delete_array = capture(function() {{ delete tx.Memos[0]; }});
        out.empty_element = capture(function() {{ tx.Memos[0] = {{}}; }});
        out.wrong_element_name = capture(function() {{
            tx.Memos[0] = {{SignerEntry: {{}}}};
        }});
        out.extra_element_key = capture(function() {{
            tx.Memos[0] = {{Memo: {{}}, Extra: {{}}}};
        }});
        out.delete_element_field = capture(function() {{ delete w.Memo; }});
        out.undefined_element_field = capture(function() {{ w.Memo = undefined; }});

        out.enc_unchanged =
            util_hex(encode_object(tx)).toUpperCase() === hex.toUpperCase();
        JSON.stringify(out);
    """)
    assert_result(r)
    data = json.loads(r["result"])
    for key in ("dp_value", "dp_accessor", "dp_reflect", "wrap_dp"):
        assert data[key] == {
            "threw": True,
            "name": "TypeError",
            "message": STOBJECT_DESCRIPTOR_MESSAGE,
        }, key
    assert data["dp_array"] == {
        "threw": True,
        "name": "TypeError",
        "message": STARRAY_DESCRIPTOR_MESSAGE,
    }
    for key in ("length_write", "append", "named_prop"):
        assert data[key] == {
            "threw": True,
            "name": "TypeError",
            "message": STARRAY_SET_MESSAGE,
        }, key
    for key in ("empty_element", "wrong_element_name", "extra_element_key"):
        assert data[key] == {
            "threw": True,
            "name": "TypeError",
            "message": STARRAY_SET_MESSAGE,
        }, key
    for key in ("delete_element_field", "undefined_element_field"):
        assert data[key] == {
            "threw": True,
            "name": "TypeError",
            "message": STARRAY_ELEMENT_MESSAGE,
        }, key
    assert data["delete_array"] == {
        "threw": True,
        "name": "TypeError",
        "message": STARRAY_DELETE_MESSAGE,
    }
    assert data["enc_unchanged"] is True


def test_plain_starray_element_must_emit_one_protocol_field(js_runner):
    r = js_runner("""
        var caught = null;
        try {
            encode_object({
                TransactionType: "Payment",
                Flags: 0,
                Sequence: 100,
                Amount: "500000",
                Fee: "12",
                Account: "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
                Destination: "rPT1Sjq2YGrBMTttX4GZHjKu9dyfzbpAYe",
                Memos: [{}]
            });
        } catch (e) {
            caught = {name: e.name, message: e.message};
        }
        JSON.stringify(caught);
    """)
    assert_result(r)
    data = json.loads(r["result"])
    assert data["name"] == "TypeError"
    assert "Memos[0]" in data["message"]
    assert "exactly one protocol field" in data["message"]


def test_scalar_stobject_typed_field_rejected_cleanly(js_runner):
    r = js_runner(f"""
        var tx = decode_object("{MEMO_HEX}");
        tx.Memo = 1;
        var json = JSON.parse(JSON.stringify(tx));
        var caught = null;
        try {{
            encode_object(tx);
        }} catch (e) {{
            caught = {{name: e.name, message: e.message}};
        }}
        JSON.stringify({{jsonMemo: json.Memo, caught: caught}});
    """)
    assert_result(r)
    data = json.loads(r["result"])
    assert data["jsonMemo"] == 1
    assert data["caught"]["name"] == "TypeError"
    assert "Memo" in data["caught"]["message"]
    assert "STObject" in data["caught"]["message"]
    assert "must be an object" in data["caught"]["message"]


def _run_js_with_gas(host_path: Path, wasm_path: Path, js_code: str) -> tuple[dict, int]:
    script = textwrap.dedent(js_code).strip() + "\n"
    with tempfile.NamedTemporaryFile(
        "w", suffix=".js", dir=CODEC / "tests", delete=False
    ) as f:
        f.write(script)
        script_path = Path(f.name)

    try:
        r = subprocess.run(
            [
                str(host_path),
                "--wasm",
                str(wasm_path),
                "--script",
                str(script_path),
                "--gas",
                "1000000000",
            ],
            capture_output=True,
            text=True,
            timeout=30,
        )
    finally:
        script_path.unlink(missing_ok=True)

    output = r.stdout + r.stderr
    result_lines = [
        line.split("] ", 1)[1]
        for line in output.splitlines()
        if line.startswith("[result] ")
    ]
    gas_lines = [
        line.split("] ", 1)[1]
        for line in output.splitlines()
        if line.startswith("[gas] ")
    ]
    if r.returncode != 0 or not result_lines or not gas_lines:
        raise AssertionError(f"gas run failed: rc={r.returncode}\n{output[:2000]}")
    return json.loads(result_lines[0]), int(gas_lines[0])


def test_fast_path_fuel_delta_and_mutations_disable_it(host_path, wasm_path):
    clean_result, clean_fuel = _run_js_with_gas(
        host_path,
        wasm_path,
        f"""
        var hex = "{MEMO_HEX}";
        var tx = decode_object(hex);
        JSON.stringify({{rehex: util_hex(encode_object(tx)).toUpperCase()}});
        """,
    )
    assert clean_result["rehex"] == MEMO_HEX

    noop_result, noop_fuel = _run_js_with_gas(
        host_path,
        wasm_path,
        f"""
        var hex = "{MEMO_HEX}";
        var tx = decode_object(hex);
        delete tx.NotAProtocolField;
        JSON.stringify({{rehex: util_hex(encode_object(tx)).toUpperCase()}});
        """,
    )
    assert noop_result["rehex"] == MEMO_HEX

    scripts = {
        "top": """
            tx.Fee = "20";
            var decoded = decode_object(encode_object(tx));
            result = {fee: decoded.Fee};
        """,
        "nested": """
            tx.Memos[0].Memo.MemoData = "DEADBEEF";
            var decoded = decode_object(encode_object(tx));
            result = {memo_data: decoded.Memos[0].Memo.MemoData};
        """,
        "element": """
            tx.Memos[0] = {Memo: {MemoData: "ABCD"}};
            var decoded = decode_object(encode_object(tx));
            result = {memo_data: decoded.Memos[0].Memo.MemoData};
        """,
        "delete": """
            delete tx.Fee;
            var decoded = decode_object(encode_object(tx));
            result = {has_fee: "Fee" in decoded};
        """,
    }
    expected = {
        "top": {"fee": "20"},
        "nested": {"memo_data": "DEADBEEF"},
        "element": {"memo_data": "ABCD"},
        "delete": {"has_fee": False},
    }

    mutated_fuels = {}
    for name, body in scripts.items():
        result, fuel = _run_js_with_gas(
            host_path,
            wasm_path,
            f"""
            var hex = "{MEMO_HEX}";
            var tx = decode_object(hex);
            var result;
            {body}
            JSON.stringify(result);
            """,
        )
        assert result == expected[name], name
        mutated_fuels[name] = fuel

    assert min(mutated_fuels.values()) > clean_fuel * 1.05, (
        clean_fuel,
        mutated_fuels,
    )
    assert noop_fuel < min(mutated_fuels.values()), (noop_fuel, mutated_fuels)


def test_array_element_non_object_writes_rejected(js_runner):
    """arr[i] accepts only object replacements: undefined/scalar writes would
    silently drop or resurrect the element at encode time (post-unfreeze
    review finding), so they must reject loud with bytes unchanged."""
    r = js_runner(f"""
        var hex = "{MEMO_HEX}";
        var tx = decode_object(hex);
        function capture(fn) {{
            try {{ fn(); return {{threw: false}}; }}
            catch (e) {{ return {{threw: true, name: e.name, message: e.message}}; }}
        }}
        var out = {{}};
        out.set_undefined = capture(function() {{ tx.Memos[0] = undefined; }});
        out.set_scalar = capture(function() {{ tx.Memos[0] = 42; }});
        out.set_null = capture(function() {{ tx.Memos[0] = null; }});
        out.memo_data = tx.Memos[0].Memo.MemoData;
        out.enc_unchanged =
            util_hex(encode_object(tx)).toUpperCase() === hex.toUpperCase();
        JSON.stringify(out);
    """)
    assert_result(r)
    data = json.loads(r["result"])
    for key in ("set_undefined", "set_scalar", "set_null"):
        assert data[key]["threw"] is True, key
        assert data[key]["name"] == "TypeError", key
        assert STARRAY_SET_MESSAGE in data[key]["message"], key
    assert data["memo_data"] == "48656C6C6F"
    assert data["enc_unchanged"] is True
