"""Join // @binding provider: tags vs the projected v1 surface.

cpp/xahau-types must not include hook_imports.hpp or call hook_* / host_*.
Host crossings live in cpp/provider/bindings/. Result is a JS class, not a
host call. Tags stay on the provider plane: they are the product JS surface.
"""

from __future__ import annotations

import json
import re
from pathlib import Path

from jshookz.host import WasmHost
from jshookz.paths import (
    REPO_ROOT,
    XAHAU_HOOK_PROVIDER_WASM,
    XAHAU_V1_CONSENSUS_ENTROPY_JAVASCRIPT_SURFACE,
    XAHAU_V1_JAVASCRIPT_SURFACE,
)

_TAG = re.compile(
    r"^\s*//\s*@binding\s+"
    r"(?P<plane>provider):(?P<path>[A-Za-z][\w.\[\]]*)\s*$"
)
_HOST_CALL = re.compile(r"\b(?:hook_|host_)[A-Za-z]\w*\s*\(")
_HOOK_IMPORT = re.compile(r'^\s*#\s*include\s*[<"][^>"]*hook_imports\.hpp[>"]')
_UINT_WIDTHS = ("8", "16", "32", "64")
PROVIDER_ONLY = frozenset({"CallbackInfo", "CallbackInfo.invocationId"})
_XAHAU_DEFINITIONS = REPO_ROOT / "cpp/x-data/definitions/xahau_definitions.json"
_PROVIDER_STATIC_POLICY = (
    REPO_ROOT / "cpp/x-data/definitions/provider_static_policy.json"
)
_FIELD_SENTINELS = frozenset({"ObjectEndMarker", "ArrayEndMarker"})
_MATERIAL_PROFILES = {
    "account_id": "AccountID",
    "amount": "Amount",
    "blob": "STBlob",
    "currency": "Currency",
    "hash128": "Hash128",
    "hash160": "Hash160",
    "hash192": "Hash192",
    "hash256": "Hash256",
    "issue": "Issue",
    "path_set": "PathSet",
    "st_array": "STArray",
    "st_object": "STObject",
    "uint8": "UInt",
    "uint16": "UInt",
    "uint32": "UInt",
    "uint64": "UInt",
    "vector256": "Vector256",
    "xchain_bridge": "XChainBridge",
}
_UINT_BITS = {"uint8": 8, "uint16": 16, "uint32": 32, "uint64": 64}
_FIXED_PAYLOAD_SIZES = {
    "ledger_entry_type": 2,
    "transaction_result": 1,
    "transaction_type": 2,
    "uint8": 1,
    "uint16": 2,
    "uint32": 4,
    "uint64": 8,
    "hash128": 16,
    "hash160": 20,
    "hash192": 24,
    "hash256": 32,
    "currency": 20,
}


def _cpp_roots() -> list[Path]:
    return [
        REPO_ROOT / "cpp" / "provider",
        REPO_ROOT / "cpp" / "xahau-types",
        REPO_ROOT / "cpp" / "x-data" / "generated",
    ]


def _iter_tagged() -> list[tuple[str, str, Path]]:
    found: list[tuple[str, str, Path]] = []
    for root in _cpp_roots():
        for path in root.rglob("*"):
            if path.suffix not in {".cpp", ".hpp", ".h"}:
                continue
            for line in path.read_text(encoding="utf-8").splitlines():
                match = _TAG.match(line)
                if match:
                    found.append((match.group("plane"), match.group("path"), path))
    return found


def _surface_paths(surface: dict) -> set[str]:
    paths: set[str] = set()
    for name, kind in surface["globals"].items():
        if kind == "function":
            paths.add(name)
    for namespace, members in surface["namespaces"].items():
        paths.update(f"{namespace}.{name}" for name in members)
    for name, spec in surface["prototypes"].items():
        paths.update(f"{name}.{member}" for member in spec["members"])
    for name, members in surface["statics"].items():
        paths.update(f"{name}.{member}" for member in members)
    return paths


