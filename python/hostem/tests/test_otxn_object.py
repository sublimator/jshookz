from __future__ import annotations

from pathlib import Path

import pytest

from hookz import hookapi
from hookz.runtime import HookRuntime
from hookz.handlers.otxn import otxn_slot as builtin_otxn_slot
from hookz.handlers.slot import slot_clear as builtin_slot_clear
from hostem.runner import HookRunner, _HookzProvider
from jshookz.host import WasmHost


ROOT = Path(__file__).parents[3]
HOOK = Path(__file__).parents[1] / "examples" / "accept-incoming-xah.hook.ts"

# Independently encoded with xahau-codec at vectors pin b865c6f. These are
# static oracle outputs; the provider under test never encodes its own input.
SOURCE = bytes.fromhex("B5F762798A53D543A014CAF8B297CFF8F2F937E8")
DESTINATION = bytes.fromhex("841F44689750ED44FFB6A21950C8F29403915DFD")
TRANSACTION_TYPE_ACCOUNT_SET = 3
SEQUENCE = bytes.fromhex("2400000001")
FEE = bytes.fromhex("68400000000000000A")
SIGNING_PUB_KEY = bytes.fromhex("7300")
ACCOUNT = bytes.fromhex("8114") + SOURCE
DESTINATION_FIELD = bytes.fromhex("8314") + DESTINATION
PAYMENT = bytes.fromhex(
    "120000"
    "2400000001"
    "6140000000000F4240"
    "68400000000000000A"
    "7300"
    "8114B5F762798A53D543A014CAF8B297CFF8F2F937E8"
    "8314841F44689750ED44FFB6A21950C8F29403915DFD"
)
ACCOUNT_SET = bytes.fromhex(
    "120003240000000168400000000000000A73008114B5F762798A53D543A014CAF8B297CFF8F2F937E8"
)
PAYMENT_WITHOUT_DESTINATION = bytes.fromhex(
    "120000"
    "2400000001"
    "6140000000000F4240"
    "68400000000000000A"
    "7300"
    "8114B5F762798A53D543A014CAF8B297CFF8F2F937E8"
)
IOU_PAYMENT = bytes.fromhex(
    "1200002400000001"
    "61D4838D7EA4C680000000000000000000000000005553440000000000"
    "B5F762798A53D543A014CAF8B297CFF8F2F937E8"
    "68400000000000000A7300"
    "8114B5F762798A53D543A014CAF8B297CFF8F2F937E8"
    "8314841F44689750ED44FFB6A21950C8F29403915DFD"
)
MPT_PAYMENT = bytes.fromhex(
    "1200002400000001"
    "61600000000000000001000000010101010101010101010101010101010101010101"
    "68400000000000000A7300"
    "8114B5F762798A53D543A014CAF8B297CFF8F2F937E8"
    "8314841F44689750ED44FFB6A21950C8F29403915DFD"
)


def _runner(transaction: bytes, *, hook_account: bytes = DESTINATION) -> HookRunner:
    runtime = HookRuntime()
    runtime.otxn_blob = transaction
    runtime.otxn_type = 0
    runtime.otxn_account = SOURCE
    runtime.hook_account = hook_account
    return HookRunner(runtime)


def _call_names(result) -> list[str]:
    return [call.name for call in result.call_log]


def test_otxn_object_is_one_certified_cached_local_view() -> None:
    source = """
        export function main(): never {
          const first = otxn.object();
          const second = otxn.object();
          if (first !== second) rollback("otxn cache identity changed", 1);
          if (!(first instanceof Payment) ||
              !(first instanceof Transaction) ||
              !(first instanceof STObject)) {
            rollback("specialized hierarchy mismatch", 2);
          }
          const account = first.Account;
          const destination = first.Destination;
          const amount = first.Amount;
          if (account.toHex() !== "B5F762798A53D543A014CAF8B297CFF8F2F937E8" ||
              destination.toHex() !== "841F44689750ED44FFB6A21950C8F29403915DFD" ||
              !amount.isNative() || amount.drops !== 1000000n) {
            rollback("local field mismatch", 5);
          }
          accept("one certified transaction", 0);
        }
    """

    result = _runner(PAYMENT).run_typescript(source)

    assert result.accepted, result.error
    assert result.return_msg == b"one certified transaction"
    assert _call_names(result) == [
        "otxn_slot",
        "slot_size",
        "slot_clear",
        "otxn_slot",
        "slot",
        "slot_clear",
        "accept",
    ]


