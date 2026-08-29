from __future__ import annotations

import pytest
from hookz.runtime import HookRuntime
from hostem.runner import HookRunner


def _run(runtime: HookRuntime, source: str):
    return HookRunner(runtime).run_typescript(source)


def _call_names(result) -> list[str]:
    return [call.name for call in result.call_log]


def test_local_delete_uses_zero_length_state_set_and_removes_state() -> None:
    runtime = HookRuntime()
    runtime.state_db[b"K"] = b"value"
    rich_key = bytes.fromhex("11" * 20)
    runtime.state_db[rich_key] = b"rich"

    result = _run(
        runtime,
        """
        export function main(): never {
          rollback.onFail(state.del("K"), "delete K failed");
          rollback.onFail(state.del("MISSING"), "delete missing failed");
          rollback.onFail(
            state.del(AccountID.fromHex("11".repeat(20))),
            "delete rich key failed",
          );
          if (rollback.onFail(state.get("K")) !== undefined ||
              rollback.onFail(state.get("MISSING")) !== undefined) {
            rollback("deleted state remains", 1);
          }
          accept("local state deleted", 0);
        }
        """,
    )

    assert result.accepted, result.error
    assert b"K" not in runtime.state_db
    assert rich_key not in runtime.state_db
    assert _call_names(result) == [
        "state_set",
        "state_set",
        "state_set",
        "state",
        "state",
        "accept",
    ]
    for call in result.call_log[:3]:
        assert call.args[0:2] == (0, 0)


def test_local_delete_preserves_exact_host_failure_as_void_result() -> None:
    runtime = HookRuntime()
    runtime.handlers["state_set"] = lambda *_args: -77

    result = _run(
        runtime,
        """
        export function main(): never {
          const code = state.del("K").okOrHandle(
            (error) => Number(error.code),
          );
          if (code !== -77) rollback("delete status changed", 1);
          state.del("K").moot();
          accept("delete failure preserved", 0);
        }
        """,
    )

    assert result.accepted, result.error
    assert _call_names(result) == ["state_set", "state_set", "accept"]
    assert all(call.args[0:2] == (0, 0) for call in result.call_log[:2])


def test_foreign_accessor_reads_exact_scopes_without_provenance_bleed() -> None:
    runtime = HookRuntime()
    account_a = bytes.fromhex("11" * 20)
    account_b = bytes.fromhex("22" * 20)
    namespace_a = bytes.fromhex("AA" * 32)
    namespace_b = bytes.fromhex("BB" * 32)
    runtime._foreign_state_db[(account_a, namespace_a, b"K")] = b"first"
    runtime._foreign_state_db[(account_b, namespace_b, b"K")] = b"second"
    runtime._foreign_state_db[(account_a, namespace_a, b"EMPTY")] = b""

    result = _run(
        runtime,
        """
        function accessor(account: string, namespace: string): state.ForeignAccessor {
          return state.foreign(
            AccountID.fromHex(account),
            Hash256.fromHex(namespace),
          );
        }

        export function main(): never {
          const first = accessor("11".repeat(20), "AA".repeat(32));
          const second = accessor("22".repeat(20), "BB".repeat(32));
          const firstValue = rollback.requirePresent(first.get("K"), "first missing");
          const secondValue = rollback.requirePresent(second.get("K"), "second missing");
          const empty = rollback.requirePresent(first.get("EMPTY"), "empty missing");
          const missing = rollback.onFail(first.get("MISSING"), "missing failed");
          if (firstValue.toHex() !== "6669727374" ||
              secondValue.toHex() !== "7365636F6E64" ||
              empty.byteLength !== 0 ||
              missing !== undefined) {
            rollback("foreign state mismatch", 1);
          }
          accept("foreign scopes isolated", 0);
        }
        """,
    )

    assert result.accepted, result.error
    assert _call_names(result) == [
        "state_foreign",
        "state_foreign",
        "state_foreign",
        "state_foreign",
        "accept",
    ]
    for call, key_length in zip(result.call_log[:4], (1, 1, 5, 7)):
        assert call.args[1] == 4096
        assert call.args[3] == key_length
        assert call.args[5] == 32
        assert call.args[7] == 20


def test_foreign_accessor_is_frozen_read_only_and_receiver_checked() -> None:
    runtime = HookRuntime()
    result = _run(
        runtime,
        """
        export function main(): never {
          const foreign = state.foreign(AccountID.zero, Hash256.zero);
          const prototype = Object.getPrototypeOf(foreign);
          if (!Object.isFrozen(foreign) || !Object.isFrozen(prototype) ||
              Object.getOwnPropertyNames(foreign).length !== 0 ||
              Object.getOwnPropertyNames(prototype).join(",") !== "get" ||
              "set" in foreign || "del" in foreign || "getMany" in foreign ||
              "ForeignStateAccessor" in globalThis) {
            rollback("foreign accessor surface mismatch", 1);
          }
          let wrongReceiverRejected = false;
          try {
            const get = foreign.get;
            rollback.onFail(get("K"), "wrong receiver unexpectedly called");
          } catch (error) {
            wrongReceiverRejected = error instanceof TypeError;
          }
          if (!wrongReceiverRejected) rollback("foreign receiver forged", 2);
          accept("foreign accessor sealed", 0);
        }
        """,
    )

    assert result.accepted, result.error
    assert _call_names(result) == ["accept"]


@pytest.mark.parametrize(
    "expression",
    [
        "state.foreign({}, Hash256.zero)",
        "state.foreign(AccountID.zero, {})",
        "state.foreign(Hash256.zero, AccountID.zero)",
        "state.foreign(new Proxy({}, {}), Hash256.zero)",
    ],
)
def test_foreign_accessor_rejects_non_nominal_scope_before_host_call(
    expression: str,
) -> None:
    result = HookRunner(HookRuntime()).run(
        f"""
        export function main() {{
          let rejected = false;
          try {{ {expression}; }} catch (error) {{
            rejected = error instanceof TypeError;
          }}
          if (!rejected) rollback("foreign scope accepted", 1);
          accept("foreign scope rejected", 0);
        }}
        """
    )

    assert result.accepted, result.error
    assert _call_names(result) == ["accept"]


@pytest.mark.parametrize("raw_result", [-77, 4097])
def test_foreign_read_preserves_host_failure_and_rejects_oversized_claim(
    raw_result: int,
) -> None:
    runtime = HookRuntime()
    runtime.handlers["state_foreign"] = lambda *_args: raw_result
    result = _run(
        runtime,
        """
        export function main(): never {
          const value = state.foreign(AccountID.zero, Hash256.zero)
            .get("K")
            .okOrHandle((error) => Number(error.code));
          if (value !== -77) rollback("foreign status changed", 1);
          accept("foreign status preserved", 0);
        }
        """,
    )

    assert _call_names(result) == ["state_foreign"] + (
        ["accept"] if raw_result < 0 else []
    )
    if raw_result < 0:
        assert result.accepted, result.error
    else:
        assert result.error is not None
        assert "host returned oversized length 4097" in str(result.error)
