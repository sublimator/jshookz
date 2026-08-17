"""Differential JSON oracle across codec backends."""

from __future__ import annotations

import json
import shutil
import subprocess

import pytest

from conftest import CODEC, CODEC_WASM, assert_result, run_js


DIFFERENTIAL = CODEC / "differential"
NODE_HARNESS = DIFFERENTIAL / "decode-fixtures.cjs"
NODE_MODULE = DIFFERENTIAL / "node_modules" / "ripple-binary-codec"
XAHAU_DEFINITIONS = (
    CODEC.parent / "x-data" / "definitions" / "xahau_definitions.json"
)


def _fixture_items(codec_fixtures, sle_fixtures, tx_fixtures):
    items = []
    for idx, fixture in enumerate(codec_fixtures):
        items.append(
            {
                "category": "codec",
                "index": idx,
                "name": fixture.get("name", str(idx)),
                "hex": fixture["hex"],
            }
        )
    for idx, fixture in enumerate(sle_fixtures):
        items.append(
            {
                "category": "sle",
                "index": idx,
                "name": fixture.get("type", str(idx)),
                "hex": fixture["hex"],
            }
        )
    for idx, fixture in enumerate(tx_fixtures):
        items.append(
            {
                "category": "tx",
                "index": idx,
                "name": fixture.get("name", str(idx)),
                "hex": fixture["binary"],
            }
        )
        if "meta_binary" in fixture:
            items.append(
                {
                    "category": "tx_meta",
                    "index": idx,
                    "name": fixture.get("name", str(idx)) + ":meta",
                    "hex": fixture["meta_binary"],
                }
            )
    return items


def _wasm_script(items):
    return (
        "var items = "
        + json.dumps(items)
        + ";\n"
        + "var out = [];\n"
        + "for (var i = 0; i < items.length; i++) {\n"
        + "  var item = items[i];\n"
        + "  try {\n"
        + "    var obj = decode_object(item.hex);\n"
        + "    out.push({category:item.category,index:item.index,"
        + "name:item.name,decoded:JSON.parse(JSON.stringify(obj))});\n"
        + "  } catch (e) {\n"
        + "    out.push({category:item.category,index:item.index,"
        + "name:item.name,error:String(e && e.message ? e.message : e)});\n"
        + "  }\n"
        + "}\n"
        + "JSON.stringify(out);\n"
    )


def _run_wasm_backend(host_path, wasm_path, items):
    r = run_js(host_path, wasm_path, _wasm_script(items), timeout=120)
    assert_result(r, str(wasm_path))
    return json.loads(r["result"])


def _run_node_backend(items, tmp_path):
    if shutil.which("node") is None:
        pytest.fail("required differential-test dependency is missing: node")
    if not NODE_MODULE.exists():
        pytest.fail(
            "required differential-test dependency is missing: run "
            "npm ci --prefix cpp/x-data-quickjs/differential --ignore-scripts"
        )
    input_path = tmp_path / "differential-items.json"
    input_path.write_text(json.dumps(items))
    result = subprocess.run(
        [
            "node",
            str(NODE_HARNESS),
            str(XAHAU_DEFINITIONS),
            str(input_path),
        ],
        capture_output=True,
        text=True,
        timeout=120,
    )
    if result.returncode != 0:
        raise AssertionError(
            f"node differential harness failed: rc={result.returncode}\n"
            f"{(result.stdout + result.stderr)[:2000]}"
        )
    return json.loads(result.stdout)


def _normal_json(value):
    if isinstance(value, list):
        return [_normal_json(item) for item in value]
    if isinstance(value, dict):
        # QuickJS lazy STArray views currently stringify as {"0": item, ...},
        # while ripple-binary-codec emits JSON arrays. Convert only contiguous
        # decimal-key maps, then sort ordinary object keys for stable diffs.
        if value and all(key.isdecimal() for key in value):
            indexes = sorted(int(key) for key in value)
            if indexes == list(range(len(indexes))):
                return [_normal_json(value[str(index)]) for index in indexes]
        return {key: _normal_json(value[key]) for key in sorted(value)}
    return value


def _rows_by_key(rows):
    return {(row["category"], row["index"], row["name"]): row for row in rows}


def _assert_same_json(left_name, left_rows, right_name, right_rows, items):
    expected_keys = {
        (item["category"], item["index"], item["name"]) for item in items
    }
    left_by_key = _rows_by_key(left_rows)
    right_by_key = _rows_by_key(right_rows)
    assert set(left_by_key) == expected_keys
    assert set(right_by_key) == expected_keys

    failures = []
    for item in items:
        key = (item["category"], item["index"], item["name"])
        left = left_by_key[key]
        right = right_by_key[key]
        if "error" in left or "error" in right:
            failures.append(
                f"{key}: {left_name} error={left.get('error')} "
                f"{right_name} error={right.get('error')}"
            )
            continue

        left_json = _normal_json(left["decoded"])
        right_json = _normal_json(right["decoded"])
        if left_json != right_json:
            failures.append(
                f"{key}: {left_name}={json.dumps(left_json, sort_keys=True)} "
                f"{right_name}={json.dumps(right_json, sort_keys=True)}"
            )

    assert not failures, "\n".join(failures[:20])


def test_xdata_matches_ripple_binary_codec(
    host_path, tmp_path, codec_fixtures, sle_fixtures, tx_fixtures
):
    if not CODEC_WASM.exists():
        pytest.fail(f"required codec WASM not found: {CODEC_WASM}")

    items = _fixture_items(codec_fixtures, sle_fixtures, tx_fixtures)
    xdata_rows = _run_wasm_backend(host_path, CODEC_WASM, items)
    node_rows = _run_node_backend(items, tmp_path)
    _assert_same_json("xdata", xdata_rows, "ripple-binary-codec", node_rows, items)