def _expand(path: str) -> set[str]:
    names = {path}
    if path.startswith("UInt.") and not path.startswith("UInt8"):
        rest = path[len("UInt.") :]
        names.update(f"UInt{width}.{rest}" for width in _UINT_WIDTHS)
    if path.startswith("Result."):
        names.add("VoidResult." + path[len("Result.") :])
    if path.startswith("LedgerEntry."):
        member = path[len("LedgerEntry.") :]
        names.update(
            f"{leaf}.{member}"
            for leaf in ("AccountRoot", "HookDefinition", "HookLedger", "URIToken")
        )
    return names


def _field_header(type_code: int, field_code: int) -> bytes:
    header = bytearray(
        [
            (type_code if type_code < 16 else 0) << 4
            | (field_code if field_code < 16 else 0)
        ]
    )
    if type_code >= 16:
        header.append(type_code)
    if field_code >= 16:
        header.append(field_code)
    return bytes(header)


def _field_payload(materializer: str, *, vl_encoded: bool) -> bytes:
    if vl_encoded:
        # Keep every VL row nonempty so the 325-row runtime join exercises
        # canonical JSON and byte emission for Blob, AccountID, and Vector256.
        if materializer == "account_id":
            return bytes.fromhex("14B5F762798A53D543A014CAF8B297CFF8F2F937E8")
        if materializer == "vector256":
            return b"\x20" + bytes(range(1, 33))
        return b"\x01\xab"
    if materializer == "amount":
        return b"\x40" + bytes(7)
    if materializer == "number":
        return bytes.fromhex("000470DE4DF82000FFFFFFF1")  # canonical 1.25
    if materializer == "st_object":
        return b"\xe1"
    if materializer == "st_array":
        return b"\xf1"
    if materializer == "path_set":
        return b"\x01" + bytes(20) + b"\x00"
    if materializer == "issue":
        return bytes(20)
    if materializer == "xchain_bridge":
        return b"\x00" + bytes(20) + b"\x00" + bytes(20)
    return bytes(_FIXED_PAYLOAD_SIZES[materializer])


def _expected_field_rows() -> list[dict[str, object]]:
    definitions = json.loads(_XAHAU_DEFINITIONS.read_text(encoding="utf-8"))
    policy = json.loads(_PROVIDER_STATIC_POLICY.read_text(encoding="utf-8"))
    type_codes = definitions["TYPES"]
    type_materializers = policy["wire_type_materializers"]
    overrides = policy["descriptor_overrides"]
    rows: list[dict[str, object]] = []
    seen: set[str] = set()

    for name, metadata in definitions["FIELDS"]:
        if name in seen:
            continue
        seen.add(name)
        if not metadata["isSerialized"] or name in _FIELD_SENTINELS:
            continue
        type_code = type_codes[metadata["type"]]
        field_code = metadata["nth"]
        materializer = overrides.get(name, type_materializers[metadata["type"]])
        encoded_payload = _field_payload(
            materializer,
            vl_encoded=metadata["isVLEncoded"],
        )
        wire_payload_size = (
            encoded_payload[0] if metadata["isVLEncoded"] else len(encoded_payload)
        )
        rows.append(
            {
                "name": name,
                "code": (type_code << 16) | field_code,
                "typeCode": type_code,
                "fieldCode": field_code,
                "materializer": materializer,
                "profile": _MATERIAL_PROFILES.get(materializer),
                "uintBits": _UINT_BITS.get(materializer),
                "wire": (_field_header(type_code, field_code) + encoded_payload).hex(),
                "wirePayloadSize": wire_payload_size,
            }
        )
    return rows


