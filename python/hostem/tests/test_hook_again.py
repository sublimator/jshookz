from __future__ import annotations

import pytest
from hookz.runtime import HookRuntime
from hostem.runner import HookRunner


def _run(status: int, source: str):
    runtime = HookRuntime()
    runtime.handlers["hook_again"] = lambda: status
    return HookRunner(runtime).run_typescript(source)


def _call_names(result) -> list[str]:
    return [call.name for call in result.call_log]


def test_hostem_models_strong_nomination_and_manual_again_delivery() -> None:
    source = """
      export function main(): never {
        const mode = hook.mode();
        if (mode === HookExecutionMode.Strong) {
          rollback.onFail(hook.again(), "first again request failed");
          const duplicate = hook.again().okOrHandle(error => Number(error.code));
          if (duplicate !== HookReturnCode.ALREADY_SET) {
            rollback("duplicate again status changed", 1);
          }
          accept("strong nominated again", 11);
        }
        if (mode === HookExecutionMode.Again) {
          const status = hook.again().okOrHandle(error => Number(error.code));
          if (status !== HookReturnCode.PREREQUISITE_NOT_MET) {
            rollback("Again invocation status changed", 2);
          }
          accept("Again invocation observed", 12);
        }
        rollback("unexpected invocation mode", 3);
      }
    """
    runner = HookRunner(HookRuntime())

    strong = runner.run_typescript(source, mode="strong")
    again = runner.run_typescript(source, mode="again")

    assert strong.accepted, strong.error
    assert strong.return_code == 11
    assert strong.execution_mode == "strong"
    assert strong.again_requested
    assert _call_names(strong) == ["hook_again", "hook_again", "accept"]
    assert again.accepted, again.error
    assert again.return_code == 12
    assert again.execution_mode == "again"
    assert not again.again_requested
    assert _call_names(again) == ["hook_again", "accept"]


@pytest.mark.parametrize("mode", ["weak", "again"])
def test_hostem_non_strong_invocation_cannot_nominate_again(mode: str) -> None:
    result = HookRunner(HookRuntime()).run_typescript(
        """
        export function main(): never {
          const status = hook.again().okOrHandle(error => Number(error.code));
          if (status !== HookReturnCode.PREREQUISITE_NOT_MET) {
            rollback("non-strong again status changed", 1);
          }
          accept("non-strong rejected", 0);
        }
        """,
        mode=mode,
    )

    assert result.accepted, result.error
    assert not result.again_requested
    assert _call_names(result) == ["hook_again", "accept"]


def test_hostem_callback_cannot_nominate_again() -> None:
    result = HookRunner(HookRuntime()).run_typescript(
        """
        export function main(): never { return accept("unused"); }
        export function callback(_info: CallbackInfo): never {
          const status = hook.again().okOrHandle(error => Number(error.code));
          if (status !== HookReturnCode.PREREQUISITE_NOT_MET) {
            rollback("callback again status changed", 1);
          }
          accept("callback rejected", 0);
        }
        """,
        mode="callback",
    )

    assert result.accepted, result.error
    assert result.execution_mode == "callback"
    assert not result.again_requested
    assert _call_names(result) == ["hook_again", "accept"]


def test_hook_again_success_is_a_void_result() -> None:
    result = _run(
        1,
        """
        export function main(): never {
          hook.again().moot();
          accept("again requested", 0);
        }
        """,
    )

    assert result.accepted, result.error
    assert result.return_msg == b"again requested"
    assert _call_names(result) == ["hook_again", "accept"]


@pytest.mark.parametrize("status", [-8, -9, -77])
def test_hook_again_preserves_exact_host_failure(status: int) -> None:
    result = _run(
        status,
        f"""
        export function main(): never {{
          const code = hook.again().okOrHandle(
            error => Number(error.code),
          );
          if (code !== {status}) rollback("again status changed", 1);
          accept("again failure preserved", 0);
        }}
        """,
    )

    assert result.accepted, result.error
    assert _call_names(result) == ["hook_again", "accept"]


@pytest.mark.parametrize("status", [0, 2, 255])
def test_hook_again_rejects_noncanonical_nonnegative_status(status: int) -> None:
    result = _run(
        status,
        """
        export function main(): never {
          hook.again().moot();
          accept("unreachable", 0);
        }
        """,
    )

    assert not result.accepted
    assert result.error is not None
    assert f"hook.again: host returned {status}, expected 1" in str(result.error)
    assert _call_names(result) == ["hook_again"]