def test_otxn_type_is_direct_and_equals_the_certified_object_discriminator() -> None:
    source = """
        export function main(): never {
          const transaction = otxn.object();
          if (otxn.type() !== transaction.TransactionType) {
            rollback("originating transaction type mismatch", 1);
          }
          accept("one trusted discriminator", 0);
        }
    """

    result = _runner(PAYMENT).run_typescript(source)

    assert result.accepted, result.error
    assert result.return_msg == b"one trusted discriminator"
    assert _call_names(result) == [
        "otxn_slot",
        "slot_size",
        "slot_clear",
        "otxn_slot",
        "slot",
        "slot_clear",
        "otxn_type",
        "accept",
    ]


def test_accept_require_transaction_returns_one_cached_specialized_view() -> None:
    source = """
        export function main(): never {
          const payment = accept.requireTransaction(TransactionType.Payment);
          const cached = otxn.object();
          if (payment !== cached || !(payment instanceof Payment)) {
            rollback("required transaction identity changed", 1);
          }
          if (payment.Destination.toHex() !==
              "841F44689750ED44FFB6A21950C8F29403915DFD") {
            rollback("required Payment was not statically specialized", 2);
          }
          accept("one required Payment", 0);
        }
    """

    result = _runner(PAYMENT).run_typescript(source)

    assert result.accepted, result.error
    assert result.return_msg == b"one required Payment"
    assert _call_names(result) == [
        "otxn_type",
        "otxn_slot",
        "slot_size",
        "slot_clear",
        "otxn_slot",
        "slot",
        "slot_clear",
        "accept",
    ]


def test_accept_require_transaction_exits_before_object_materialization() -> None:
    source = """
        export function main(): never {
          accept.requireTransaction(
            TransactionType.Payment,
            "Payments only",
            17,
          );
          rollback("unreachable", 18);
        }
    """

    runner = _runner(ACCOUNT_SET)
    runner.runtime.otxn_type = TRANSACTION_TYPE_ACCOUNT_SET
    result = runner.run_typescript(source)

    assert result.accepted, result.error
    assert result.return_msg == b"Payments only"
    assert result.return_code == 17
    assert _call_names(result) == ["otxn_type", "accept"]


def test_accept_require_transaction_maps_only_specialized_kinds() -> None:
    _runner(PAYMENT).run_typescript(
        "export function main(): never { "
        "const payment = accept.requireTransaction(TransactionType.Payment); "
        "void payment.Destination; accept(); }"
    )

    with pytest.raises(
        RuntimeError,
        match="Property 'Destination' does not exist on type 'Transaction'",
    ):
        HookRunner().run_typescript(
            "export function main(): never { "
            "const accountSet = "
            "accept.requireTransaction(TransactionType.AccountSet); "
            "void accountSet.Destination; accept(); }"
        )


def test_negative_otxn_type_is_an_execution_invariant_failure() -> None:
    runner = _runner(PAYMENT)
    runner.runtime.otxn_type = -5

    result = runner.run_typescript(
        "export function main(): never { void otxn.type(); accept('no'); }"
    )

    assert not result.accepted
    assert not result.rejected
    assert result.error is not None
    assert "host violated total invocation fact with -5" in str(result.error)
    assert _call_names(result) == ["otxn_type"]


def test_otxn_id_preserves_exact_hash_and_accepts_flags() -> None:
    runner = _runner(PAYMENT)
    runner.runtime.otxn_id_val = bytes.fromhex("A1" * 32)
    source = """
      export function main(): never {
        const plain: Hash256 = otxn.id();
        const flagged: Hash256 = otxn.id(1);
        if (plain.toHex() !== "A1".repeat(32) || !plain.equals(flagged)) {
          rollback("originating transaction ID mismatch", 1);
        }
        accept("originating transaction ID preserved", 0);
      }
    """

    result = runner.run_typescript(source)

    assert result.accepted, result.error
    assert result.return_msg == b"originating transaction ID preserved"
    assert _call_names(result) == ["otxn_id", "otxn_id", "accept"]
    assert result.call_log[0].args[-1] == 0
    assert result.call_log[1].args[-1] == 1


def test_negative_otxn_id_is_an_execution_invariant_failure() -> None:
    # otxn.id() is total, like otxn.type(): an executing Hook always has an
    # originating id, so a negative host status is a fault, not contract data.
    runner = _runner(PAYMENT)
    runner.runtime.handlers["otxn_id"] = lambda *_args: hookapi.INVALID_ARGUMENT

    result = runner.run_typescript(
        "export function main(): never { void otxn.id(); accept('no'); }"
    )

    assert not result.accepted
    assert not result.rejected
    assert result.error is not None
    assert "host violated total invocation fact with -7" in str(result.error)
    assert _call_names(result) == ["otxn_id"]


