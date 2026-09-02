"""CallbackInfo.invocationId: the raw otxn_id flag-1 read in its typed home.

Constructing the info costs no host call; the first read crosses once with
flag 1 and caches the Hash256; a refused or malformed host answer is an
execution invariant failure. otxn.id() stays flag 0 and takes no arguments.
"""

from __future__ import annotations

from hookz import hookapi
from hookz.runtime import HookRuntime
from hostem.runner import HookRunner


SOURCE = bytes.fromhex("B5F762798A53D543A014CAF8B297CFF8F2F937E8")
DESTINATION = bytes.fromhex("841F44689750ED44FFB6A21950C8F29403915DFD")
PAYMENT = bytes.fromhex(
    "120000"
    "2400000001"
    "6140000000000F4240"
    "68400000000000000A"
    "7300"
    "8114B5F762798A53D543A014CAF8B297CFF8F2F937E8"
    "8314841F44689750ED44FFB6A21950C8F29403915DFD"
)


def _runner() -> HookRunner:
    runtime = HookRuntime()
    runtime.otxn_blob = PAYMENT
    runtime.otxn_type = 0
    runtime.otxn_account = SOURCE
    runtime.hook_account = DESTINATION
    runtime.otxn_id_val = bytes.fromhex("C4" * 32)
    return HookRunner(runtime)


def _call_names(result) -> list[str]:
    return [call.name for call in result.call_log]


def _otxn_id_calls(result) -> list:
    return [call for call in result.call_log if call.name == "otxn_id"]


def test_constructing_callback_info_makes_no_host_call() -> None:
    result = _runner().run_typescript(
        """
        export function main(): never { rollback("not the callback", 1); }
        export function callback(info: CallbackInfo): never {
          void info.applied;
          void info.rawFlags;
          accept("info built without a crossing", 0);
        }
        """,
        mode="callback",
    )

    assert result.accepted, result.error
    assert _call_names(result) == ["accept"]


def test_first_read_crosses_once_with_flag_one_and_returns_hash256() -> None:
    result = _runner().run_typescript(
        """
        export function main(): never { rollback("not the callback", 1); }
        export function callback(info: CallbackInfo): never {
          const id: Hash256 = info.invocationId;
          if (!(id instanceof Hash256) || id.toHex() !== "C4".repeat(32)) {
            rollback("invocation id mismatch", 1);
          }
          accept("invocation id read", 0);
        }
        """,
        mode="callback",
    )

    assert result.accepted, result.error
    assert _call_names(result) == ["otxn_id", "accept"]
    assert _otxn_id_calls(result)[0].args[-1] == 1


def test_repeated_reads_are_cached_and_identical() -> None:
    result = _runner().run_typescript(
        """
        export function main(): never { rollback("not the callback", 1); }
        export function callback(info: CallbackInfo): never {
          const first = info.invocationId;
          const second = info.invocationId;
          if (first !== second || !first.equals(info.invocationId)) {
            rollback("invocation id was not cached", 1);
          }
          accept("invocation id cached", 0);
        }
        """,
        mode="callback",
    )

    assert result.accepted, result.error
    assert _call_names(result) == ["otxn_id", "accept"]


def test_refused_host_answer_is_an_execution_invariant_failure() -> None:
    runner = _runner()
    runner.runtime.handlers["otxn_id"] = lambda *_args: hookapi.INVALID_ARGUMENT

    result = runner.run_typescript(
        """
        export function main(): never { rollback("not the callback", 1); }
        export function callback(info: CallbackInfo): never {
          void info.invocationId;
          accept("unreachable", 0);
        }
        """,
        mode="callback",
    )

    assert not result.accepted
    assert not result.rejected
    assert result.error is not None
    assert "invocationId: host violated total invocation fact with -7" in str(
        result.error
    )
    assert _call_names(result) == ["otxn_id"]


def test_malformed_host_length_is_an_execution_invariant_failure() -> None:
    runner = _runner()
    runner.runtime.handlers["otxn_id"] = lambda *_args: 31

    result = runner.run_typescript(
        """
        export function main(): never { rollback("not the callback", 1); }
        export function callback(info: CallbackInfo): never {
          void info.invocationId;
          accept("unreachable", 0);
        }
        """,
        mode="callback",
    )

    assert result.error is not None
    assert "invocationId: host returned 31, expected 32" in str(result.error)
    assert _call_names(result) == ["otxn_id"]


def test_otxn_id_stays_flag_zero_and_rejects_arguments() -> None:
    flagless = _runner().run_typescript(
        "export function main(): never { void otxn.id(); accept('flag zero', 0); }"
    )
    assert flagless.accepted, flagless.error
    assert _otxn_id_calls(flagless)[0].args[-1] == 0

    # Checked JavaScript bypasses the TypeScript arity check; the runtime
    # must still refuse the old flag word.
    with_argument = _runner().run(
        "export function main() { otxn.id(1); accept('unreachable', 0); }"
    )
    assert with_argument.error is not None
    assert "otxn.id: takes no arguments" in str(with_argument.error)
    assert _call_names(with_argument) == []
