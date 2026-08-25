from __future__ import annotations

from jshookz.host import WasmHost
from jshookz.paths import XAHAU_HOOK_PROVIDER_WASM
from jshookz.runtime_types import SCHEMA, observe_runtime_types

_NOMINAL_MATRIX_GAS = 11_162_169


class _EffectHost:
    def state_set(self, *_args: object) -> int:
        return 0


_NOMINAL_MATRIX = r"""
JSON.stringify((() => {
  const empty = util.decodeObject(new Uint8Array());
  const accountIDValue = AccountID.from(new Uint8Array(20));
  const hash128Value = empty.withField("EmailHash", new Uint8Array(16)).EmailHash;
  const hash160Value = empty.withField(
    "TakerPaysCurrency", new Uint8Array(20)).TakerPaysCurrency;
  const hash192Value = empty.withField(
    "MPTokenIssuanceID", new Uint8Array(24)).MPTokenIssuanceID;
  const hash256Value = Hash256.zero;
  const currencyValue = empty.withField(
    "BaseAsset", new Uint8Array(20)).BaseAsset;
  const issueValue = empty.withField(
    "LockingChainIssue", new Uint8Array(20)).LockingChainIssue;
  const vectorValue = util.decodeObject(STBlob.fromHex(
    "011320000102030405060708090A0B0C0D0E0F" +
    "101112131415161718191A1B1C1D1E1F")).Indexes;
  const bridgeValue = util.decodeObject(STBlob.fromHex(
    "011914B5F762798A53D543A014CAF8B297CFF8F2F937E8" +
    "0000000000000000000000000000000000000000" +
    "14B5F762798A53D543A014CAF8B297CFF8F2F937E8" +
    "0000000000000000000000000000000000000000")).XChainBridge;
  const nativeAmountValue = util.decodeObject(
    STBlob.fromHex("61400000000000002A")).Amount;
  const iouAmountValue = util.decodeObject(STBlob.fromHex(
    "61D4838D7EA4C680000000000000000000000000005553440000000000" +
    "B5F762798A53D543A014CAF8B297CFF8F2F937E8")).Amount;
  const mptAmountValue = util.decodeObject(STBlob.fromHex(
    "61600000000000000001000102030405060708090A0B0C0D0E0F" +
    "1011121314151617")).Amount;
  const pathSetValue = util.decodeObject(STBlob.fromHex(
    "011201B5F762798A53D543A014CAF8B297CFF8F2F937E800")).Paths;
  const pathValue = pathSetValue.at(0);
  const pathHopValue = pathValue.at(0);
  const stArrayValue = util.decodeObject(new Uint8Array([
    0xF9, 0xEA, 0x22, 0, 0, 0, 1, 0xE1, 0xF1,
  ])).Memos;
  const stObjectValue = util.decodeObject(new Uint8Array([0x22, 0, 0, 0, 9]));
  const resultValue = UInt8.from(7);
  const voidResultValue = state.set("runtime-type-matrix", new Uint8Array());
  const xflValue = iouAmountValue.toXFL();

  const nouns = [
    "AccountID", "Amount", "Currency", "Hash", "Hash128", "Hash160",
    "Hash192", "Hash256", "IOUAmount", "Issue", "MPTAmount",
    "NativeAmount", "Path", "PathHop", "PathSet", "Result", "STArray",
    "STBlob", "STObject", "SerializedField", "UInt", "UInt8", "UInt16",
    "UInt32", "UInt64", "Vector256", "VoidResult", "XChainBridge",
    "XFLDecimal",
  ];
  const cases = [
    ["AccountID", accountIDValue, ["AccountID"]],
    ["native amount", nativeAmountValue, ["Amount", "NativeAmount"]],
    ["IOU amount", iouAmountValue, ["Amount", "IOUAmount"]],
    ["MPT amount", mptAmountValue, ["Amount", "MPTAmount"]],
    ["Currency", currencyValue, ["Currency"]],
    ["Hash128", hash128Value, ["Hash", "Hash128"]],
    ["Hash160", hash160Value, ["Hash", "Hash160"]],
    ["Hash192", hash192Value, ["Hash", "Hash192"]],
    ["Hash256", hash256Value, ["Hash", "Hash256"]],
    ["Issue", issueValue, ["Issue"]],
    ["Path", pathValue, ["Path"]],
    ["PathHop", pathHopValue, ["PathHop"]],
    ["PathSet", pathSetValue, ["PathSet"]],
    ["Result", resultValue, ["Result"]],
    ["STArray", stArrayValue, ["STArray"]],
    ["STBlob", STBlob.from(new Uint8Array()), ["STBlob"]],
    ["STObject", stObjectValue, ["STObject"]],
    ["SerializedField", Field.Flags, ["SerializedField"]],
    ["UInt8", UInt8.zero, ["UInt", "UInt8"]],
    ["UInt16", UInt16.zero, ["UInt", "UInt16"]],
    ["UInt32", UInt32.zero, ["UInt", "UInt32"]],
    ["UInt64", UInt64.zero, ["UInt", "UInt64"]],
    ["Vector256", vectorValue, ["Vector256"]],
    ["VoidResult", voidResultValue, ["VoidResult"]],
    ["XChainBridge", bridgeValue, ["XChainBridge"]],
    ["XFLDecimal", xflValue, ["XFLDecimal"]],
  ];
  const failures = [];
  const fail = where => failures.push(where);

  for (const [label, instance, positives] of cases) {
    const expected = new Set(positives);
    for (const name of nouns) {
      if ((instance instanceof globalThis[name]) !== expected.has(name))
        fail(`matrix:${label}:${name}`);
    }
    let callbacks = 0;
    const proxy = new Proxy(instance, {
      get() { ++callbacks; throw new Error("get trap"); },
      getPrototypeOf() { ++callbacks; throw new Error("prototype trap"); },
    });
    for (const name of nouns) {
      if (proxy instanceof globalThis[name]) fail(`proxy:${label}:${name}`);
    }
    if (callbacks !== 0) fail(`proxy-callback:${label}`);

    let clone = {};
    try { clone = JSON.parse(JSON.stringify(instance)); } catch (_) {}
    for (const name of nouns) {
      if (clone instanceof globalThis[name]) fail(`clone:${label}:${name}`);
    }
  }

  const forged = {
    ok: true,
    value: 7,
    bits: 64,
    byteLength: 32,
    kind: "native",
    code: Field.Flags.code,
    typeCode: Field.Flags.typeCode,
    fieldCode: Field.Flags.fieldCode,
    [Symbol.toStringTag]: "Amount",
  };
  const plain = [
    null, undefined, false, 0, 0n, "", "value", {}, forged,
    Object.assign({}, forged), Object.create(forged),
  ];
  for (const candidate of plain) {
    for (const name of nouns) {
      if (candidate instanceof globalThis[name])
        fail(`plain:${String(candidate)}:${name}`);
    }
  }
  return failures;
})())
""".strip()


