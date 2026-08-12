"""Test decode_object with various fixture types."""

import json
import pytest
from conftest import FIXTURE_DIR, assert_result


CODEC_JSON_XFAILS = {
    # STArray views currently stringify as object-index maps; M2 will settle
    # the array protocol. Re-encoding this fixture is still asserted below.
    "Payment_with_memos": (
        "Payment_with_memos: decoded Memos is {'0': {'Memo': ...}} but "
        "fixture expects [{'Memo': ...}]"
    ),
}


def _codec_fixture_params(xfails=None):
    xfails = xfails or {}
    with open(FIXTURE_DIR / "codec-fixtures.json") as f:
        fixtures = json.load(f)["stobject"]
    params = []
    for fixture in fixtures:
        name = fixture["name"]
        marks = []
        if name in xfails:
            marks.append(pytest.mark.xfail(reason=xfails[name], strict=True))
        params.append(pytest.param(fixture, id=name, marks=marks))
    return params


class TestBasicDecode:
    def test_payment(self, js_runner):
        r = js_runner("""
            var hex = "12000024000000016140000000000F424068400000000000000A8114B5F762798A53D543A014CAF8B297CFF8F2F937E8831449FF0C73CA6AF9733DA805F76CA2C37776B7C46B";
            var tx = decode_object(hex);
            JSON.stringify({
                type: tx.TransactionType,
                account: tx.Account,
                dest: tx.Destination,
                amount: tx.Amount,
                fee: tx.Fee,
                seq: tx.Sequence,
                keys: Object.keys(tx)
            });
        """)
        data = json.loads(r["result"])
        assert data["type"] == "Payment"
        assert data["account"] == "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh"
        assert data["dest"] == "rfkE1aSy9G8Upk4JssnwBxhEv5p4mn2KTy"
        assert data["amount"] == "1000000"
        assert data["fee"] == "10"
        assert data["seq"] == 1
        assert len(data["keys"]) == 6

    def test_instanceof_stobject(self, js_runner):
        r = js_runner("""
            var hex = "12000024000000016140000000000F424068400000000000000A8114B5F762798A53D543A014CAF8B297CFF8F2F937E8831449FF0C73CA6AF9733DA805F76CA2C37776B7C46B";
            var tx = decode_object(hex);
            JSON.stringify({
                is_stobj: tx instanceof STObject,
                plain_is_not: !({} instanceof STObject),
                new_is: (new STObject()) instanceof STObject
            });
        """)
        data = json.loads(r["result"])
        assert data["is_stobj"] is True
        assert data["plain_is_not"] is True
        assert data["new_is"] is True

    def test_lazy_field_access(self, js_runner):
        """Accessing one field should not decode all fields."""
        r = js_runner("""
            var hex = "12000024000000016140000000000F424068400000000000000A8114B5F762798A53D543A014CAF8B297CFF8F2F937E8831449FF0C73CA6AF9733DA805F76CA2C37776B7C46B";
            var tx = decode_object(hex);
            JSON.stringify(tx.TransactionType);
        """)
        assert r["result"] is not None, f"No result. Error: {r.get('error')} Raw: {r['raw'][:300]}"
        assert json.loads(r["result"]) == "Payment"


