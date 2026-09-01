from __future__ import annotations

from enum import IntEnum
from pathlib import Path

import pytest

from hookz.account import to_raddr
from hookz.runtime import HookRuntime
from hookz.xrpl.txn_parser import parse_object
from hostem.runner import HookRunner
from jshookz.paths import (
    XAHAU_CONSENSUS_ENTROPY_HOOK_PROVIDER_WASM,
    XAHAU_CONSENSUS_ENTROPY_RUNTIME_PROFILE_LOCK,
    XAHAU_V1_CONSENSUS_ENTROPY_HOOKS_API_DECLARATIONS,
)


HOOK = Path(__file__).parents[1] / "examples" / "lucky-six.hook.ts"

SOURCE = bytes.fromhex("B5F762798A53D543A014CAF8B297CFF8F2F937E8")
DESTINATION = bytes.fromhex("841F44689750ED44FFB6A21950C8F29403915DFD")
OTHER_ACCOUNT = bytes.fromhex("11" * 20)


class RawHookStatus(IntEnum):
    INVALID_ARGUMENT = -7
    TOO_LITTLE_ENTROPY = -48


class EntropyTierValue(IntEnum):
    CONSENSUS_FALLBACK = 1
    PARTICIPANT_ALIGNED = 2
    VALIDATOR_QUORUM = 3
    VALIDATOR_FULL = 4


class LuckySixResultCode(IntEnum):
    IGNORED = 0
    WRONG_DESTINATION = 1
    SELF_PAYMENT = 2
    INVALID_STAKE = 3
    INVALID_CONFIGURATION = 4
    ENTROPY_QUALITY = 5


class TransactionTypeValue(IntEnum):
    PAYMENT = 0
    ACCOUNT_SET = 3


DICE_SIDES = 6
WINNING_FACE = 5
DEFAULT_ENTROPY_COUNT = 19
DEFAULT_ENTROPY_DENOMINATOR = 20
ENTROPY_POLICY = "EntropyPolicy"
ENTROPY_TIER_SHIFT = 32
ENTROPY_COUNT_SHIFT = 16


def _entropy_runner(runtime: HookRuntime | None = None) -> HookRunner:
    return HookRunner(
        runtime,
        wasm_path=XAHAU_CONSENSUS_ENTROPY_HOOK_PROVIDER_WASM,
        profile_path=XAHAU_CONSENSUS_ENTROPY_RUNTIME_PROFILE_LOCK,
        declarations=XAHAU_V1_CONSENSUS_ENTROPY_HOOKS_API_DECLARATIONS,
    )


def _packed_entropy_status(
    tier: EntropyTierValue,
    count: int,
    denominator: int,
) -> int:
    return (
        (int(tier) << ENTROPY_TIER_SHIFT) | (count << ENTROPY_COUNT_SHIFT) | denominator
    )


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
IOU_PAYMENT = bytes.fromhex(
    "1200002400000001"
    "61D4838D7EA4C680000000000000000000000000005553440000000000"
    "B5F762798A53D543A014CAF8B297CFF8F2F937E8"
    "68400000000000000A7300"
    "8114B5F762798A53D543A014CAF8B297CFF8F2F937E8"
    "8314841F44689750ED44FFB6A21950C8F29403915DFD"
)
TWO_XAH_PAYMENT = PAYMENT.replace(
    bytes.fromhex("6140000000000F4240"),
    bytes.fromhex("6140000000001E8480"),
)
SELF_PAYMENT = PAYMENT.replace(
    bytes.fromhex("8114") + SOURCE,
    bytes.fromhex("8114") + DESTINATION,
)


