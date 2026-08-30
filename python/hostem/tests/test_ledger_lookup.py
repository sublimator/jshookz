from __future__ import annotations

import pytest
from hookz.handlers.slot import slot_set as builtin_slot_set
from hookz.runtime import HookRuntime
from hostem.runner import HookRunner


ACCOUNT_HEX = "B5F762798A53D543A014CAF8B297CFF8F2F937E8"
ACCOUNT_KEYLET_HEX = (
    "00612B6AC232AA4C4BE41BF49D2459FA4A0347E1B543A4C92FCEE0821C0201E2E9A8"
)

URI_TOKEN_ID_HEX = "AA" * 32
URI_TOKEN_KEYLET_HEX = "0055" + URI_TOKEN_ID_HEX
URI_TOKEN_OWNER_HEX = "D0F5430B66E06498D4CEEC816C7B3337F9982337"

# Independently serialized through the pinned xahau-codec fixture executable.
# It contains every generated AccountRoot required field and a native Balance.
ACCOUNT_ROOT = bytes.fromhex(
    "110061"
    "2200000000"
    "2400000007"
    "250000007B"
    "2D00000000"
    "550000000000000000000000000000000000000000000000000000000000000000"
    "62400000000EE6B280"
    "8114B5F762798A53D543A014CAF8B297CFF8F2F937E8"
)

PAYMENT = bytes.fromhex(
    "12000024000000016140000000000F4240"
    "68400000000000000A73008114"
    "B5F762798A53D543A014CAF8B297CFF8F2F937E8"
    "831439249EE0886DE835D4F4D47DA9D9B1D2AED83C11"
)

INCOMPLETE_ACCOUNT_ROOT = bytes.fromhex(
    "11006122000000002400000007250000007B"
    "550000000000000000000000000000000000000000000000000000000000000000"
    "62400000000EE6B2808114"
    "B5F762798A53D543A014CAF8B297CFF8F2F937E8"
)

IOU_BALANCE_ACCOUNT_ROOT = bytes.fromhex(
    "1100612200000000240000000125000000012D00000000"
    "550000000000000000000000000000000000000000000000000000000000000000"
    "62D4838D7EA4C680000000000000000000000000005553440000000000"
    "B5F762798A53D543A014CAF8B297CFF8F2F937E8"
    "8114B5F762798A53D543A014CAF8B297CFF8F2F937E8"
)

UNADMITTED_FIELD_ACCOUNT_ROOT = ACCOUNT_ROOT + bytes.fromhex(
    "831439249EE0886DE835D4F4D47DA9D9B1D2AED83C11"
)

# Pinned xahau-codec serialization of one complete URIToken ledger entry.
URI_TOKEN = bytes.fromhex(
    "110055"
    "2200000000"
    "2500000001"
    "340000000000000000"
    "550000000000000000000000000000000000000000000000000000000000000000"
    "751368747470733A2F2F6578616D706C652E636F6D"
    "8214D0F5430B66E06498D4CEEC816C7B3337F9982337"
    "8414D0F5430B66E06498D4CEEC816C7B3337F9982337"
)

# Independently serialized through the pinned protocol codec fixture.
# Every format-specific optional field is present.
COMPLETE_URI_TOKEN = bytes.fromhex(
    "110055"
    "2200000000"
    "2500000001"
    "340000000000000000"
    "550000000000000000000000000000000000000000000000000000000000000000"
    "50151111111111111111111111111111111111111111111111111111111111111111"
    "6140000000000F4240"
    "751368747470733A2F2F6578616D706C652E636F6D"
    "8214B5F762798A53D543A014CAF8B297CFF8F2F937E8"
    "8314B5F762798A53D543A014CAF8B297CFF8F2F937E8"
    "8414B5F762798A53D543A014CAF8B297CFF8F2F937E8"
)


