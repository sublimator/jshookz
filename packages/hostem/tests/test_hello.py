from pathlib import Path

from hostem import HookRunner


EXAMPLE = Path(__file__).parents[1] / "examples" / "hello.hook.js"
ROOT = Path(__file__).parents[3]
XAHAU_PROVIDER = ROOT / "build" / "xahau-provider" / "jshookz_provider.wasm"
XAHAU_TYPESCRIPT = (
    ROOT / "packages" / "hostem" / "examples" / "xahau-accept.hook.ts"
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
        export function main(_reserved) {
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


def test_rollback_on_fail_preserves_absence_and_exact_failure_code():
    source = """
        export function main(_reserved) {
          const missing = rollback.onFail(state.get("MISSING"));
          if (missing !== undefined) rollback("state unexpectedly present", -1);
          rollback.onFail(
            state.set("K".repeat(33), new Uint8Array([1]))
          );
          accept("unexpected");
        }
    """

    result = HookRunner().run(source)

    assert result.rejected, result.error
    assert result.return_code < 0
    assert result.return_msg == f"host operation failed: {result.return_code}".encode()
    assert [call.name for call in result.call_log] == [
        "state",
        "state_set",
        "rollback",
    ]


def test_rollback_on_fail_accepts_context_without_losing_status_code():
    source = """
        export function main(_reserved) {
          rollback.onFail(
            state.set("K".repeat(33), new Uint8Array([1])),
            "state write failed"
          );
          accept("unexpected");
        }
    """

    result = HookRunner().run(source)

    assert result.rejected, result.error
    assert result.return_code < 0
    assert result.return_msg == b"state write failed"
    assert [call.name for call in result.call_log] == ["state_set", "rollback"]


def test_rollback_on_fail_applies_policy_to_an_uncoded_result():
    source = """
        export function main(_reserved) {
          rollback.onFail(
            { ok: false, issue: "wrong-length" },
            "invalid configuration",
            137
          );
          accept("unexpected");
        }
    """

    result = HookRunner().run(source)

    assert result.rejected, result.error
    assert result.return_code == 137
    assert result.return_msg == b"invalid configuration"
    assert [call.name for call in result.call_log] == ["rollback"]


def test_rollback_require_collapses_host_failure_and_absence():
    source = """
        export function main(_reserved) {
          const mode = otxn.type();
          if (!mode.ok) rollback("unexpected type failure", -1);
          if (mode.value === 1) {
            rollback.require(
              state.set("K".repeat(33), new Uint8Array([1])),
              "required operation failed",
              71
            );
          }
          const value = rollback.require(
            state.get("MISSING"),
            "required value missing",
            72
          );
          accept(`required:${value.byteLength}`, 73);
        }
    """

    failed_runner = HookRunner()
    failed_runner.runtime.otxn_type = 1
    failed = failed_runner.run(source)
    assert failed.rejected, failed.error
    assert failed.return_code == 71
    assert failed.return_msg == b"required operation failed"
    assert [call.name for call in failed.call_log] == [
        "otxn_type",
        "state_set",
        "rollback",
    ]

    missing_runner = HookRunner()
    missing_runner.runtime.otxn_type = 0
    missing = missing_runner.run(source)
    assert missing.rejected, missing.error
    assert missing.return_code == 72
    assert missing.return_msg == b"required value missing"
    assert [call.name for call in missing.call_log] == [
        "otxn_type",
        "state",
        "rollback",
    ]

    present_runner = HookRunner()
    present_runner.runtime.otxn_type = 0
    present_runner.runtime.state_db[b"MISSING"] = b"\x01"
    present = present_runner.run(source)
    assert present.accepted, present.error
    assert present.return_code == 73
    assert present.return_msg == b"required:1"
    assert [call.name for call in present.call_log] == [
        "otxn_type",
        "state",
        "accept",
    ]


def test_rollback_on_any_fail_returns_every_value_and_accepts_empty_input():
    source = """
        export function main(_reserved) {
          const empty = rollback.onAnyFail([]);
          const values = rollback.onAnyFail([
            { ok: true, value: "first" },
            { ok: true, value: undefined },
            { ok: true, value: "third" }
          ]);
          if (empty.length !== 0 || values.length !== 3 ||
              values[0] !== "first" || values[1] !== undefined ||
              values[2] !== "third") {
            rollback("unexpected batch values", 80);
          }
          accept("all succeeded", 81);
        }
    """

    result = HookRunner().run(source)

    assert result.accepted, result.error
    assert result.return_code == 81
    assert result.return_msg == b"all succeeded"
    assert [call.name for call in result.call_log] == ["accept"]


def test_rollback_on_any_fail_uses_first_failure_in_input_order():
    source = """
        export function main(_reserved) {
          rollback.onAnyFail([
            { ok: true, value: "before" },
            { ok: false, code: -31 },
            { ok: false, code: -32 }
          ], "first batch failure");
          accept("unexpected");
        }
    """

    result = HookRunner().run(source)

    assert result.rejected, result.error
    assert result.return_code == -31
    assert result.return_msg == b"first batch failure"
    assert [call.name for call in result.call_log] == ["rollback"]


def test_rollback_on_any_fail_applies_domain_failure_policy():
    source = """
        export function main(_reserved) {
          rollback.onAnyFail([
            { ok: true, value: "before" },
            { ok: false, issue: "invalid-value" }
          ], "invalid batch value", 137);
          accept("unexpected");
        }
    """

    result = HookRunner().run(source)

    assert result.rejected, result.error
    assert result.return_code == 137
    assert result.return_msg == b"invalid batch value"
    assert [call.name for call in result.call_log] == ["rollback"]


def test_rollback_on_all_fail_returns_successes_in_input_order():
    source = """
        export function main(_reserved) {
          const values = rollback.onAllFail([
            { ok: false, issue: "invalid-value" },
            { ok: true, value: "first" },
            { ok: false, issue: "wrong-length" },
            { ok: true, value: "second" }
          ], "all failed", 91);
          if (values.length !== 2 || values[0] !== "first" ||
              values[1] !== "second") {
            rollback("unexpected successful values", 92);
          }
          accept("partial success", 93);
        }
    """

    result = HookRunner().run(source)

    assert result.accepted, result.error
    assert result.return_code == 93
    assert result.return_msg == b"partial success"
    assert [call.name for call in result.call_log] == ["accept"]


def test_rollback_on_all_fail_applies_policy_to_all_failed_and_empty_batches():
    all_failed = """
        export function main(_reserved) {
          rollback.onAllFail([
            { ok: false, code: -41 },
            { ok: false, issue: "invalid-value" }
          ], "no operation succeeded", 94);
          accept("unexpected");
        }
    """
    empty = """
        export function main(_reserved) {
          rollback.onAllFail([], "empty batch", 95);
          accept("unexpected");
        }
    """

    failed_result = HookRunner().run(all_failed)
    empty_result = HookRunner().run(empty)

    assert failed_result.rejected, failed_result.error
    assert failed_result.return_code == 94
    assert failed_result.return_msg == b"no operation succeeded"
    assert [call.name for call in failed_result.call_log] == ["rollback"]
    assert empty_result.rejected, empty_result.error
    assert empty_result.return_code == 95
    assert empty_result.return_msg == b"empty batch"
    assert [call.name for call in empty_result.call_log] == ["rollback"]


def test_hook_terminal_bypasses_javascript_try_catch():
    source = """
        export function main(_reserved) {
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
        export function main(_reserved) {
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


def test_rich_hook_input_retains_bare_array_buffer_from_to_bytes():
    source = """
        export function main(_reserved) {
          const rich = {
            toBytes() {
              return new Uint8Array([0xA1, 0xB2, 0xC3]).buffer;
            },
          };
          rollback.onFail(state.set("RICH", rich));
          rollback.onFail(state.set("HEX", { toBytes: () => "0D0E0F" }));
          accept("rich bytes retained", 85);
        }
    """
    runner = HookRunner()

    result = runner.run(source)

    assert result.accepted, result.error
    assert result.return_msg == b"rich bytes retained"
    assert result.return_code == 85
    assert runner.runtime.state_db[b"RICH"] == bytes([0xA1, 0xB2, 0xC3])
    assert runner.runtime.state_db[b"HEX"] == bytes([0x0D, 0x0E, 0x0F])
    assert [call.name for call in result.call_log] == [
        "state_set",
        "state_set",
        "accept",
    ]


def test_rich_hook_input_preserves_to_bytes_exception():
    source = """
        export function main(_reserved) {
          let message = "missing exception";
          try {
            state.set("KEY", {
              toBytes() { throw new Error("sentinel"); },
            });
          } catch (error) {
            message = error.message;
          }
          if (message !== "sentinel") rollback(`unexpected:${message}`, 86);
          accept("exception preserved", 87);
        }
    """

    result = HookRunner().run(source)

    assert result.accepted, result.error
    assert result.return_msg == b"exception preserved"
    assert result.return_code == 87
    assert [call.name for call in result.call_log] == ["accept"]


def test_later_rich_input_cannot_detach_earlier_key_view():
    source = """
        export function main(_reserved) {
          const keyBuffer = new Uint8Array([75, 69, 89, 33]).buffer;
          const key = new Uint8Array(keyBuffer);
          const value = {
            toBytes() {
              keyBuffer.transfer(0);
              return new Uint8Array([1, 2, 3]);
            },
          };
          rollback.onFail(state.set(key, value));
          accept("detachment safe", 88);
        }
    """
    runner = HookRunner()

    result = runner.run(source)

    assert result.accepted, result.error
    assert result.return_msg == b"detachment safe"
    assert result.return_code == 88
    assert runner.runtime.state_db[b"KEY!"] == bytes([1, 2, 3])
    assert [call.name for call in result.call_log] == ["state_set", "accept"]


def test_accept_does_not_invoke_rich_to_bytes():
    source = """
        export function main(_reserved) {
          let called = false;
          try {
            accept({
              toBytes() {
                called = true;
                return new Uint8Array([1]);
              },
            });
          } catch (error) {
            if (!(error instanceof TypeError)) rollback("wrong error", 89);
          }
          if (called) rollback("accept invoked toBytes", 90);
          accept("rich accept rejected", 91);
        }
    """

    result = HookRunner().run(source)

    assert result.accepted, result.error
    assert result.return_msg == b"rich accept rejected"
    assert result.return_code == 91
    assert [call.name for call in result.call_log] == ["accept"]


def test_module_without_main_export_is_refused():
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
    assert "no exported main entry point" in str(result.error)
    assert result.call_log == []


def test_xahau_provider_runs_compiled_typescript_hook():
    assert XAHAU_PROVIDER.exists(), "build the XAHAU_HOOK_PROVIDER CMake variant"
    assert XAHAU_TYPESCRIPT.exists()

    result = HookRunner(wasm_path=XAHAU_PROVIDER).run_file(XAHAU_TYPESCRIPT)

    assert result.accepted, result.error
    assert result.return_msg == b"hello from TypeScript"
    assert result.return_code == 42
    assert [call.name for call in result.call_log] == ["accept"]


def test_typescript_public_api_reaches_the_real_host():
    source = """
        export function main(_reserved: number): never {
          const previous = rollback.onFail(state.get("TYPED"));
          if (previous !== undefined) rollback("state unexpectedly present", 92);

          rollback.onAnyFail([
            state.set("TYPED", STBlob.from("A1B2C3")),
            state.set("TYPED-SECOND", new Uint8Array([4, 5, 6])),
          ]);
          const selected = rollback.onAllFail<
            string,
            ResultFailure & { readonly issue: "missing" }
          >(
            [
              { ok: false, issue: "missing" },
              { ok: true, value: "kept" },
            ],
            "no typed candidate",
            96,
          );
          if (selected.length !== 1 || selected[0] !== "kept") {
            rollback("typed result mismatch", 97);
          }
          const stored = rollback.require(
            state.get("TYPED"),
            "typed state write disappeared",
            93,
          );
          if (stored.toHex() !== "A1B2C3") rollback("typed state mismatch", 94);
          accept("TypeScript reached hookz", 95);
        }
    """
    runner = HookRunner()

    result = runner.run_typescript(source)

    assert result.accepted, result.error
    assert result.return_msg == b"TypeScript reached hookz"
    assert result.return_code == 95
    assert runner.runtime.state_db[b"TYPED"] == bytes.fromhex("A1B2C3")
    assert runner.runtime.state_db[b"TYPED-SECOND"] == bytes([4, 5, 6])
    assert [call.name for call in result.call_log] == [
        "state",
        "state_set",
        "state_set",
        "state",
        "accept",
    ]