class TestBytesLikeInput:
    HEX = "12000024000000016140000000000F424068400000000000000A8114B5F762798A53D543A014CAF8B297CFF8F2F937E8831449FF0C73CA6AF9733DA805F76CA2C37776B7C46B"

    HEX_EXPECTED = {
        "type": "Payment",
        "account": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
        "dest": "rfkE1aSy9G8Upk4JssnwBxhEv5p4mn2KTy",
        "amount": "1000000",
        "fee": "10",
        "seq": 1,
    }

    def test_hex_string(self, js_runner):
        r = js_runner(f"""
            var hex = "{self.HEX}";
            var tx = decode_object(hex);
            JSON.stringify(tx.TransactionType);
        """)
        assert json.loads(r["result"]) == "Payment"

    def test_uint8array(self, js_runner):
        r = js_runner(f"""
            var hex = "{self.HEX}";
            var bytes = new Uint8Array(hex.length / 2);
            for (var i = 0; i < hex.length; i += 2)
                bytes[i / 2] = parseInt(hex.substr(i, 2), 16);
            var tx = decode_object(bytes);
            JSON.stringify(tx.TransactionType);
        """)
        assert json.loads(r["result"]) == "Payment"

    def test_uint8array_detach_after_decode_does_not_affect_lazy_reads(self, js_runner):
        r = js_runner(f"""
            var hex = "{self.HEX}";
            function bytesFromHex(s) {{
                var bytes = new Uint8Array(s.length / 2);
                for (var i = 0; i < s.length; i += 2)
                    bytes[i / 2] = parseInt(s.substr(i, 2), 16);
                return bytes;
            }}
            function snapshot(tx) {{
                return {{
                    type: tx.TransactionType,
                    account: tx.Account,
                    dest: tx.Destination,
                    amount: tx.Amount,
                    fee: tx.Fee,
                    seq: tx.Sequence
                }};
            }}

            var source = bytesFromHex(hex);
            var tx = decode_object(source);
            var used_transfer = typeof source.buffer.transfer === "function";
            var source_detached = false;
            if (used_transfer) {{
                var moved = source.buffer.transfer();
                source_detached = source.buffer.byteLength === 0;
                new Uint8Array(moved).fill(0);
            }} else {{
                source.fill(0);
            }}

            var first = snapshot(tx);
            var second = snapshot(tx);
            var tx_json = JSON.parse(JSON.stringify(tx));
            JSON.stringify({{
                used_transfer: used_transfer,
                source_detached: source_detached,
                first: first,
                second: second,
                tx_json: tx_json
            }});
        """)
        assert_result(r)
        data = json.loads(r["result"])
        if data["used_transfer"]:
            assert data["source_detached"] is True
        assert data["first"] == self.HEX_EXPECTED
        assert data["second"] == self.HEX_EXPECTED
        assert data["tx_json"] == {
            "TransactionType": "Payment",
            "Sequence": 1,
            "Amount": "1000000",
            "Fee": "10",
            "Account": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
            "Destination": "rfkE1aSy9G8Upk4JssnwBxhEv5p4mn2KTy",
        }

    def test_uint8array_mutation_after_decode_does_not_affect_view_or_encode(self, js_runner):
        r = js_runner(f"""
            var hex = "{self.HEX}";
            var source = new Uint8Array(hex.length / 2);
            for (var i = 0; i < hex.length; i += 2)
                source[i / 2] = parseInt(hex.substr(i, 2), 16);
            var tx = decode_object(source);
            source.fill(0);
            JSON.stringify({{
                fields: {{
                    type: tx.TransactionType,
                    account: tx.Account,
                    dest: tx.Destination,
                    amount: tx.Amount,
                    fee: tx.Fee,
                    seq: tx.Sequence
                }},
                rehex: util_hex(encode_object(tx)).toUpperCase()
            }});
        """)
        assert_result(r)
        data = json.loads(r["result"])
        assert data["fields"] == self.HEX_EXPECTED
        assert data["rehex"] == self.HEX.upper()

    def test_arraybuffer(self, js_runner):
        r = js_runner(f"""
            var hex = "{self.HEX}";
            var bytes = new Uint8Array(hex.length / 2);
            for (var i = 0; i < hex.length; i += 2)
                bytes[i / 2] = parseInt(hex.substr(i, 2), 16);
            var tx = decode_object(bytes.buffer);
            JSON.stringify(tx.TransactionType);
        """)
        assert json.loads(r["result"]) == "Payment"

    def test_array_number(self, js_runner):
        r = js_runner(f"""
            var hex = "{self.HEX}";
            var arr = [];
            for (var i = 0; i < hex.length; i += 2)
                arr.push(parseInt(hex.substr(i, 2), 16));
            var tx = decode_object(arr);
            JSON.stringify(tx.TransactionType);
        """)
        assert json.loads(r["result"]) == "Payment"


