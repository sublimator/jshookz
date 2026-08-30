from __future__ import annotations

import pytest
from hookz.runtime import HookRuntime
from hostem.runner import HookRunner


def _run(runtime: HookRuntime, source: str):
    return HookRunner(runtime).run_typescript(source)


def _call_names(result) -> list[str]:
    return [call.name for call in result.call_log]


_EXPECT_SCHEMA_FAILURE = """
function expectSchemaFailure<T>(
  result: SchemaReadResult<T>,
): HostError | ParseError {
  if (result.ok) {
    rollback("expected schema-read failure", 1);
  } else {
    return result.error;
  }
}
"""


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
              Object.getOwnPropertyNames(prototype).join(",") !== "get,set,del" ||
              !("set" in foreign) || !("del" in foreign) ||
              "getMany" in foreign ||
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


def test_foreign_accessor_writes_and_deletes_its_captured_scope() -> None:
    runtime = HookRuntime()
    account = bytes.fromhex("11" * 20)
    namespace = bytes.fromhex("AA" * 32)
    runtime._foreign_state_db[(account, namespace, b"DELETE")] = b"old"

    result = _run(
        runtime,
        """
        export function main(): never {
          const foreign = state.foreign(
            AccountID.fromHex("11".repeat(20)),
            Hash256.fromHex("AA".repeat(32)),
          );
          rollback.onFail(foreign.set("K", "value"), "set failed");
          rollback.onFail(foreign.del("DELETE"), "delete failed");
          const value = rollback.requirePresent(foreign.get("K"), "missing");
          const deleted = rollback.onFail(foreign.get("DELETE"), "read failed");
          if (value.toHex() !== "76616C7565" || deleted !== undefined) {
            rollback("foreign mutation mismatch", 1);
          }
          accept("foreign mutation exact", 0);
        }
        """,
    )

    assert result.accepted, result.error
    assert runtime._foreign_state_db[(account, namespace, b"K")] == b"value"
    assert (account, namespace, b"DELETE") not in runtime._foreign_state_db
    assert _call_names(result) == [
        "state_foreign_set",
        "state_foreign_set",
        "state_foreign",
        "state_foreign",
        "accept",
    ]
    assert result.call_log[0].args[1::2] == (5, 1, 32, 20)
    assert result.call_log[1].args[0:2] == (0, 0)
    assert result.call_log[1].args[3::2] == (6, 32, 20)


def test_schema_sized_local_read_preserves_parse_absence_and_host_channels() -> None:
    runtime = HookRuntime()
    runtime.state_db[b"K"] = bytes.fromhex("78563412")
    runtime.state_db[b"SHORT"] = bytes.fromhex("010203")
    runtime.state_db[b"LONG"] = bytes.fromhex("0102030405")
    runtime.state_db[b"MALFORMED_XFL"] = bytes.fromhex("0100000000000000")

    result = _run(
        runtime,
        _EXPECT_SCHEMA_FAILURE
        + """
        export function main(): never {
          const schema = cell("U32", record.u32le());
          const value = rollback.onFail(
            state.get("K", schema),
            "schema-sized local state read failed",
          );
          const short = expectSchemaFailure(state.get("SHORT", schema));
          const long = expectSchemaFailure(state.get("LONG", schema));
          const missing = rollback.onFail(
            state.get("MISSING", schema),
            "schema-sized missing-state read failed",
          );
          const untypedLong = rollback.requirePresent(
            state.get("LONG"),
            "untyped long missing",
          );
          const xflSchema = cell("PersistedXFL", record.xflle());
          const malformed = expectSchemaFailure(
            state.get("MALFORMED_XFL", xflSchema),
          );
          if (value !== 0x12345678 ||
              short.domain !== "parse" || short.issue !== "wrong-length" ||
              short.expectedLength !== 4 || short.actualLength !== 3 ||
              long.domain !== "host" ||
              long.code !== HookReturnCode.TOO_SMALL ||
              missing !== undefined ||
              untypedLong.byteLength !== 5 ||
              malformed.domain !== "parse" ||
              malformed.issue !== "invalid-value") {
            rollback("schema-sized local state mismatch", 1);
          }
          accept("schema-sized local state", 0);
        }
        """,
    )

    assert result.accepted, result.error
    assert _call_names(result) == ["state"] * 6 + ["accept"]
    assert [call.args[1] for call in result.call_log[:6]] == [
        4,
        4,
        4,
        4,
        4096,
        8,
    ]