def test_otxn_id_rejects_malformed_positive_host_length() -> None:
    runner = _runner(PAYMENT)
    runner.runtime.handlers["otxn_id"] = lambda *_args: 31

    result = runner.run_typescript(
        "export function main(): never { void otxn.id(); accept('no'); }"
    )

    assert result.error is not None
    assert "host returned 31, expected 32" in str(result.error)
    assert _call_names(result) == ["otxn_id"]


@pytest.mark.parametrize(
    ("transaction", "code"),
    [
        (IOU_PAYMENT, 3),
        (MPT_PAYMENT, 3),
    ],
)
def test_payment_policy_rejects_missing_or_non_native_amounts(
    transaction: bytes, code: int
) -> None:
    result = _runner(transaction).run_file(HOOK)

    assert result.rejected, result.error
    assert result.return_code == code
    assert _call_names(result)[:8] == [
        "otxn_type",
        "otxn_slot",
        "slot_size",
        "slot_clear",
        "otxn_slot",
        "slot",
        "slot_clear",
        "hook_account",
    ]


def test_incomplete_payment_shaped_origin_is_internal_invariant_failure() -> None:
    result = _runner(PAYMENT_WITHOUT_DESTINATION).run_file(HOOK)

    assert not result.accepted
    assert not result.rejected
    assert result.error is not None
    assert "not a complete Transaction" in str(result.error)
    assert _call_names(result) == [
        "otxn_type",
        "otxn_slot",
        "slot_size",
        "slot_clear",
        "otxn_slot",
        "slot",
        "slot_clear",
    ]


def test_payment_policy_accepts_matching_native_payment() -> None:
    result = _runner(PAYMENT).run_file(HOOK)

    assert result.accepted, result.error
    assert result.return_code == 0
    assert result.return_msg == b"incoming native XAH accepted"
    assert _call_names(result) == [
        "otxn_type",
        "otxn_slot",
        "slot_size",
        "slot_clear",
        "otxn_slot",
        "slot",
        "slot_clear",
        "hook_account",
        "accept",
    ]


def test_payment_policy_skips_non_payment_without_materializing() -> None:
    runner = _runner(ACCOUNT_SET)
    runner.runtime.otxn_type = 3

    result = runner.run_file(HOOK)

    assert result.accepted, result.error
    assert result.return_msg == b"not an incoming Payment"
    assert _call_names(result) == ["otxn_type", "accept"]


def test_payment_policy_rejects_wrong_destination_and_self_payment() -> None:
    wrong_destination = _runner(PAYMENT, hook_account=bytes.fromhex("CC" * 20))
    wrong = wrong_destination.run_file(HOOK)
    assert wrong.rejected, wrong.error
    assert wrong.return_code == 1

    self_payment = (
        bytes.fromhex("120000")
        + SEQUENCE
        + bytes.fromhex("6140000000000F4240")
        + FEE
        + SIGNING_PUB_KEY
        + ACCOUNT
        + bytes.fromhex("8314")
        + SOURCE
    )
    self_result = _runner(self_payment, hook_account=SOURCE).run_file(HOOK)
    assert self_result.rejected, self_result.error
    assert self_result.return_code == 2


def test_malformed_trusted_transaction_is_internal_failure_after_cleanup() -> None:
    result = _runner(bytes.fromhex("1200")).run_typescript(
        "export function main(): never { otxn.object(); accept('no', 1); }"
    )

    assert not result.accepted
    assert not result.rejected
    assert result.error is not None
    assert "trusted object certification failed" in str(result.error)
    assert _call_names(result) == [
        "otxn_slot",
        "slot_size",
        "slot_clear",
        "otxn_slot",
        "slot",
        "slot_clear",
    ]


def test_measurement_failure_clears_its_returned_slot() -> None:
    runner = _runner(PAYMENT)
    runner.runtime.handlers["slot_size"] = lambda _slot: -5

    result = runner.run_typescript(
        "export function main(): never { otxn.object(); accept('no', 1); }"
    )

    assert result.error is not None
    assert "slot_size returned -5" in str(result.error)
    assert _call_names(result) == ["otxn_slot", "slot_size", "slot_clear"]
    assert runner.runtime._slot_overrides == {}