class TestSTArray:
    MEMO_HEX = "1200002200000000240000006461400000000007A12068400000000000000C8114B5F762798A53D543A014CAF8B297CFF8F2F937E88314F667B0CA50CC7709A220B0561B85E53A48461FA8F9EA7C0A746578742F706C61696E7D0548656C6C6FE1F1"

    def test_array_access(self, js_runner):
        r = js_runner(f"""
            var hex = "{self.MEMO_HEX}";
            var tx = decode_object(hex);
            var memos = tx.Memos;
            JSON.stringify({{
                length: memos.length,
                has_memo: "Memo" in memos[0],
                memo_type: memos[0].Memo.MemoType,
                memo_data: memos[0].Memo.MemoData
            }});
        """)
        data = json.loads(r["result"])
        assert data["length"] == 1
        assert data["has_memo"] is True
        assert data["memo_type"] == "746578742F706C61696E"  # "text/plain" in hex
        assert data["memo_data"] == "48656C6C6F"  # "Hello" in hex

    def test_array_element_views_have_stable_identity_and_shape(self, js_runner):
        r = js_runner(f"""
            var hex = "{self.MEMO_HEX}";
            var tx = decode_object(hex);
            var first = tx.Memos[0];
            JSON.stringify({{
                element_is_stobject: first instanceof STObject,
                element_identity: first === tx.Memos[0],
                inner_identity: first.Memo === first.Memo,
                wrapper_json: JSON.stringify(first),
                rehex: util_hex(encode_object(tx)).toUpperCase()
            }});
        """)
        assert_result(r)
        data = json.loads(r["result"])
        assert data["element_is_stobject"] is True
        assert data["element_identity"] is True
        assert data["inner_identity"] is True
        assert json.loads(data["wrapper_json"]) == {
            "Memo": {
                "MemoType": "746578742F706C61696E",
                "MemoData": "48656C6C6F",
            }
        }
        assert data["rehex"] == self.MEMO_HEX

    def test_retained_nested_child_survives_root_drop_under_gc_pressure(self, js_runner):
        r = js_runner(f"""
            var hex = "{self.MEMO_HEX}";
            var tx = decode_object(hex);
            var memo = tx.Memos[0].Memo;
            var fee = tx.Fee;
            tx = null;

            // The sandbox does not expose gc(); create and discard enough
            // objects and strings to put pressure on QuickJS's collector.
            for (var i = 0; i < 4000; ++i) {{
                var junk = {{
                    index: i,
                    text: "pressure-" + i + "-" + hex
                }};
                junk.more = [junk.text, {{copy: junk.text + i}}];
            }}

            JSON.stringify({{
                fee: fee,
                memo_type: memo.MemoType,
                memo_data: memo.MemoData
            }});
        """)
        assert_result(r)
        data = json.loads(r["result"])
        assert data == {
            "fee": "12",
            "memo_type": "746578742F706C61696E",
            "memo_data": "48656C6C6F",
        }

    def test_decode_nested_drop_churn_completes_under_memory_limit(self, js_runner):
        r = js_runner(f"""
            var hex = "{self.MEMO_HEX}";
            var checksum = 0;
            for (var i = 0; i < 500; ++i) {{
                var tx = decode_object(hex);
                var memo = tx.Memos[0].Memo;
                checksum += memo.MemoData.length;
                tx = null;
                memo = null;
                var junk = "pressure-" + i + "-" + hex;
                var obj = {{junk: junk, list: [junk, junk + i]}};
            }}
            JSON.stringify({{checksum: checksum}});
        """, timeout=60)
        assert_result(r)
        assert json.loads(r["result"]) == {"checksum": 500 * len("48656C6C6F")}

    def test_enumeration_and_json_stringify_shape_stay_unchanged(self, js_runner):
        r = js_runner(f"""
            var hex = "{self.MEMO_HEX}";
            var tx = decode_object(hex);
            var memos = tx.Memos;
            var first = memos[0];
            JSON.stringify({{
                root_keys: Object.keys(tx),
                memos_keys: Object.keys(memos),
                element_keys: Object.keys(first),
                json: JSON.stringify(tx),
                rehex: util_hex(encode_object(tx)).toUpperCase()
            }});
        """)
        assert_result(r)
        data = json.loads(r["result"])
        assert data["root_keys"] == [
            "TransactionType",
            "Flags",
            "Sequence",
            "Amount",
            "Fee",
            "Account",
            "Destination",
            "Memos",
        ]
        assert data["memos_keys"] == ["0"]
        assert data["element_keys"] == ["Memo"]
        assert data["json"] == (
            '{"TransactionType":"Payment","Flags":0,"Sequence":100,'
            '"Amount":"500000","Fee":"12",'
            '"Account":"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",'
            '"Destination":"rPT1Sjq2YGrBMTttX4GZHjKu9dyfzbpAYe",'
            '"Memos":{"0":{"Memo":{"MemoType":"746578742F706C61696E",'
            '"MemoData":"48656C6C6F"}}}}'
        )
        assert data["rehex"] == self.MEMO_HEX

    def test_empty_array_json_stringify(self, js_runner):
        r = js_runner("""
            var encoded = encode_object({
                TransactionType: "Payment",
                Sequence: 1,
                Amount: "1000000",
                Fee: "10",
                Account: "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
                Destination: "rfkE1aSy9G8Upk4JssnwBxhEv5p4mn2KTy",
                Memos: []
            });
            var tx = decode_object(encoded);
            var tx_json = JSON.stringify(tx);
            JSON.stringify({
                length: tx.Memos.length,
                memos_json: JSON.stringify(tx.Memos),
                tx_json: tx_json
            });
        """)
        assert_result(r)
        data = json.loads(r["result"])
        assert data["length"] == 0
        assert data["memos_json"] == "{}"
        assert json.loads(data["tx_json"])["Memos"] == {}