def _field_join_javascript(
    rows: list[dict[str, object]], profiles: dict[str, object]
) -> str:
    encoded_rows = json.dumps(json.dumps(rows, separators=(",", ":")))
    encoded_profiles = json.dumps(json.dumps(profiles, separators=(",", ":")))
    return f"""
JSON.stringify((() => {{
  const expected = JSON.parse({encoded_rows});
  const profiles = JSON.parse({encoded_profiles});
  const failures = [];
  const ownKeys = Reflect.ownKeys(Field);
  const ownNames = Object.getOwnPropertyNames(Field);
  const enumerableNames = Object.keys(Field);
  const descriptors = Object.getOwnPropertyDescriptors(Field);
  const values = Object.values(Field);
  const entries = Object.entries(Field);
  const spread = {{...Field}};
  const assigned = Object.assign({{}}, Field);
  const identities = new Set();
  const descriptorPrototype = Object.getPrototypeOf(Field[expected[0].name]);
  const materialPrototypes = {{}};
  const root = util.decodeObject(STBlob.fromHex(expected.map(row => row.wire).join("")));
  const rendered = root.toJSON();
  const canonicalHex = Array.from(root.toBytes(), byte =>
    byte.toString(16).padStart(2, "0")).join("");

  const compareNames = (label, actual, wanted) => {{
    if (actual.length !== wanted.length ||
        actual.some((name, index) => name !== wanted[index]))
      failures.push(`${{label}}: expected ${{JSON.stringify(wanted)}}, got ${{JSON.stringify(actual)}}`);
  }};
  const prototypeNames = prototype => {{
    const names = Object.getOwnPropertyNames(prototype);
    if (Object.hasOwn(prototype, Symbol.iterator)) names.push("[Symbol.iterator]");
    return names.sort();
  }};

  for (const [name, profile] of Object.entries(profiles)) {{
    const receiver = eval(profile.probe);
    const prototype = Object.getPrototypeOf(receiver);
    materialPrototypes[name] = prototype;
    compareNames(`material prototype ${{name}}`, prototypeNames(prototype),
                 [...profile.own].sort());
    for (const [member, kind] of Object.entries(profile.members)) {{
      const observed = member === "[Symbol.iterator]"
        ? receiver[Symbol.iterator] : receiver[member];
      if (kind === "function" ? typeof observed !== "function" : !(member in receiver))
        failures.push(`material prototype ${{name}}.${{member}}: expected ${{kind}}`);
    }}
  }}

  const expectedNames = expected.map(row => row.name);
  const expectedWire = expected.map(row => row.wire).join("");
  compareNames("Field Reflect.ownKeys", ownKeys, expectedNames);
  compareNames("Field own names", ownNames, expectedNames);
  compareNames("Field enumerable names", enumerableNames, expectedNames);
  compareNames("STObject.toJSON names", Object.keys(rendered), expectedNames);
  if (canonicalHex !== expectedWire)
    failures.push("STObject.toBytes differs from the complete 325-row wire");
  if (!Object.isFrozen(Field)) failures.push("Field table is not frozen");
  if (Object.getPrototypeOf(Field) !== Object.prototype)
    failures.push("Field table prototype is not Object.prototype");
  compareNames("SerializedField prototype", prototypeNames(descriptorPrototype),
               ["code", "fieldCode", "typeCode"]);
  if (!Object.isFrozen(descriptorPrototype))
    failures.push("SerializedField prototype is not frozen");

  expected.forEach((row, index) => {{
    const own = descriptors[row.name];
    const value = Field[row.name];
    const prefix = `Field.${{row.name}}`;
    if (!Object.hasOwn(rendered, row.name))
      failures.push(`${{prefix}}: toJSON omitted material field`);
    if (!own) {{ failures.push(`${{prefix}}: missing descriptor`); return; }}
    if (own.value !== value || values[index] !== value ||
        entries[index][0] !== row.name || entries[index][1] !== value ||
        spread[row.name] !== value || assigned[row.name] !== value)
      failures.push(`${{prefix}}: reflection identity differs`);
    if (!own.enumerable || own.configurable || own.writable ||
        Object.hasOwn(own, "get") || Object.hasOwn(own, "set"))
      failures.push(`${{prefix}}: property flags differ`);
    if (Object.getPrototypeOf(value) !== descriptorPrototype ||
        !Object.isFrozen(value) || Reflect.ownKeys(value).length !== 0 ||
        value !== Field[row.name])
      failures.push(`${{prefix}}: nominal descriptor identity differs`);
    if (value.code !== row.code || value.typeCode !== row.typeCode ||
        value.fieldCode !== row.fieldCode)
      failures.push(`${{prefix}}: expected ${{row.code}}/${{row.typeCode}}/${{row.fieldCode}}, got ${{value.code}}/${{value.typeCode}}/${{value.fieldCode}}`);
    identities.add(value);

    const material = root.get(value);
    if (!root.has(value) || !root.has(row.name) || material !== root.get(row.name) ||
        material !== root[row.name])
      failures.push(`${{prefix}}: descriptor/name material identity differs`);
    const fieldBytes = root.fieldBytes(value);
    if (fieldBytes === undefined || fieldBytes.byteLength !== row.wirePayloadSize)
      failures.push(`${{prefix}}: payload size differs`);

    if (row.materializer === "number") {{
      if (typeof material !== "string" || material !== "1.25")
        failures.push(`${{prefix}}: expected canonical Number string`);
    }} else if (row.materializer === "ledger_entry_type" ||
               row.materializer === "transaction_type" ||
               row.materializer === "transaction_result") {{
      if (typeof material !== "number" || material !== 0)
        failures.push(`${{prefix}}: expected numeric enum materializer`);
    }} else {{
      if (typeof material !== "object" || material === null ||
          Object.getPrototypeOf(material) !== materialPrototypes[row.profile])
        failures.push(`${{prefix}}: expected ${{row.profile}} materializer`);
      if (row.uintBits !== null && material.bits !== row.uintBits)
        failures.push(`${{prefix}}: expected UInt${{row.uintBits}} materializer`);
    }}
  }});
  if (identities.size !== expected.length)
    failures.push(`Field descriptor identities: expected ${{expected.length}}, got ${{identities.size}}`);
  return failures;
}})())
""".strip()