def test_schema_sized_foreign_read_uses_exact_capacity() -> None:
    runtime = HookRuntime()
    scope = (bytes(20), bytes(32))
    runtime._foreign_state_db[(*scope, b"K")] = bytes.fromhex("3412")
    runtime._foreign_state_db[(*scope, b"LONG")] = bytes.fromhex("010203")
    runtime._foreign_state_db[(*scope, b"MALFORMED_XFL")] = bytes.fromhex(
        "0100000000000000"
    )

    result = _run(
        runtime,
        _EXPECT_SCHEMA_FAILURE
        + """
        export function main(): never {
          const schema = cell("U16", record.u16le());
          const foreign = state.foreign(AccountID.zero, Hash256.zero);
          const value = rollback.onFail(
            foreign.get("K", schema),
            "schema-sized foreign state read failed",
          );
          const long = expectSchemaFailure(foreign.get("LONG", schema));
          const xflSchema = cell("PersistedXFL", record.xflle());
          const malformed = expectSchemaFailure(
            foreign.get("MALFORMED_XFL", xflSchema),
          );
          if (value !== 0x1234 ||
              long.domain !== "host" ||
              long.code !== HookReturnCode.TOO_SMALL ||
              malformed.domain !== "parse" ||
              malformed.issue !== "invalid-value") {
            rollback("schema-sized foreign state mismatch", 1);
          }
          accept("schema-sized foreign state", 0);
        }
        """,
    )

    assert result.accepted, result.error
    assert _call_names(result) == [
        "state_foreign",
        "state_foreign",
        "state_foreign",
        "accept",
    ]
    assert [call.args[1] for call in result.call_log[:3]] == [2, 2, 8]


def test_schema_sized_reads_preserve_exact_negative_host_status() -> None:
    runtime = HookRuntime()
    runtime.handlers["state"] = lambda *_args: -77
    runtime.handlers["state_foreign"] = lambda *_args: -77

    result = _run(
        runtime,
        _EXPECT_SCHEMA_FAILURE
        + """
        export function main(): never {
          const schema = cell("PersistedXFL", record.xflle());
          const local = expectSchemaFailure(state.get("K", schema));
          const foreign = expectSchemaFailure(
            state.foreign(AccountID.zero, Hash256.zero).get("K", schema),
          );
          if (local.domain !== "host" || Number(local.code) !== -77 ||
              foreign.domain !== "host" || Number(foreign.code) !== -77) {
            rollback("typed host status changed", 1);
          }
          accept("typed host status preserved", 0);
        }
        """,
    )

    assert result.accepted, result.error
    assert _call_names(result) == ["state", "state_foreign", "accept"]
    assert [call.args[1] for call in result.call_log[:2]] == [8, 8]


def test_foreign_mutation_preserves_exact_host_failure() -> None:
    runtime = HookRuntime()
    runtime.handlers["state_foreign_set"] = lambda *_args: -77

    result = _run(
        runtime,
        """
        export function main(): never {
          const foreign = state.foreign(AccountID.zero, Hash256.zero);
          const setCode = foreign.set("K", "V").okOrHandle(
            (error) => Number(error.code),
          );
          const delCode = foreign.del("K").okOrHandle(
            (error) => Number(error.code),
          );
          if (setCode !== -77 || delCode !== -77) {
            rollback("foreign status changed", 1);
          }
          accept("foreign failure preserved", 0);
        }
        """,
    )

    assert result.accepted, result.error
    assert _call_names(result) == [
        "state_foreign_set",
        "state_foreign_set",
        "accept",
    ]


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