def _runner(value: bytes | None = ACCOUNT_ROOT) -> HookRunner:
    runtime = HookRuntime()
    if value is not None:
        runtime.ledger[bytes.fromhex(ACCOUNT_KEYLET_HEX)] = value
    return HookRunner(runtime)


def _uri_runner(value: bytes | None = URI_TOKEN) -> HookRunner:
    runtime = HookRuntime()
    if value is not None:
        runtime.ledger[bytes.fromhex(URI_TOKEN_KEYLET_HEX)] = value
    return HookRunner(runtime)


def _call_names(result) -> list[str]:
    return [call.name for call in result.call_log]


def test_account_keylet_vector_and_whole_account_root_lookup() -> None:
    source = f"""
      export function main(): never {{
        const keylet = util.keylet.account(AccountID.fromHex("{ACCOUNT_HEX}"));
        if (!(keylet instanceof LedgerKeylet) ||
            keylet.byteLength !== 34 || keylet.type !== 97 ||
            keylet.toHex() !== "{ACCOUNT_KEYLET_HEX}") {{
          rollback("account keylet mismatch", 1);
        }}
        if ("prototype" in LedgerKeylet ||
            !Object.isFrozen(LedgerKeylet) ||
            !Object.isFrozen(Object.getPrototypeOf(keylet)) ||
            Object.isExtensible(keylet)) {{
          rollback("keylet nominal surface mismatch", 2);
        }}
        const copy = keylet.toBytes();
        copy[0] = 255;
        if (keylet.toHex() !== "{ACCOUNT_KEYLET_HEX}") {{
          rollback("keylet bytes were not copied", 3);
        }}

        const account = rollback.requirePresent(
          ledger.lookup(keylet),
          "AccountRoot missing",
          4,
        );
        if (!(account instanceof AccountRoot) ||
            !(account instanceof LedgerEntry) ||
            !(account instanceof STObject) ||
            account.Account.toHex() !== "{ACCOUNT_HEX}" ||
            account.Sequence.toNumber() !== 7 ||
            account.OwnerCount.toNumber() !== 0 ||
            !account.Balance.isNative() ||
            account.Balance.drops !== 250000000n) {{
          rollback("AccountRoot local view mismatch", 5);
        }}
        accept("typed AccountRoot lookup", 0);
      }}
    """

    result = _runner().run_typescript(source)

    assert result.accepted, result.error
    assert result.return_msg == b"typed AccountRoot lookup"
    assert _call_names(result) == [
        "slot_set",
        "slot_size",
        "slot_clear",
        "slot_set",
        "slot",
        "slot_clear",
        "accept",
    ]


def test_missing_account_root_is_successful_undefined() -> None:
    source = f"""
      export function main(): never {{
        const keylet = util.keylet.account(AccountID.fromHex("{ACCOUNT_HEX}"));
        const value = rollback.onFail(ledger.lookup(keylet), "lookup failed");
        if (value !== undefined) rollback("unexpected AccountRoot", 1);
        accept("AccountRoot absent", 0);
      }}
    """

    result = _runner(None).run_typescript(source)

    assert result.accepted, result.error
    assert _call_names(result) == ["slot_set", "accept"]


def test_uri_token_keylet_and_specific_lookup_are_typed() -> None:
    source = f'''
      export function main(): never {{
        const keylet = util.keylet.uriToken(
          Hash256.fromHex("{URI_TOKEN_ID_HEX}"),
        );
        if (!(keylet instanceof LedgerKeylet) || keylet.type !== 85 ||
            keylet.toHex() !== "{URI_TOKEN_KEYLET_HEX}") {{
          rollback("URI token keylet mismatch", 1);
        }}
        const token = rollback.requirePresent(
          ledger.lookup(keylet),
          "URIToken missing",
          2,
        );
        if (!(token instanceof URIToken) ||
            !(token instanceof LedgerEntry) ||
            !(token instanceof STObject) ||
            token.LedgerEntryType !== LedgerEntryType.URIToken ||
            token.Owner.toHex() !== "{URI_TOKEN_OWNER_HEX}") {{
          rollback("URIToken local view mismatch", 3);
        }}
        accept("typed URIToken lookup", 0);
      }}
    '''

    result = _uri_runner().run_typescript(source)

    assert result.accepted, result.error
    assert result.return_msg == b"typed URIToken lookup"
    assert _call_names(result) == [
        "slot_set",
        "slot_size",
        "slot_clear",
        "slot_set",
        "slot",
        "slot_clear",
        "accept",
    ]