def test_short_bulk_copy_frees_guest_bytes_and_clears_copy_slot() -> None:
    runner = _runner(PAYMENT)
    runner.runtime.handlers["slot"] = lambda _output, capacity, _slot: capacity - 1

    result = runner.run_typescript(
        "export function main(): never { otxn.object(); accept('no', 1); }"
    )

    assert result.error is not None
    assert "slot returned" in str(result.error)
    assert _call_names(result) == [
        "otxn_slot",
        "slot_size",
        "slot_clear",
        "otxn_slot",
        "slot",
        "slot_clear",
    ]
    assert runner.runtime._slot_overrides == {}


def test_second_slot_failure_occurs_after_measurement_slot_cleanup() -> None:
    runner = _runner(PAYMENT)
    calls = 0

    def fail_second(requested: int) -> int:
        nonlocal calls
        calls += 1
        if calls == 1:
            return builtin_otxn_slot(runner.runtime, requested)
        return -5

    runner.runtime.handlers["otxn_slot"] = fail_second
    result = runner.run_typescript(
        "export function main(): never { otxn.object(); accept('no', 1); }"
    )

    assert result.error is not None
    assert "second otxn_slot returned -5" in str(result.error)
    assert _call_names(result) == [
        "otxn_slot",
        "slot_size",
        "slot_clear",
        "otxn_slot",
    ]
    assert runner.runtime._slot_overrides == {}


@pytest.mark.parametrize("failed_clear", [1, 2])
def test_slot_clear_failure_is_an_internal_invariant_failure(
    failed_clear: int,
) -> None:
    runner = _runner(PAYMENT)
    calls = 0

    def clear(requested: int) -> int:
        nonlocal calls
        calls += 1
        if calls == failed_clear:
            return -5
        return builtin_slot_clear(runner.runtime, requested)

    runner.runtime.handlers["slot_clear"] = clear
    result = runner.run_typescript(
        "export function main(): never { otxn.object(); accept('no', 1); }"
    )

    assert result.error is not None
    assert "slot_clear returned -5" in str(result.error)
    assert _call_names(result)[-1] == "slot_clear"
    # The deliberately broken host retained precisely the slot whose clear it
    # refused. Native HookContext destruction is the fallback owner release.
    assert len(runner.runtime._slot_overrides) == 2


def test_cache_resets_before_each_invocation_in_one_provider_instance() -> None:
    runtime = HookRuntime()
    runtime.otxn_blob = PAYMENT
    provider = _HookzProvider(runtime)
    host = WasmHost.profiled(handler=provider)
    host.init()
    try:
        bytecode = host.compile_source(
            """
            export function main() {
              throw new Error(otxn.object().get(Field.Account).toHex());
            }
            """,
            module=True,
        )
        with runtime.bind_memory(host.memory, host.store):
            first = host.run_hook_bytecode(bytecode)
            first_calls = [call.name for call in runtime.call_log]
            runtime.call_log = []
            replacement = bytes.fromhex("AA" * 20)
            runtime.otxn_blob = (
                bytes.fromhex("120000")
                + SEQUENCE
                + bytes.fromhex("6140000000000F4240")
                + FEE
                + SIGNING_PUB_KEY
                + bytes.fromhex("8114")
                + replacement
                + DESTINATION_FIELD
            )
            second = host.run_hook_bytecode(bytecode)
            second_calls = [call.name for call in runtime.call_log]
    finally:
        host.destroy()

    expected_calls = [
        "otxn_slot",
        "slot_size",
        "slot_clear",
        "otxn_slot",
        "slot",
        "slot_clear",
    ]
    assert first.error == f"Error: {SOURCE.hex().upper()}"
    assert first_calls == expected_calls
    assert first.host_work_used == len(PAYMENT) + 6
    assert second.error == f"Error: {'AA' * 20}"
    assert second_calls == expected_calls
    assert second.host_work_used == 2 * (len(PAYMENT) + 6)


def test_maximum_vl_field_stays_lazy_and_inside_host_work_envelope() -> None:
    # 918,744 is xahaud's canonical maximum VL payload. SigningPubKey is an
    # admitted required Payment field; FE D4 17 is that maximum's VL prefix.
    maximum_signing_key = bytes.fromhex("73FED417") + bytes(918_744)
    transaction = (
        bytes.fromhex("120000")
        + SEQUENCE
        + bytes.fromhex("6140000000000F4240")
        + FEE
        + maximum_signing_key
        + ACCOUNT
        + DESTINATION_FIELD
    )
    result = _runner(transaction).run_file(HOOK)

    assert result.accepted, result.error
    assert result.return_msg == b"incoming native XAH accepted"
    assert len(transaction) + 6 < 2_097_152
