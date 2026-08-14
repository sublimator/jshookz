from pathlib import Path

from hostem import HookRunner


EXAMPLE = Path(__file__).parents[1] / "examples" / "hello.hook.js"
ROOT = Path(__file__).parents[3]
XAHAU_PROVIDER = ROOT / "build" / "xahau-provider" / "jshookz_provider.wasm"
XAHAU_TYPESCRIPT = (
    ROOT / "build" / "xahau-provider" / "hooks" / "xahau-accept.hook.js"
)


def test_javascript_hook_calls_real_hookz_host_and_accepts():
    runner = HookRunner()
    runner.runtime.hook_account = bytes.fromhex("11" * 20)
    runner.runtime.otxn_type = 99
    runner.runtime.ledger_seq_val = 456
    runner.runtime.ledger_last_time_val = 123

    result = runner.run_file(EXAMPLE)

    assert result.accepted, result.error
    assert result.return_code == 17
    assert result.return_msg == b"hello:99:11111111"
    assert runner.runtime.state_db[b"HELLO"] == bytes([1, 2, 3, 4])
    assert [call.name for call in result.call_log] == [
        "otxn_type",
        "hook_account",
        "state_set",
        "ledger_seq",
        "ledger_last_time",
        "ledger_last_hash",
        "accept",
    ]


def test_tagged_host_failure_can_drive_javascript_rollback():
    source = """
        export function hook(_reserved) {
          const write = state.set("K".repeat(33), new Uint8Array([1]));
          if (!write.ok) rollback("state failed", write.code);
          accept("unexpected");
        }
    """

    result = HookRunner().run(source)

    assert result.rejected, result.error
    assert result.return_msg == b"state failed"
    assert result.return_code < 0
    assert [call.name for call in result.call_log] == ["state_set", "rollback"]


def test_hook_terminal_bypasses_javascript_try_catch():
    source = """
        export function hook(_reserved) {
          try {
            accept("terminal", 23);
          } catch (error) {
            state.set("CAUGHT", new Uint8Array([1]));
            rollback("caught terminal", 24);
          }
        }
    """
    runner = HookRunner()

    result = runner.run(source)

    assert result.accepted, result.error
    assert result.return_msg == b"terminal"
    assert result.return_code == 23
    assert b"CAUGHT" not in runner.runtime.state_db
    assert [call.name for call in result.call_log] == ["accept"]


def test_javascript_reads_host_state_as_blob_then_replaces_it():
    source = """
        export function hook(_reserved) {
          const seeded = state.get("BRIDGE");
          if (!seeded.ok) rollback("state read failed", seeded.code);
          if (seeded.value === undefined) rollback("state missing", -1);
          const seededHex = seeded.value.toHex();
          trace("js-state-read", seededHex);
          if (seededHex !== "66726F6D2D63") {
            rollback(`unexpected state:${seededHex}`, -2);
          }
          const write = state.set("BRIDGE", "from-js");
          if (!write.ok) rollback("state write failed", write.code);
          trace("js-state-write", "from-js");
          accept("state bridged", 84);
        }
    """
    runner = HookRunner()
    runner.runtime.state_db[b"BRIDGE"] = b"from-c"

    result = runner.run(source)

    assert result.accepted, result.error
    assert result.return_msg == b"state bridged"
    assert result.return_code == 84
    assert runner.runtime.state_db[b"BRIDGE"] == b"from-js"
    assert [call.name for call in result.call_log] == [
        "state",
        "trace",
        "state_set",
        "trace",
        "accept",
    ]
    assert [entry.tag for entry in runner.runtime.traces] == [
        "js-state-read",
        "js-state-write",
    ]


def test_module_without_hook_export_is_refused():
    source = """
        const marker = "ordinary module initialization";
        export function other(_reserved) {
          accept(marker, 1);
        }
    """
    runner = HookRunner()

    result = runner.run(source)

    assert not result.accepted
    assert isinstance(result.error, RuntimeError)
    assert "no exported hook entry point" in str(result.error)
    assert result.call_log == []


def test_xahau_provider_runs_compiled_typescript_hook():
    assert XAHAU_PROVIDER.exists(), "build the XAHAU_HOOK_PROVIDER CMake variant"
    assert XAHAU_TYPESCRIPT.exists(), "compile tsconfig.xahau-integration.json"

    result = HookRunner(wasm_path=XAHAU_PROVIDER).run_file(XAHAU_TYPESCRIPT)

    assert result.accepted, result.error
    assert result.return_msg == b"hello from TypeScript"
    assert result.return_code == 42
    assert [call.name for call in result.call_log] == ["accept"]