def _runner(
    transaction: bytes = PAYMENT,
    *,
    transaction_type: int = TransactionTypeValue.PAYMENT,
    hook_account: bytes = DESTINATION,
    face: int = 0,
    entropy_tier: EntropyTierValue = EntropyTierValue.VALIDATOR_QUORUM,
    entropy_count: int = DEFAULT_ENTROPY_COUNT,
    entropy_denominator: int = DEFAULT_ENTROPY_DENOMINATOR,
    require_full_commit_reveal: bool = False,
    minimum_entropy_contributors: int | None = None,
) -> HookRunner:
    runtime = HookRuntime()
    runtime.otxn_blob = transaction
    runtime.otxn_type = transaction_type
    runtime.otxn_account = SOURCE
    runtime.hook_account = hook_account
    runtime.ledger_seq_val = 100
    runtime.handlers["entropy_cr_dice"] = lambda sides, tier: face
    runtime.handlers["entropy_cr_status"] = lambda: _packed_entropy_status(
        entropy_tier,
        entropy_count,
        entropy_denominator,
    )
    if require_full_commit_reveal or minimum_entropy_contributors is not None:
        minimum = minimum_entropy_contributors or 0
        runtime.params[ENTROPY_POLICY] = bytes(
            [int(require_full_commit_reveal)]
        ) + minimum.to_bytes(2, "little")
    return _entropy_runner(runtime)


def _calls(result, name: str):
    return [call for call in result.call_log if call.name == name]


def test_declaration_requires_an_explicit_minimum_tier() -> None:
    with pytest.raises(RuntimeError, match="Expected 2 arguments, but got 1"):
        _entropy_runner().run_typescript(
            "export function main(): never { "
            "entropy.cr.dice(6); accept('unreachable'); }"
        )


@pytest.mark.parametrize(
    ("transaction", "transaction_type", "hook_account", "accepted", "code"),
    [
        (
            ACCOUNT_SET,
            TransactionTypeValue.ACCOUNT_SET,
            DESTINATION,
            True,
            LuckySixResultCode.IGNORED,
        ),
        (
            PAYMENT,
            TransactionTypeValue.PAYMENT,
            OTHER_ACCOUNT,
            False,
            LuckySixResultCode.WRONG_DESTINATION,
        ),
        (
            SELF_PAYMENT,
            TransactionTypeValue.PAYMENT,
            DESTINATION,
            False,
            LuckySixResultCode.SELF_PAYMENT,
        ),
        (
            IOU_PAYMENT,
            TransactionTypeValue.PAYMENT,
            DESTINATION,
            False,
            LuckySixResultCode.INVALID_STAKE,
        ),
        (
            TWO_XAH_PAYMENT,
            TransactionTypeValue.PAYMENT,
            DESTINATION,
            False,
            LuckySixResultCode.INVALID_STAKE,
        ),
    ],
)
def test_policy_failures_do_not_draw_or_emit(
    transaction: bytes,
    transaction_type: int,
    hook_account: bytes,
    accepted: bool,
    code: int,
) -> None:
    result = _runner(
        transaction,
        transaction_type=transaction_type,
        hook_account=hook_account,
    ).run_file(HOOK)

    assert result.accepted is accepted, result.error
    assert result.rejected is not accepted, result.error
    assert result.return_code == code
    assert not _calls(result, "entropy_cr_dice")
    assert not _calls(result, "etxn_reserve")
    assert not _calls(result, "emit")


@pytest.mark.parametrize("face", range(5))
def test_losing_faces_accept_without_emission(face: int) -> None:
    runner = _runner(face=face)

    result = runner.run_file(HOOK)

    assert result.accepted, result.error
    assert result.return_msg == f"Thanks! You rolled {face + 1}.".encode()
    assert [(call.args, call.result) for call in _calls(result, "entropy_cr_dice")] == [
        ((DICE_SIDES, EntropyTierValue.VALIDATOR_QUORUM), face)
    ]
    assert not _calls(result, "entropy_cr_status")
    assert not _calls(result, "etxn_reserve")
    assert not _calls(result, "emit")
    assert runner.runtime.emitted_txns == []