class TestRoundtrip:
    def test_basic_roundtrip(self, js_runner):
        hex_val = "12000024000000016140000000000F424068400000000000000A8114B5F762798A53D543A014CAF8B297CFF8F2F937E8831449FF0C73CA6AF9733DA805F76CA2C37776B7C46B"
        r = js_runner(f"""
            var hex = "{hex_val}";
            var tx = decode_object(hex);
            var re = util_hex(encode_object(tx));
            JSON.stringify(hex.toUpperCase() === re.toUpperCase());
        """)
        assert json.loads(r["result"]) is True

    def test_fast_path_unmodified(self, js_runner):
        """Unmodified STObject should use fast path (original bytes)."""
        hex_val = "12000024000000016140000000000F424068400000000000000A8114B5F762798A53D543A014CAF8B297CFF8F2F937E8831449FF0C73CA6AF9733DA805F76CA2C37776B7C46B"
        r = js_runner(f"""
            var hex = "{hex_val}";
            var tx = decode_object(hex);
            var re = util_hex(encode_object(tx));
            JSON.stringify(re);
        """)
        assert json.loads(r["result"]) == hex_val.upper()


class TestLazySTObject:
    """Tests for the lazy STObject exotic class behavior."""
    HEX = "12000024000000016140000000000F424068400000000000000A8114B5F762798A53D543A014CAF8B297CFF8F2F937E8831449FF0C73CA6AF9733DA805F76CA2C37776B7C46B"

    def test_object_keys(self, js_runner):
        """Object.keys should return all field names from the offset map."""
        r = js_runner(f"""
            var tx = decode_object("{self.HEX}");
            var keys = Object.keys(tx);
            JSON.stringify(keys.sort());
        """)
        assert_result(r)
        keys = json.loads(r["result"])
        assert sorted(keys) == sorted(["TransactionType", "Sequence", "Amount", "Fee", "Account", "Destination"])

    def test_json_stringify(self, js_runner):
        """JSON.stringify should trigger lazy decode of all fields."""
        r = js_runner(f"""
            var tx = decode_object("{self.HEX}");
            JSON.stringify(tx);
        """)
        assert_result(r)
        data = json.loads(r["result"])
        assert data["TransactionType"] == "Payment"
        assert data["Account"] == "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh"
        assert data["Destination"] == "rfkE1aSy9G8Upk4JssnwBxhEv5p4mn2KTy"
        assert data["Amount"] == "1000000"
        assert data["Fee"] == "10"
        assert data["Sequence"] == 1

    def test_property_cache_hit(self, js_runner):
        """Accessing the same property twice should return the same value (cache)."""
        r = js_runner(f"""
            var tx = decode_object("{self.HEX}");
            var a = tx.TransactionType;
            var b = tx.TransactionType;
            JSON.stringify({{
                same: a === b,
                val_a: a,
                val_b: b
            }});
        """)
        assert_result(r)
        data = json.loads(r["result"])
        assert data["same"] is True
        assert data["val_a"] == "Payment"
        assert data["val_b"] == "Payment"

    def test_property_mutation_updates_view(self, js_runner):
        r = js_runner(f"""
            var hex = "{self.HEX}";
            var tx = decode_object(hex);
            tx.Fee = "20";
            JSON.stringify({{
                fee: tx.Fee
            }});
        """)
        assert_result(r)
        data = json.loads(r["result"])
        assert data["fee"] == "20"


