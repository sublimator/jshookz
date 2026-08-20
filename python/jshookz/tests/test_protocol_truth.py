"""Protocol-value and runtime-shape gates for the exact v1 surface."""

import json

from jshookz.host import WasmHost


class _EffectHost:
    def state_set(self, *_args):
        return 0

    def rollback(self, *_args):
        return 0


def test_rich_roots_are_non_constructible_factory_objects():
    host = WasmHost.profiled()
    host.init()
    try:
        result = host.eval(
            "JSON.stringify([STBlob, Hash256, AccountID, XFLDecimal].map(root => {"
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
        ["object", False, True, True],
    ]


def test_xfl_accessors_match_official_hook_float_vectors():
    host = WasmHost.profiled()
    host.init()
    try:
        result = host.eval(
            "JSON.stringify((() => {"
            "  const positive = XFLDecimal.fromRaw(6089866696204910592n);"
            "  const negative = XFLDecimal.fromRaw(1478180677777522688n);"
            "  const hookApiVector = XFLDecimal.fromRaw(6270245249190730432n);"
            "  const hookApiNegativeVector = XFLDecimal.fromRaw(1658559230763342528n);"
            "  const zero = XFLDecimal.fromRaw(0n);"
            "  let invalid = false;"
            "  try { XFLDecimal.fromRaw(-1n); } catch (error) { invalid = error instanceof RangeError; }"
            "  let oversized = false;"
            "  try { XFLDecimal.fromRaw(1n << 64n); } catch (error) { oversized = error instanceof RangeError; }"
            "  return {"
            "    positive: [positive.mantissa().toString(), positive.exponent(), positive.isNegative()],"
            "    negative: [negative.mantissa().toString(), negative.exponent(), negative.isNegative()],"
            "    hookApiVector: [hookApiVector.mantissa().toString(), hookApiVector.exponent(), hookApiVector.isNegative()],"
            "    hookApiNegativeVector: [hookApiNegativeVector.mantissa().toString(), hookApiNegativeVector.exponent(), hookApiNegativeVector.isNegative()],"
            "    mantissaType: typeof positive.mantissa(),"
            "    zero: [zero.mantissa().toString(), zero.exponent(), zero.isNegative()],"
            "    invalid, oversized"
            "  };"
            "})())"
        )
    finally:
        host.destroy()

    assert result.ok, result.error
    assert json.loads(result.result_value) == {
        "positive": ["1000000000000000", -15, False],
        "negative": ["1000000000000000", -15, True],
        "hookApiVector": ["1234567891000000", -5, False],
        "hookApiNegativeVector": ["1234567891000000", -5, True],
        "mantissaType": "bigint",
        "zero": ["0", 0, False],
        "invalid": True,
        "oversized": True,
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


def test_accept_unless_refuses_void_effect_results_at_runtime():
    host = WasmHost.profiled(handler=_EffectHost())
    host.init()
    try:
        result = host.eval(
            "JSON.stringify((() => {"
            "  try { accept.unless(state.set('K', [1]), 'skip'); }"
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


def test_rollback_require_refuses_void_effect_results_at_runtime():
    host = WasmHost.profiled(handler=_EffectHost())
    host.init()
    try:
        result = host.eval(
            "JSON.stringify((() => {"
            "  try { rollback.require(state.set('K', [1]), 'required'); }"
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


def test_rollback_require_preserves_a_falsy_present_result():
    handler = _EffectHost()
    host = WasmHost.profiled(handler=handler)
    host.init()
    try:
        result = host.eval(
            "JSON.stringify(rollback.require(UInt64.zero.toNumber(), 'required'))"
        )
    finally:
        host.destroy()

    assert result.ok, result.error
    assert json.loads(result.result_value) == 0