def test_face_six_emits_one_exact_five_xah_prize_to_the_sender() -> None:
    runner = _runner(face=WINNING_FACE)

    result = runner.run_file(HOOK)

    assert result.accepted, result.error
    assert result.return_msg == b"Lucky six! Your five-XAH prize is on its way."
    assert [(call.args, call.result) for call in _calls(result, "entropy_cr_dice")] == [
        ((DICE_SIDES, EntropyTierValue.VALIDATOR_QUORUM), WINNING_FACE)
    ]
    assert not _calls(result, "entropy_cr_status")
    assert [call.args for call in _calls(result, "etxn_reserve")] == [(1,)]
    assert len(_calls(result, "emit")) == 1
    assert len(runner.runtime.emitted_txns) == 1

    parsed = parse_object(runner.runtime.emitted_txns[0], strict=True)
    assert parsed.complete and not parsed.illegal
    assert parsed.fields["TransactionType"] == "Payment"
    assert parsed.fields["Account"] == to_raddr(DESTINATION)
    assert parsed.fields["Destination"] == to_raddr(SOURCE)
    assert parsed.fields["Amount"] == "5000000"


@pytest.mark.parametrize(
    ("status", "public_name"),
    [
        (RawHookStatus.TOO_LITTLE_ENTROPY, "TOO_LITTLE_ENTROPY"),
        (RawHookStatus.INVALID_ARGUMENT, "INVALID_ARGUMENT"),
    ],
)
def test_negative_raw_statuses_remain_distinguishable_host_errors(
    status: RawHookStatus,
    public_name: str,
) -> None:
    runner = _runner()
    runner.runtime.handlers["entropy_cr_dice"] = lambda _sides, _tier: status
    source = f"""
        export function main() {{
          const outcome = entropy.cr.dice(6, EntropyTier.validatorQuorum);
          if (outcome.ok) rollback("expected host failure");
          if (outcome.error.code !== HookReturnCode.{public_name})
            rollback("host status changed");
          accept("preserved host status");
        }}
    """

    result = runner.run(source)

    assert result.accepted, result.error
    assert result.return_msg == b"preserved host status"
    assert [(call.args, call.result) for call in _calls(result, "entropy_cr_dice")] == [
        ((DICE_SIDES, EntropyTierValue.VALIDATOR_QUORUM), status)
    ]


@pytest.mark.parametrize(("sides", "tier"), [(0, 3), (6, 0), (6, 5)])
def test_lossless_raw_domain_errors_cross_the_host(sides: int, tier: int) -> None:
    runner = _runner()
    runner.runtime.handlers["entropy_cr_dice"] = lambda _sides, _tier: (
        RawHookStatus.INVALID_ARGUMENT
    )
    source = f"""
        export function main() {{
          const outcome = entropy.cr.dice({sides}, {tier});
          if (outcome.ok ||
              outcome.error.code !== HookReturnCode.INVALID_ARGUMENT)
            rollback("raw domain status changed");
          accept("raw domain status");
        }}
    """

    result = runner.run(source)

    assert result.accepted, result.error
    assert [(call.args, call.result) for call in _calls(result, "entropy_cr_dice")] == [
        ((sides, tier), RawHookStatus.INVALID_ARGUMENT)
    ]


@pytest.mark.parametrize(
    "status",
    [RawHookStatus.TOO_LITTLE_ENTROPY, RawHookStatus.INVALID_ARGUMENT],
)
def test_unavailable_entropy_rolls_back_the_original_payment(
    status: RawHookStatus,
) -> None:
    runner = _runner()
    runner.runtime.handlers["entropy_cr_dice"] = lambda _sides, _tier: status

    result = runner.run_file(HOOK)

    assert result.rejected, result.error
    assert result.return_msg == b"Required entropy is unavailable"
    assert result.return_code == status
    assert len(_calls(result, "entropy_cr_dice")) == 1
    assert not _calls(result, "etxn_reserve")
    assert not _calls(result, "emit")


def test_fallback_tier_cannot_satisfy_the_quorum_draw() -> None:
    runner = _runner()

    def fallback_only(sides: int, minimum_tier: int) -> int:
        assert (sides, minimum_tier) == (
            DICE_SIDES,
            EntropyTierValue.VALIDATOR_QUORUM,
        )
        return RawHookStatus.TOO_LITTLE_ENTROPY

    runner.runtime.handlers["entropy_cr_dice"] = fallback_only

    result = runner.run_file(HOOK)

    assert result.rejected, result.error
    assert result.return_msg == b"Required entropy is unavailable"
    assert not _calls(result, "emit")


