from __future__ import annotations

import pytest
from hookz.runtime import HookRuntime
from hostem.runner import HookRunner


def _run(runtime: HookRuntime, source: str):
    return HookRunner(runtime).run_typescript(source)


def _call_names(result) -> list[str]:
    return [call.name for call in result.call_log]


def test_parameter_namespaces_byte_keys_and_nonce_are_exact_v1_values() -> None:
    runtime = HookRuntime()
    runtime.tx_params["KEY"] = b"transaction"
    runtime.params["KEY"] = b"installed"
    runtime.tx_params[b"\xff"] = b"binary"
    rich_key = bytes.fromhex("11" * 20)
    runtime.tx_params[rich_key] = b"rich"

    result = _run(
        runtime,
        """
        export function main(): never {
          const transaction = rollback.requirePresent(
            rollback.onFail(otxn.param("KEY"), "otxn parameter failed"),
            "otxn parameter missing",
          );
          const installed = rollback.requirePresent(
            rollback.onFail(hook.param("KEY"), "hook parameter failed"),
            "hook parameter missing",
          );
          const binary = rollback.requirePresent(
            rollback.onFail(
              otxn.param(new Uint8Array([0xff])),
              "binary parameter failed",
            ),
            "binary parameter missing",
          );
          const nonce = rollback.onFail(ledger.nonce(), "nonce failed");
          const rich = rollback.requirePresent(
            rollback.onFail(
              otxn.param(AccountID.fromHex("11".repeat(20))),
              "rich parameter failed",
            ),
            "rich parameter missing",
          );
          if (!(transaction instanceof STBlob) ||
              !(installed instanceof STBlob) ||
              !(binary instanceof STBlob) ||
              !(rich instanceof STBlob) ||
              transaction.toHex() !== "7472616E73616374696F6E" ||
              installed.toHex() !== "696E7374616C6C6564" ||
              binary.toHex() !== "62696E617279" ||
              rich.toHex() !== "72696368" ||
              !(nonce instanceof Hash256) ||
              nonce.toHex() !== "CD".repeat(32)) {
            rollback("parameter or nonce mismatch", 1);
          }
          accept("parameter namespaces and nonce", 0);
        }
        """,
    )

    assert result.accepted, result.error
    assert result.return_msg == b"parameter namespaces and nonce"
    assert _call_names(result) == [
        "otxn_param",
        "hook_param",
        "otxn_param",
        "ledger_nonce",
        "otxn_param",
        "accept",
    ]


def test_missing_and_empty_parameters_are_the_same_typed_absence() -> None:
    runtime = HookRuntime()
    runtime.tx_params["EMPTY"] = b""
    runtime.params["EMPTY"] = b""

    result = _run(
        runtime,
        """
        export function main(): never {
          const values = [
            rollback.onFail(otxn.param("MISSING"), "missing otxn failed"),
            rollback.onFail(otxn.param("EMPTY"), "empty otxn failed"),
            rollback.onFail(hook.param("MISSING"), "missing hook failed"),
            rollback.onFail(hook.param("EMPTY"), "empty hook failed"),
          ];
          if (values.some((value) => value !== undefined)) {
            rollback("absence mismatch", 1);
          }
          accept("all absent", 0);
        }
        """,
    )

    assert result.accepted, result.error
    assert _call_names(result) == [
        "otxn_param",
        "otxn_param",
        "hook_param",
        "hook_param",
        "accept",
    ]


def test_hook_parameter_observes_the_chain_override() -> None:
    runtime = HookRuntime()
    runtime.params["KEY"] = b"installed"
    runtime._param_overrides[b"\x22" * 32] = {b"KEY": b"override"}

    result = _run(
        runtime,
        """
        export function main(): never {
          const value = rollback.requirePresent(
            rollback.onFail(hook.param("KEY"), "hook parameter failed"),
            "hook parameter missing",
          );
          if (value.toHex() !== "6F76657272696465") {
            rollback("hook parameter override mismatch", 1);
          }
          accept("override observed", 0);
        }
        """,
    )

    assert result.accepted, result.error
    assert _call_names(result) == ["hook_param", "accept"]


def test_parameter_rejects_values_outside_state_key_like_before_host_call() -> None:
    result = HookRunner(HookRuntime()).run(
        "export function main(){otxn.param({});accept('unreachable')}"
    )

    assert not result.accepted
    assert result.error is not None
    assert "otxn.param: invalid name" in str(result.error)
    assert _call_names(result) == []


def test_parameter_name_limits_remain_ordinary_host_failures() -> None:
    result = _run(
        HookRuntime(),
        """
        export function main(): never {
          const tooSmall = otxn.param("").okOrHandle(
            (error) => Number(error.code),
          );
          const tooBig = hook.param("x".repeat(33)).okOrHandle(
            (error) => Number(error.code),
          );
          if (tooSmall !== -4) rollback("empty name status", 1);
          if (tooBig !== -3) rollback("long name status", 2);
          accept("parameter limits", 0);
        }
        """,
    )

    assert result.accepted, result.error
    assert _call_names(result) == ["otxn_param", "hook_param", "accept"]


def test_parameter_host_failure_preserves_the_exact_status() -> None:
    runtime = HookRuntime()
    runtime.handlers["otxn_param"] = lambda *_args: -77

    result = _run(
        runtime,
        """
        export function main(): never {
          const value = otxn.param("KEY").okOrHandle(
            (error) => Number(error.code),
          );
          if (value !== -77) rollback("parameter status", 1);
          accept("parameter status", 0);
        }
        """,
    )

    assert result.accepted, result.error
    assert _call_names(result) == ["otxn_param", "accept"]


def test_parameter_host_cannot_claim_more_than_the_fixed_buffer() -> None:
    runtime = HookRuntime()
    runtime.handlers["hook_param"] = lambda *_args: 257

    result = _run(
        runtime,
        """
        export function main(): never {
          hook.param("KEY").okOr(undefined);
          accept("unreachable", 0);
        }
        """,
    )

    assert not result.accepted
    assert result.error is not None
    assert "hook.param: host returned oversized length 257" in str(result.error)
    assert _call_names(result) == ["hook_param"]


@pytest.mark.parametrize("raw_result", [-77, 31, 33])
def test_nonce_rejects_host_failure_and_wrong_success_lengths(
    raw_result: int,
) -> None:
    runtime = HookRuntime()
    runtime.handlers["ledger_nonce"] = lambda *_args: raw_result

    result = _run(
        runtime,
        """
        export function main(): never {
          const nonce = ledger.nonce().okOrHandle(
            (error) => Number(error.code),
          );
          if (nonce !== -77) rollback("nonce status", 1);
          accept("coded nonce failure", 0);
        }
        """,
    )

    if raw_result < 0:
        assert result.accepted, result.error
        assert result.return_msg == b"coded nonce failure"
    else:
        assert not result.accepted
        assert result.error is not None
        assert f"host returned {raw_result}, expected 32" in str(result.error)
    expected = ["ledger_nonce"]
    if raw_result < 0:
        expected.append("accept")
    assert _call_names(result) == expected