def test_uri_token_lookup_projects_complete_canonical_format() -> None:
    source = f'''
      export function main(): never {{
        const token = rollback.requirePresent(
          ledger.lookup(util.keylet.uriToken(
            Hash256.fromHex("{URI_TOKEN_ID_HEX}"),
          )),
          "URIToken missing",
          1,
        );
        if (!(token instanceof URIToken) ||
            token.LedgerEntryType !== LedgerEntryType.URIToken ||
            !token.Flags.isZero() ||
            token.OwnerNode.toBigInt() !== 0n ||
            token.PreviousTxnLgrSeq.toNumber() !== 1 ||
            !token.PreviousTxnID.equals(Hash256.zero) ||
            token.URI.toHex() !== "68747470733A2F2F6578616D706C652E636F6D" ||
            token.Owner.toHex() !== "{ACCOUNT_HEX}" ||
            token.Issuer.toHex() !== "{ACCOUNT_HEX}" ||
            token.Destination?.toHex() !== "{ACCOUNT_HEX}" ||
            token.Digest?.toHex() !== "{"11" * 32}" ||
            token.Amount?.asNative()?.drops !== 1000000n ||
            token.LedgerIndex !== undefined || token.Remarks !== undefined) {{
          rollback("complete URIToken projection mismatch", 2);
        }}
        accept("complete URIToken projected", 0);
      }}
    '''

    result = _uri_runner(COMPLETE_URI_TOKEN).run_typescript(source)

    assert result.accepted, result.error
    assert result.return_msg == b"complete URIToken projected"
    assert _call_names(result) == [
        "slot_set",
        "slot_size",
        "slot_clear",
        "slot_set",
        "slot",
        "slot_clear",
        "accept",
    ]


def test_uri_token_lookup_rejects_wrong_complete_ledger_family() -> None:
    result = _uri_runner(ACCOUNT_ROOT).run(
        f'''
          export function main() {{
            ledger.lookup(util.keylet.uriToken(
              Hash256.fromHex("{URI_TOKEN_ID_HEX}"),
            ));
          }}
        '''
    )

    assert result.error is not None
    assert "URIToken" in str(result.error)
    assert _call_names(result) == [
        "slot_set",
        "slot_size",
        "slot_clear",
        "slot_set",
        "slot",
        "slot_clear",
    ]


def test_structural_keylet_impostor_is_rejected_before_host_work() -> None:
    result = _runner().run(
        """
          export function main() {
            ledger.lookup({
              byteLength: 34,
              type: 97,
              toBytes() { return new Uint8Array(34); },
              toHex() { return ""; },
            });
            accept("no");
          }
        """
    )

    assert not result.accepted
    assert not result.rejected
    assert result.error is not None
    assert "expected a provider LedgerKeylet" in str(result.error)
    assert _call_names(result) == []


def test_proxy_wrapped_real_keylet_is_rejected_before_host_work() -> None:
    result = _runner().run(
        f"""
          export function main() {{
            const real = util.keylet.account(AccountID.fromHex("{ACCOUNT_HEX}"));
            ledger.lookup(new Proxy(real, {{}}));
            accept("no");
          }}
        """
    )

    assert not result.accepted
    assert result.error is not None
    assert "expected a provider LedgerKeylet" in str(result.error)
    assert _call_names(result) == []