@pytest.mark.parametrize(
    "call",
    [
        "entropy.cr.dice(-1, EntropyTier.validatorQuorum)",
        "entropy.cr.dice(1.5, EntropyTier.validatorQuorum)",
        "entropy.cr.dice(4_294_967_296, EntropyTier.validatorQuorum)",
        "entropy.cr.dice(NaN, EntropyTier.validatorQuorum)",
        "entropy.cr.dice(Infinity, EntropyTier.validatorQuorum)",
        "entropy.cr.dice('6' as any, EntropyTier.validatorQuorum)",
        "entropy.cr.dice(6, '3' as any)",
        "(entropy.cr.dice as any)(6)",
    ],
)
def test_malformed_unsigned_arguments_never_cross_the_host(call: str) -> None:
    runner = _runner()
    source = (
        "export function main(): never { "
        f"void rollback.onFail({call}, 'unexpected host failure'); "
        "accept('bad'); }"
    )

    result = runner.run_typescript(source)

    assert result.error is not None
    assert "TypeError" in str(result.error)
    assert not _calls(result, "entropy_cr_dice")


def test_runtime_accepts_lossless_uint32_and_bigint_arguments() -> None:
    runner = _runner(face=WINNING_FACE)
    source = """
        export function main(): never {
          const sides = rollback.onFail(UInt32.from(6), "bad sides");
          const outcome = entropy.cr.dice(sides as any, 3n as any);
          const face = rollback.onFail(outcome, "draw failed");
          rollback.when(face !== 5, "wrong face");
          accept("lossless arguments");
        }
    """

    result = runner.run_typescript(source)

    assert result.accepted, result.error
    assert [(call.args, call.result) for call in _calls(result, "entropy_cr_dice")] == [
        ((DICE_SIDES, EntropyTierValue.VALIDATOR_QUORUM), WINNING_FACE)
    ]


def test_status_decodes_exact_frozen_commit_reveal_metadata() -> None:
    runner = _runner()
    source = f"""
        export function main(): never {{
          const quality = rollback.onFail(
            entropy.cr.status(),
            "status unavailable",
          );
          const tier = Object.getOwnPropertyDescriptor(quality, "tier");
          if (quality.tier !== EntropyTier.validatorQuorum ||
              quality.count !== {DEFAULT_ENTROPY_COUNT} ||
              quality.denominator !== {DEFAULT_ENTROPY_DENOMINATOR} ||
              Object.isExtensible(quality) ||
              tier === undefined || tier.writable || tier.configurable) {{
            rollback("status shape changed");
          }}
          accept("status decoded");
        }}
    """

    result = runner.run_typescript(source)

    assert result.accepted, result.error
    assert [
        (call.args, call.result) for call in _calls(result, "entropy_cr_status")
    ] == [
        (
            (),
            _packed_entropy_status(
                EntropyTierValue.VALIDATOR_QUORUM,
                DEFAULT_ENTROPY_COUNT,
                DEFAULT_ENTROPY_DENOMINATOR,
            ),
        )
    ]


def test_strict_policy_requires_full_entropy_then_draws_at_full_tier() -> None:
    runner = _runner(
        face=WINNING_FACE,
        entropy_tier=EntropyTierValue.VALIDATOR_FULL,
        entropy_count=DEFAULT_ENTROPY_DENOMINATOR,
        entropy_denominator=DEFAULT_ENTROPY_DENOMINATOR,
        require_full_commit_reveal=True,
        minimum_entropy_contributors=DEFAULT_ENTROPY_COUNT,
    )

    result = runner.run_file(HOOK)

    assert result.accepted, result.error
    assert len(_calls(result, "entropy_cr_status")) == 1
    assert [(call.args, call.result) for call in _calls(result, "entropy_cr_dice")] == [
        ((DICE_SIDES, EntropyTierValue.VALIDATOR_FULL), WINNING_FACE)
    ]
    assert len(_calls(result, "emit")) == 1