def test_binding_tags_match_surface():
    baseline = json.loads(XAHAU_V1_JAVASCRIPT_SURFACE.read_text())
    entropy = json.loads(XAHAU_V1_CONSENSUS_ENTROPY_JAVASCRIPT_SURFACE.read_text())
    required = _surface_paths(baseline) | _surface_paths(entropy)
    tagged = _iter_tagged()
    assert tagged, "no // @binding tags found"

    provider = {path for _plane, path, _ in tagged}
    covered = set()
    for path in provider:
        covered.update(_expand(path) & (required | PROVIDER_ONLY | {path}))

    extra = (provider - PROVIDER_ONLY) - required
    extra -= {path for path in extra if _expand(path) & required}
    assert extra == set(), f"provider tags not on a product surface: {sorted(extra)}"

    missing = sorted(required - covered)
    assert missing == [], (
        "product surface members without a provider @binding:\n  "
        + "\n  ".join(missing)
    )


def test_entropy_surface_is_an_exact_named_extension() -> None:
    baseline = _surface_paths(json.loads(XAHAU_V1_JAVASCRIPT_SURFACE.read_text()))
    entropy = _surface_paths(
        json.loads(XAHAU_V1_CONSENSUS_ENTROPY_JAVASCRIPT_SURFACE.read_text())
    )

    assert baseline <= entropy
    assert entropy - baseline == {
        "EntropyTier.consensusFallback",
        "EntropyTier.participantAligned",
        "EntropyTier.validatorFull",
        "EntropyTier.validatorQuorum",
        "entropy.cr",
        "entropy.cr.dice",
        "entropy.cr.status",
    }


def test_sealed_provider_field_table_joins_all_generated_material_rows():
    rows = _expected_field_rows()
    assert len(rows) == 325
    assert len({row["name"] for row in rows}) == 325
    assert len({row["code"] for row in rows}) == 325
    assert len({row["typeCode"] for row in rows}) == 19

    surface = json.loads(XAHAU_V1_JAVASCRIPT_SURFACE.read_text(encoding="utf-8"))
    required_profiles = {row["profile"] for row in rows if row["profile"] is not None}
    profiles = {name: surface["prototypes"][name] for name in sorted(required_profiles)}

    host = WasmHost(wasm_path=XAHAU_HOOK_PROVIDER_WASM)
    host.init()
    try:
        result = host.eval(_field_join_javascript(rows, profiles))
    finally:
        host.destroy()

    assert result.ok, result.error
    assert json.loads(result.result_value) == []


def test_xahau_types_do_not_call_host():
    root = REPO_ROOT / "cpp" / "xahau-types"
    for path in root.rglob("*"):
        if path.suffix not in {".cpp", ".hpp", ".h"}:
            continue
        text = path.read_text(encoding="utf-8")
        includes = [line for line in text.splitlines() if _HOOK_IMPORT.match(line)]
        assert includes == [], f"{path}: xahau-types must not include hook imports"
        hits = _HOST_CALL.findall(text)
        assert hits == [], f"{path}: xahau-types must not call host ({hits})"
