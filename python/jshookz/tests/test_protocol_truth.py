"""Protocol-value and runtime-shape gates for the exact v1 surface."""

import json

from jshookz.host import WasmHost


class _EffectHost:
    def state_set(self, *_args):
        return 0

    def rollback(self, *_args):
        return 0


IOU_AMOUNT_OBJECTS = {
    "positive": (
        "61D4838D7EA4C680000000000000000000000000005553440000000000"
        "B5F762798A53D543A014CAF8B297CFF8F2F937E8"
        "8114B5F762798A53D543A014CAF8B297CFF8F2F937E8"
    ),
    "negative": (
        "6194838D7EA4C680000000000000000000000000005553440000000000"
        "B5F762798A53D543A014CAF8B297CFF8F2F937E8"
        "8114B5F762798A53D543A014CAF8B297CFF8F2F937E8"
    ),
    "zero": (
        "6180000000000000000000000000000000000000005553440000000000"
        "B5F762798A53D543A014CAF8B297CFF8F2F937E8"
        "8114B5F762798A53D543A014CAF8B297CFF8F2F937E8"
    ),
}


def test_legacy_rich_roots_are_non_constructible_factory_objects():
    host = WasmHost.profiled()
    host.init()
    try:
        result = host.eval(
            "JSON.stringify([STBlob, Hash256, AccountID].map(root => {"
            "  let constructError = false;"
            "  let instanceError = false;"
            "  try { new root(); } catch (error) { constructError = error instanceof TypeError; }"
            "  try { void ({} instanceof root); } catch (error) { instanceError = error instanceof TypeError; }"
            "  return [typeof root, 'prototype' in root, constructError, instanceError];"
            "}))"
        )
    finally:
        host.destroy()

    assert result.ok, result.error
    assert json.loads(result.result_value) == [
        ["object", False, True, True],
        ["object", False, True, True],
        ["object", False, True, True],
    ]


def test_selected_value_types_have_no_global_factory_or_constructor():
    host = WasmHost.profiled()
    host.init()
    try:
        result = host.eval(
            "JSON.stringify(['XFLDecimal', 'Amount', 'PathSet'].map(name => {"
            "  const root = globalThis[name];"
            "  let constructError = false;"
            "  let instanceError = false;"
            "  try { new root(); } catch (error) { constructError = error instanceof TypeError; }"
            "  try { void ({} instanceof root); } catch (error) { instanceError = error instanceof TypeError; }"
            "  return [name, !Object.hasOwn(globalThis, name), root === undefined, constructError, instanceError];"
            "}))"
        )
    finally:
        host.destroy()

    assert result.ok, result.error
    assert json.loads(result.result_value) == [
        ["XFLDecimal", True, True, True, True],
        ["Amount", True, True, True, True],
        ["PathSet", True, True, True, True],
    ]


def test_xfl_decimal_scalar_predicates_use_provider_minted_iou_amounts():
    vectors = json.dumps(IOU_AMOUNT_OBJECTS)
    host = WasmHost.profiled()
    host.init()
    try:
        result = host.eval(
            "JSON.stringify((() => {"
            f"  const encoded = {vectors};"
            "  const amounts = Object.fromEntries(Object.entries(encoded).map("
            "    ([name, hex]) => [name, util.decodeObject(STBlob.fromHex(hex)).Amount]));"
            "  const values = Object.fromEntries(Object.entries(amounts).map("
            "    ([name, amount]) => [name, amount.value]));"
            "  const prototype = Object.getPrototypeOf(values.positive);"
            "  return {"
            "    amountKinds: Object.values(amounts).map(amount => amount.kind),"
            "    positive: [values.positive.isNegative(), values.positive.isZero()],"
            "    negative: [values.negative.isNegative(), values.negative.isZero()],"
            "    zero: [values.zero.isNegative(), values.zero.isZero()],"
            "    samePrototype: Object.values(values).every("
            "      value => Object.getPrototypeOf(value) === prototype),"
            "    prototypeOwn: Object.getOwnPropertyNames(prototype).sort(),"
            "    noRawWordMembers: Object.values(values).every(value =>"
            "      !('raw' in value) && !('mantissa' in value) &&"
            "      !('exponent' in value) && !('fromRaw' in value))"
            "  };"
            "})())"
        )
    finally:
        host.destroy()

    assert result.ok, result.error
    assert json.loads(result.result_value) == {
        "amountKinds": ["iou", "iou", "iou"],
        "positive": [False, False],
        "negative": [True, False],
        "zero": [False, True],
        "samePrototype": True,
        "prototypeOwn": ["isNegative", "isZero"],
        "noRawWordMembers": True,
    }


def test_account_id_constants_have_value_semantics():
    host = WasmHost.profiled()
    host.init()
    try:
        result = host.eval(
            "JSON.stringify(["
            "  AccountID.from(new Uint8Array(20)).equals(AccountID.zero),"
            "  AccountID.zero.isZero(),"
            "  AccountID.one.isZero(),"
            "  AccountID.one.equals(AccountID.fromHex('00'.repeat(19) + '01'))"
            "])"
        )
    finally:
        host.destroy()

    assert result.ok, result.error
    assert json.loads(result.result_value) == [True, True, False, True]


def test_accept_unless_present_refuses_void_effect_results_at_runtime():
    host = WasmHost.profiled(handler=_EffectHost())
    host.init()
    try:
        result = host.eval(
            "JSON.stringify((() => {"
            "  try { accept.unlessPresent(state.set('K', [1]), 'skip'); }"
            "  catch (error) { return [error instanceof TypeError, String(error)]; }"
            "  return [false, 'accepted'];"
            "})())"
        )
    finally:
        host.destroy()

    assert result.ok, result.error
    caught, message = json.loads(result.result_value)
    assert caught
    assert "void-effect Result" in message


def test_rollback_require_present_refuses_void_effect_results_at_runtime():
    host = WasmHost.profiled(handler=_EffectHost())
    host.init()
    try:
        result = host.eval(
            "JSON.stringify((() => {"
            "  try { rollback.requirePresent(state.set('K', [1]), 'required'); }"
            "  catch (error) { return [error instanceof TypeError, String(error)]; }"
            "  return [false, 'accepted'];"
            "})())"
        )
    finally:
        host.destroy()

    assert result.ok, result.error
    caught, message = json.loads(result.result_value)
    assert caught
    assert "void-effect Result" in message


def test_rollback_require_present_preserves_a_falsy_result():
    handler = _EffectHost()
    host = WasmHost.profiled(handler=handler)
    host.init()
    try:
        result = host.eval(
            "JSON.stringify(rollback.requirePresent(UInt64.zero.toNumber(), 'required'))"
        )
    finally:
        host.destroy()

    assert result.ok, result.error
    assert json.loads(result.result_value) == 0