@pytest.mark.parametrize(
    ("tier", "count", "denominator", "message"),
    [
        (
            EntropyTierValue.VALIDATOR_QUORUM,
            DEFAULT_ENTROPY_COUNT,
            DEFAULT_ENTROPY_DENOMINATOR,
            b"Full commit/reveal tier is required",
        ),
        (
            EntropyTierValue.VALIDATOR_FULL,
            DEFAULT_ENTROPY_COUNT,
            DEFAULT_ENTROPY_DENOMINATOR,
            b"Every entropy contributor must reveal",
        ),
    ],
)
def test_full_policy_rejects_incomplete_commit_reveal_quality(
    tier: EntropyTierValue,
    count: int,
    denominator: int,
    message: bytes,
) -> None:
    runner = _runner(
        entropy_tier=tier,
        entropy_count=count,
        entropy_denominator=denominator,
        require_full_commit_reveal=True,
    )

    result = runner.run_file(HOOK)

    assert result.rejected, result.error
    assert result.return_msg == message
    assert not _calls(result, "entropy_cr_dice")
    assert not _calls(result, "emit")


def test_minimum_contributor_policy_is_independent_of_full_tier() -> None:
    runner = _runner(
        entropy_tier=EntropyTierValue.VALIDATOR_QUORUM,
        minimum_entropy_contributors=DEFAULT_ENTROPY_COUNT,
    )

    result = runner.run_file(HOOK)

    assert result.accepted, result.error
    assert len(_calls(result, "entropy_cr_status")) == 1
    assert [(call.args, call.result) for call in _calls(result, "entropy_cr_dice")] == [
        ((DICE_SIDES, EntropyTierValue.VALIDATOR_QUORUM), 0)
    ]


def test_minimum_contributor_policy_rejects_too_few_contributors() -> None:
    runner = _runner(
        entropy_count=DEFAULT_ENTROPY_COUNT - 1,
        minimum_entropy_contributors=DEFAULT_ENTROPY_COUNT,
    )

    result = runner.run_file(HOOK)

    assert result.rejected, result.error
    assert result.return_msg == b"Minimum entropy contributor count is not met"
    assert not _calls(result, "entropy_cr_dice")


@pytest.mark.parametrize(
    "encoded",
    [b"\x02\x00\x00", b"\x00", b"\x00\x01", b"\x00\x01\x00\x00"],
)
def test_full_policy_rejects_malformed_configuration(encoded: bytes) -> None:
    runner = _runner()
    runner.runtime.params[ENTROPY_POLICY] = encoded

    result = runner.run_file(HOOK)

    assert result.rejected, result.error
    assert not _calls(result, "entropy_cr_status")
    assert not _calls(result, "entropy_cr_dice")


def test_zeroed_policy_record_is_the_explicit_default() -> None:
    runner = _runner()
    runner.runtime.params[ENTROPY_POLICY] = b"\x00\x00\x00"

    result = runner.run_file(HOOK)

    assert result.accepted, result.error
    assert not _calls(result, "entropy_cr_status")
    assert [(call.args, call.result) for call in _calls(result, "entropy_cr_dice")] == [
        ((DICE_SIDES, EntropyTierValue.VALIDATOR_QUORUM), 0)
    ]


def test_empty_policy_parameters_are_the_same_as_absent() -> None:
    runner = _runner()
    runner.runtime.params[ENTROPY_POLICY] = b""

    result = runner.run_file(HOOK)

    assert result.accepted, result.error
    assert not _calls(result, "entropy_cr_status")
    assert [(call.args, call.result) for call in _calls(result, "entropy_cr_dice")] == [
        ((DICE_SIDES, EntropyTierValue.VALIDATOR_QUORUM), 0)
    ]


def test_strict_policy_preserves_status_host_failure() -> None:
    runner = _runner(require_full_commit_reveal=True)
    runner.runtime.handlers["entropy_cr_status"] = (
        lambda: RawHookStatus.TOO_LITTLE_ENTROPY
    )

    result = runner.run_file(HOOK)

    assert result.rejected, result.error
    assert result.return_msg == b"Cannot inspect commit/reveal quality"
    assert not _calls(result, "entropy_cr_dice")