@pytest.mark.parametrize("status", [-6, -7])
def test_first_slot_set_failure_preserves_exact_host_status(status: int) -> None:
    runner = _runner()
    runner.runtime.handlers["slot_set"] = lambda *_args: status
    result = runner.run(
        f"""
          export function main() {{
            const keylet = util.keylet.account(AccountID.fromHex("{ACCOUNT_HEX}"));
            const outcome = ledger.lookup(keylet);
            if (outcome.ok || outcome.error.code !== {status})
              rollback("slot_set status was not preserved", 1);
            accept("exact host failure", 0);
          }}
        """
    )

    assert result.accepted, result.error
    assert _call_names(result) == ["slot_set", "accept"]


@pytest.mark.parametrize(
    "value",
    [
        PAYMENT,
        INCOMPLETE_ACCOUNT_ROOT,
        IOU_BALANCE_ACCOUNT_ROOT,
        UNADMITTED_FIELD_ACCOUNT_ROOT,
    ],
    ids=[
        "wrong-discriminator",
        "missing-required-field",
        "non-native-balance",
        "unadmitted-field",
    ],
)
def test_trusted_lookup_payload_must_prove_exact_account_root(value: bytes) -> None:
    runner = _runner(value)
    result = runner.run(
        f"""
          export function main() {{
            const keylet = util.keylet.account(AccountID.fromHex("{ACCOUNT_HEX}"));
            ledger.lookup(keylet);
            accept("no");
          }}
        """
    )

    assert not result.accepted
    assert not result.rejected
    assert result.error is not None
    assert "AccountRoot" in str(result.error)
    assert _call_names(result) == [
        "slot_set",
        "slot_size",
        "slot_clear",
        "slot_set",
        "slot",
        "slot_clear",
    ]
    assert not any(
        key.startswith("slot_data:") for key in runner.runtime._slot_overrides
    )


def test_slot_size_failure_clears_the_live_measurement_slot() -> None:
    runner = _runner()
    runner.runtime.handlers["slot_size"] = lambda _slot: -7

    result = runner.run(
        f"""
          export function main() {{
            ledger.lookup(util.keylet.account(AccountID.fromHex("{ACCOUNT_HEX}")));
          }}
        """
    )

    assert result.error is not None
    assert "slot_size returned -7" in str(result.error)
    assert _call_names(result) == ["slot_set", "slot_size", "slot_clear"]
    assert not any(
        key.startswith("slot_data:") for key in runner.runtime._slot_overrides
    )


def test_second_slot_set_absence_is_an_invariant_after_observed_existence() -> None:
    runner = _runner()
    calls = 0

    def slot_set(*args: int) -> int:
        nonlocal calls
        calls += 1
        if calls == 2:
            return -5
        return builtin_slot_set(runner.runtime, *args)

    runner.runtime.handlers["slot_set"] = slot_set
    result = runner.run(
        f"""
          export function main() {{
            ledger.lookup(util.keylet.account(AccountID.fromHex("{ACCOUNT_HEX}")));
          }}
        """
    )

    assert result.error is not None
    assert "second slot_set returned -5" in str(result.error)
    assert _call_names(result) == [
        "slot_set",
        "slot_size",
        "slot_clear",
        "slot_set",
    ]
    assert not any(
        key.startswith("slot_data:") for key in runner.runtime._slot_overrides
    )


def test_short_slot_copy_clears_the_live_copy_slot() -> None:
    runner = _runner()
    runner.runtime.handlers["slot"] = lambda *_args: len(ACCOUNT_ROOT) - 1
    result = runner.run(
        f"""
          export function main() {{
            ledger.lookup(util.keylet.account(AccountID.fromHex("{ACCOUNT_HEX}")));
          }}
        """
    )

    assert result.error is not None
    assert f"slot returned {len(ACCOUNT_ROOT) - 1}" in str(result.error)
    assert _call_names(result) == [
        "slot_set",
        "slot_size",
        "slot_clear",
        "slot_set",
        "slot",
        "slot_clear",
    ]
    assert not any(
        key.startswith("slot_data:") for key in runner.runtime._slot_overrides
    )