class TestFixtures:
    @pytest.mark.parametrize("fixture", _codec_fixture_params(CODEC_JSON_XFAILS))
    def test_codec_fixtures_decode_expected_json(self, js_runner, fixture):
        """Codec fixtures should decode to the fixture's expected JSON."""
        name = fixture["name"]
        fixture_hex = json.dumps(fixture["hex"])
        expected = fixture["decoded"]
        r = js_runner(f"""
            var obj = decode_object({fixture_hex});
            JSON.stringify(JSON.parse(JSON.stringify(obj)));
        """)
        assert_result(r, name)
        assert json.loads(r["result"]) == expected

    @pytest.mark.parametrize("fixture", _codec_fixture_params())
    def test_codec_fixtures_reencode_original_hex(self, js_runner, fixture):
        """Codec fixtures should re-encode byte-identically after decode."""
        name = fixture["name"]
        fixture_hex = json.dumps(fixture["hex"])
        r = js_runner(f"""
            var obj = decode_object({fixture_hex});
            JSON.stringify(util_hex(encode_object(obj)).toUpperCase());
        """)
        assert_result(r, name)
        assert json.loads(r["result"]) == fixture["hex"].upper()

    def test_sle_fixtures_decode(self, js_runner, sle_fixtures):
        """First 20 SLE fixtures should decode."""
        hexes = json.dumps([f["hex"] for f in sle_fixtures[:20]])
        r = js_runner(f"""
            var hexes = {hexes};
            var pass_count = 0;
            for (var i = 0; i < hexes.length; i++) {{
                try {{
                    var obj = decode_object(hexes[i]);
                    if (Object.keys(obj).length > 0) pass_count++;
                }} catch(e) {{}}
            }}
            JSON.stringify(pass_count);
        """)
        assert json.loads(r["result"]) == 20


class TestUnknownFieldCode:
    """An unrecognised field code must fail, not end the object.

    Decided 2026-07-27 (issue 0016). The scan is also the validator, and it
    used to `return true` on a code it did not recognise — so a buffer whose
    tail happened to parse as a field header decoded clean and short, with no
    error. Validated now means whole-buffer.

    No fixture reaches these paths (every fixture is all-known fields), so the
    codes here are constructed. `10C8` is type UInt16 (1), nth 200, which the
    Xahau definitions do not define: code (1 << 16) | 200 = 65736.
    """

    PAYMENT = ("12000024000000016140000000000F424068400000000000000A"
               "8114B5F762798A53D543A014CAF8B297CFF8F2F937E8"
               "831449FF0C73CA6AF9733DA805F76CA2C37776B7C46B")

    def test_trailing_unknown_field_throws(self, js_runner):
        r = js_runner(f"""
            try {{
                decode_object("{self.PAYMENT}" + "10C80000");
                JSON.stringify({{threw: false}});
            }} catch (e) {{
                JSON.stringify({{threw: true, msg: String(e)}});
            }}
        """)
        data = json.loads(r["result"])
        assert data["threw"], "unknown field code decoded without error"
        assert "unknown field code" in data["msg"], data["msg"]

    def test_known_fields_still_decode(self, js_runner):
        """The failure path must not have broken the success path."""
        r = js_runner(f"""
            var tx = decode_object("{self.PAYMENT}");
            JSON.stringify({{keys: Object.keys(tx).length, type: tx.TransactionType}});
        """)
        data = json.loads(r["result"])
        assert data["type"] == "Payment"
        assert data["keys"] == 6


class TestMalformedFieldHeader:
    """A non-canonical or truncated field header must fail, not end the object.

    `read_field_header_checked` returns *success* with `field_code = 0` when a
    continuation byte is 0 or < 16. A continuation byte is only legal for a
    value >= 16 — below that the value belongs in the nibble — so those are
    malformed or non-canonically encoded headers, not terminators. The format's
    terminator is the end-of-object marker; there is no "zero field code".

    Callers tested `field_code == 0` and ended the object successfully, which is
    issue 0016's bug wearing a different hat.
    """

    PAYMENT = TestUnknownFieldCode.PAYMENT

    def test_noncanonical_type_byte_throws(self, js_runner):
        # 0x00 -> type nibble 0, read continuation; 0x01 < 16 is non-canonical.
        r = js_runner(f"""
            try {{
                decode_object("{self.PAYMENT}" + "0001");
                JSON.stringify({{threw: false}});
            }} catch (e) {{
                JSON.stringify({{threw: true, msg: String(e)}});
            }}
        """)
        data = json.loads(r["result"])
        assert data["threw"], "non-canonical type byte decoded without error"

    def test_noncanonical_field_byte_throws(self, js_runner):
        # 0x10 -> type 1, field nibble 0, read continuation; 0x05 < 16.
        r = js_runner(f"""
            try {{
                decode_object("{self.PAYMENT}" + "1005");
                JSON.stringify({{threw: false}});
            }} catch (e) {{
                JSON.stringify({{threw: true, msg: String(e)}});
            }}
        """)
        data = json.loads(r["result"])
        assert data["threw"], "non-canonical field byte decoded without error"
