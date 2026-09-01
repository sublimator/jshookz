from __future__ import annotations

from hookz.runtime import HookRuntime
from hostem.runner import HookRunner


_ACCOUNT = bytes.fromhex("11" * 20)
_NAMESPACE = bytes.fromhex("AA" * 32)


def _run(runtime: HookRuntime, body: str):
    return HookRunner(runtime).run_typescript(
        f"""
        const foreign = state.foreign(
          AccountID.fromHex("11".repeat(20)),
          Hash256.fromHex("AA".repeat(32)),
        );

        export function main(): never {{
          {body}
        }}
        """
    )


def _seeded_runtime() -> HookRuntime:
    runtime = HookRuntime()
    runtime.state_db.update(
        {
            b"LOCAL-KEEP": b"before",
            b"LOCAL-DELETE": b"delete-me",
        }
    )
    runtime._foreign_state_db.update(
        {
            (_ACCOUNT, _NAMESPACE, b"FOREIGN-KEEP"): b"before",
            (_ACCOUNT, _NAMESPACE, b"FOREIGN-DELETE"): b"delete-me",
        }
    )
    return runtime


def test_accept_commits_local_and_foreign_state_transactions() -> None:
    runtime = _seeded_runtime()

    result = _run(
        runtime,
        """
        rollback.onFail(state.set("LOCAL-KEEP", "after"));
        rollback.onFail(state.del("LOCAL-DELETE"));
        rollback.onFail(foreign.set("FOREIGN-KEEP", "after"));
        rollback.onFail(foreign.del("FOREIGN-DELETE"));

        const local = rollback.requirePresent(state.get("LOCAL-KEEP"), "missing");
        const remote = rollback.requirePresent(
          foreign.get("FOREIGN-KEEP"),
          "missing",
        );
        if (local.toHex() !== "6166746572" || remote.toHex() !== "6166746572") {
          rollback("writes were not visible within their invocation");
        }
        accept("commit");
        """,
    )

    assert result.accepted, result.error
    assert runtime.state_db == {b"LOCAL-KEEP": b"after"}
    assert runtime._foreign_state_db == {
        (_ACCOUNT, _NAMESPACE, b"FOREIGN-KEEP"): b"after"
    }


def test_rollback_restores_state_but_preserves_attempt_evidence() -> None:
    runtime = _seeded_runtime()
    local_before = dict(runtime.state_db)
    foreign_before = dict(runtime._foreign_state_db)

    result = _run(
        runtime,
        """
        rollback.onFail(state.set("LOCAL-KEEP", "after"));
        rollback.onFail(state.del("LOCAL-DELETE"));
        rollback.onFail(foreign.set("FOREIGN-KEEP", "after"));
        rollback.onFail(foreign.del("FOREIGN-DELETE"));
        rollback("abort transaction");
        """,
    )

    assert result.rejected, result.error
    assert runtime.state_db == local_before
    assert runtime._foreign_state_db == foreign_before
    assert [
        (write.scope, write.key, write.value) for write in result.state_writes
    ] == [
        ("local", b"LOCAL-KEEP", b"after"),
        ("local", b"LOCAL-DELETE", None),
        ("foreign", b"FOREIGN-KEEP", b"after"),
        ("foreign", b"FOREIGN-DELETE", None),
    ]


def test_runtime_failure_restores_state_but_preserves_attempt_evidence() -> None:
    runtime = _seeded_runtime()
    local_before = dict(runtime.state_db)
    foreign_before = dict(runtime._foreign_state_db)

    result = _run(
        runtime,
        """
        rollback.onFail(state.set("LOCAL-NEW", "new"));
        rollback.onFail(state.del("LOCAL-DELETE"));
        rollback.onFail(foreign.set("FOREIGN-NEW", "new"));
        rollback.onFail(foreign.del("FOREIGN-DELETE"));
        throw new Error("runtime failure");
        """,
    )

    assert not result.accepted
    assert not result.rejected
    assert result.error is not None
    assert runtime.state_db == local_before
    assert runtime._foreign_state_db == foreign_before
    assert [
        (write.scope, write.key, write.value) for write in result.state_writes
    ] == [
        ("local", b"LOCAL-NEW", b"new"),
        ("local", b"LOCAL-DELETE", None),
        ("foreign", b"FOREIGN-NEW", b"new"),
        ("foreign", b"FOREIGN-DELETE", None),
    ]


def _run_emission(runtime: HookRuntime, terminal: str):
    return HookRunner(runtime).run_typescript(
        f"""
        export function main(): never {{
          rollback.onFail(emit.reserve(1), "cannot reserve");
          const built = rollback.onFail(
            emit.build.payment({{
              Destination: AccountID.fromHex("11".repeat(20)),
              Amount: Amount.drops(1n),
            }}),
            "cannot build",
          );
          rollback.onFail(emit.tx(built), "cannot emit");
          {terminal}
        }}
        """
    )


def test_only_accepted_emissions_join_durable_history() -> None:
    runtime = HookRuntime()

    accepted = _run_emission(runtime, 'accept("commit");')

    assert accepted.accepted, accepted.error
    assert len(accepted.emitted_txns) == 1
    assert accepted.attempted_emissions == []
    assert runtime.emitted_txns == accepted.emitted_txns

    accepted_history = list(runtime.emitted_txns)
    rejected = _run_emission(runtime, 'rollback("abort");')

    assert rejected.rejected, rejected.error
    assert rejected.emitted_txns == []
    assert len(rejected.attempted_emissions) == 1
    assert runtime.attempted_emissions == rejected.attempted_emissions
    assert runtime.emitted_txns == accepted_history