def test_sealed_provider_runtime_type_observation_is_actual_global_state() -> None:
    observation = observe_runtime_types(XAHAU_HOOK_PROVIDER_WASM)
    assert observation["schema"] == SCHEMA
    rows = {row["name"]: row for row in observation["globals"]}
    assert len(rows) == len(observation["globals"])

    # This is deliberately a runtime shape probe, not a declaration-derived
    # noun list. The three-way notes gate independently selects the names.
    classifier_rows = [row for row in rows.values() if row["own_has_instance"]]
    assert classifier_rows
    for row in classifier_rows:
        assert row["kind"] == "object"
        assert row["ordinary_object"]
        assert row["frozen"]
        assert not row["extensible"]
        assert row["has_instance_callable"]
        assert row["has_instance_writable"] is False
        assert row["has_instance_enumerable"] is False
        assert row["has_instance_configurable"] is False
        assert not row["own_prototype"]
        assert not row["constructible"]


def _run_nominal_matrix():
    host = WasmHost(
        handler=_EffectHost(),
        wasm_path=XAHAU_HOOK_PROVIDER_WASM,
        fuel=50_000_000,
    )
    host.init()
    try:
        return host.eval(_NOMINAL_MATRIX)
    finally:
        host.destroy()


def test_sealed_provider_nominal_matrix_is_exact_and_deterministically_bounded() -> (
    None
):
    first = _run_nominal_matrix()
    second = _run_nominal_matrix()
    assert first.ok, first.error
    assert second.ok, second.error
    assert first.result_value == second.result_value == "[]"
    assert first.gas_used == second.gas_used
    assert first.gas_used == _NOMINAL_MATRIX_GAS
