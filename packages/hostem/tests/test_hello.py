from pathlib import Path

from hostem import HookRunner


EXAMPLE = Path(__file__).parents[1] / "examples" / "hello.hook.js"
ROOT = Path(__file__).parents[3]
XAHAU_PROVIDER = ROOT / "build" / "xahau-provider" / "jshookz_provider.wasm"
XAHAU_TYPESCRIPT = (
    ROOT / "packages" / "hostem" / "examples" / "xahau-accept.hook.ts"
)
XAHAU_STATE_TYPESCRIPT = (
    ROOT / "packages" / "hostem" / "examples" / "xahau-state.hook.ts"
)
XAHAU_STATE_BATCH_TYPESCRIPT = (
    ROOT / "packages" / "hostem" / "examples" / "xahau-state-batch.hook.ts"
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
          if (write.ok || write.error.domain !== "host" ||
              Object.isExtensible(write.error)) {
            rollback("invalid host error envelope", 68);
          }
          if (!write.ok) rollback("state failed", write.error.code);
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


def test_rollback_on_fail_rejects_structural_result_lookalikes():
    source = """
        export function main(_reserved) {
          let rejected = false;
          try {
            rollback.onFail(
              { ok: false, issue: "wrong-length" },
              "invalid configuration",
              137
            );
          } catch (error) {
            rejected = error instanceof TypeError;
          }
          if (!rejected) rollback("structural Result accepted", 136);
          accept("nominal Result required", 137);
        }
    """

    result = HookRunner().run(source)

    assert result.accepted, result.error
    assert result.return_code == 137
    assert result.return_msg == b"nominal Result required"
    assert [call.name for call in result.call_log] == ["accept"]


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


def test_results_are_nominal_frozen_values_and_require_returns_truthy_values():
    source = """
        export function main() {
          const result = state.get("MISSING");
          const ok = Object.getOwnPropertyDescriptor(result, "ok");
          const value = Object.getOwnPropertyDescriptor(result, "value");
          const prototype = Object.getPrototypeOf(result);
          const okMapOr = Object.getOwnPropertyDescriptor(prototype, "okMapOr");
          if (Object.isExtensible(result) || !ok || ok.writable ||
              ok.configurable || !value || value.writable ||
              value.configurable || !Object.isFrozen(prototype) || !okMapOr ||
              okMapOr.writable || okMapOr.configurable) {
            rollback("mutable Result", 74);
          }
          if (result.moot !== undefined) rollback("value Result is mootable", 79);

          const values = [true, 1, 1n, "value", { present: true }];
          for (const item of values) {
            if (rollback.require(item, "truthy value missing", 75) !== item) {
              rollback("truthy value changed", 76);
            }
          }
          accept("nominal and present", 77);
        }
    """

    result = HookRunner().run(source)

    assert result.accepted, result.error
    assert result.return_code == 77
    assert result.return_msg == b"nominal and present"
    assert [call.name for call in result.call_log] == ["state", "accept"]


def test_rollback_require_rejects_direct_falsy_values():
    for expression in ["false", "0", "0n", '""', "null", "undefined", "NaN"]:
        source = """
        export function main() {
          rollback.require(EXPR, "direct value missing", 78);
          accept("unexpected");
        }
        """.replace("EXPR", expression)

        result = HookRunner().run(source)

        assert result.rejected, (expression, result.error)
        assert result.return_code == 78
        assert result.return_msg == b"direct value missing"
        assert [call.name for call in result.call_log] == ["rollback"]


def test_rollback_require_defaults_only_the_explicit_policy_code():
    result = HookRunner().run(
        '''
        export function main() {
          rollback.require(state.get("MISSING"), "required value missing");
          accept("unexpected");
        }
        '''
    )

    assert result.rejected, result.error
    assert result.return_msg == b"required value missing"
    assert result.return_code == 0
    assert [call.name for call in result.call_log] == [
        "state",
        "rollback",
    ]


def test_rollback_require_rejects_a_missing_contract_policy():
    result = HookRunner().run(
        '''
        export function main() {
          rollback.require(state.get("MISSING"));
          accept("unexpected");
        }
        '''
    )

    assert not result.accepted
    assert not result.rejected
    assert "rollback.require: expected rollback message" in str(result.error)
    assert [call.name for call in result.call_log] == ["state"]


def test_rollback_on_any_fail_returns_every_value_and_accepts_empty_input():
    source = """
        export function main(_reserved) {
          const empty = rollback.onAnyFail([]);
          const values = rollback.onAnyFail([
            state.get("FIRST"),
            state.get("MISSING"),
            state.get("THIRD")
          ]);
          if (empty.length !== 0 || values.length !== 3 ||
              values[0].toHex() !== "01" || values[1] !== undefined ||
              values[2].toHex() !== "03") {
            rollback("unexpected batch values", 80);
          }
          accept("all succeeded", 81);
        }
    """

    runner = HookRunner()
    runner.runtime.state_db[b"FIRST"] = b"\x01"
    runner.runtime.state_db[b"THIRD"] = b"\x03"
    result = runner.run(source)

    assert result.accepted, result.error
    assert result.return_code == 81
    assert result.return_msg == b"all succeeded"
    assert [call.name for call in result.call_log] == [
        "state",
        "state",
        "state",
        "accept",
    ]


def test_rollback_on_any_fail_uses_first_failure_in_input_order():
    source = """
        export function main(_reserved) {
          rollback.onAnyFail([
            state.set("BEFORE", new Uint8Array([1])),
            state.set("A".repeat(33), new Uint8Array([2])),
            state.set("B".repeat(33), new Uint8Array([3]))
          ], "first batch failure");
          accept("unexpected");
        }
    """

    result = HookRunner().run(source)

    assert result.rejected, result.error
    assert result.return_code < 0
    assert result.return_msg == b"first batch failure"
    assert [call.name for call in result.call_log] == [
        "state_set",
        "state_set",
        "state_set",
        "rollback",
    ]


def test_rollback_on_any_fail_applies_domain_failure_policy():
    source = """
        export function main(_reserved) {
          rollback.onAnyFail([
            state.set("BEFORE", new Uint8Array([1])),
            state.set("A".repeat(33), new Uint8Array([2]))
          ], "invalid batch value", 137);
          accept("unexpected");
        }
    """

    result = HookRunner().run(source)

    assert result.rejected, result.error
    assert result.return_code == 137
    assert result.return_msg == b"invalid batch value"
    assert [call.name for call in result.call_log] == [
        "state_set",
        "state_set",
        "rollback",
    ]


def test_rollback_on_all_fail_returns_successes_in_input_order():
    source = """
        export function main(_reserved) {
          const values = rollback.onAllFail([
            state.set("A".repeat(33), new Uint8Array([1])),
            state.get("FIRST"),
            state.set("B".repeat(33), new Uint8Array([2])),
            state.get("SECOND")
          ], "all failed", 91);
          if (values.length !== 2 || values[0].toHex() !== "01" ||
              values[1].toHex() !== "02") {
            rollback("unexpected successful values", 92);
          }
          accept("partial success", 93);
        }
    """

    runner = HookRunner()
    runner.runtime.state_db[b"FIRST"] = b"\x01"
    runner.runtime.state_db[b"SECOND"] = b"\x02"
    result = runner.run(source)

    assert result.accepted, result.error
    assert result.return_code == 93
    assert result.return_msg == b"partial success"
    assert [call.name for call in result.call_log] == [
        "state_set",
        "state",
        "state_set",
        "state",
        "accept",
    ]


def test_rollback_on_all_fail_applies_policy_to_all_failed_and_empty_batches():
    all_failed = """
        export function main(_reserved) {
          rollback.onAllFail([
            state.set("A".repeat(33), new Uint8Array([1])),
            state.set("B".repeat(33), new Uint8Array([2]))
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
    assert [call.name for call in failed_result.call_log] == [
        "state_set",
        "state_set",
        "rollback",
    ]
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
          if (!seeded.ok) rollback("state read failed", seeded.error.code);
          if (seeded.value === undefined) rollback("state missing", -1);
          const seededHex = seeded.value.toHex();
          trace("js-state-read", seededHex);
          if (seededHex !== "66726F6D2D63") {
            rollback(`unexpected state:${seededHex}`, -2);
          }
          const write = state.set("BRIDGE", "from-js");
          if (!write.ok) rollback("state write failed", write.error.code);
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


def test_trace_dispatches_scalars_and_byte_values_without_c_api_variants():
    source = """
        export function main() {
          trace("number", 42n);
          trace("bytes", STBlob.fromHex("A1B2"));
          trace("object", { toString() { return "rendered"; } });
          accept("traced", 84);
        }
    """
    runner = HookRunner()

    result = runner.run(source)

    assert result.accepted, result.error
    assert result.return_msg == b"traced"
    assert [entry.tag for entry in runner.runtime.traces] == [
        "number",
        "bytes",
        "object",
    ]
    assert [entry.value for entry in runner.runtime.traces] == [
        "b'42'",
        "a1b2",
        "b'rendered'",
    ]
    assert [call.name for call in result.call_log] == [
        "trace",
        "trace",
        "trace",
        "accept",
    ]


def test_bytes_like_accepts_byte_arrays_and_produces_uint8_arrays():
    source = """
        export function main() {
          const blob = STBlob.from([0, 127, 255]);
          const bytes = blob.toBytes();
          if (!(bytes instanceof Uint8Array) ||
              bytes.length !== 3 || bytes[2] !== 255 ||
              blob.toHex() !== "007FFF") {
            rollback("byte array mismatch", 85);
          }
          bytes[0] = 99;
          if (blob.toBytes()[0] !== 0)
            rollback("byte output aliases provider memory", 85);
          let rejected = false;
          try { STBlob.from([0, 256]); } catch (error) {
            rejected = error instanceof TypeError;
          }
          if (!rejected) rollback("invalid byte accepted", 86);
          accept("byte arrays", 87);
        }
    """

    result = HookRunner().run(source)

    assert result.accepted, result.error
    assert result.return_msg == b"byte arrays"
    assert result.return_code == 87


def test_lifecycle_messages_accept_declared_stblob_values():
    source = """
        export function main() {
          accept(STBlob.fromHex("A1B2"), 88);
        }
    """

    result = HookRunner().run(source)

    assert result.accepted, result.error
    assert result.return_msg == bytes.fromhex("A1B2")
    assert result.return_code == 88


def test_emit_bytes_reject_undeclared_strings_before_crossing_the_host():
    source = """
        export function main() {
          let prepareRejected = false;
          let txRejected = false;
          try { emit.prepare("A1B2"); } catch (error) {
            prepareRejected = error instanceof TypeError;
          }
          try { emit.tx("A1B2"); } catch (error) {
            txRejected = error instanceof TypeError;
          }
          if (!prepareRejected || !txRejected)
            rollback("emit accepted string bytes", 89);
          accept("emit byte policy", 90);
        }
    """

    result = HookRunner().run(source)

    assert result.accepted, result.error
    assert result.return_msg == b"emit byte policy"
    assert [call.name for call in result.call_log] == ["accept"]


def test_rich_byte_coercion_requires_a_provider_registered_class():
    source = """
        export function main() {
          const counterfeit = { toBytes() { return new Uint8Array([1]); } };
          let stateRejected = false;
          let emitRejected = false;
          try { state.set("COUNTERFEIT", counterfeit); } catch (error) {
            stateRejected = error instanceof TypeError;
          }
          try { emit.tx(counterfeit); } catch (error) {
            emitRejected = error instanceof TypeError;
          }
          if (!stateRejected || !emitRejected)
            rollback("counterfeit byte type accepted", 91);
          accept("nominal byte policy", 92);
        }
    """

    result = HookRunner().run(source)

    assert result.accepted, result.error
    assert result.return_msg == b"nominal byte policy"
    assert [call.name for call in result.call_log] == ["accept"]


def test_bounded_uint_values_pin_width_arithmetic_and_conversion_policy():
    source = """
        export function main() {
          const byte = rollback.onFail(UInt8.from(255n), "u8 construction", 100);
          const previous = rollback.onFail(UInt8.from(254n), "u8 comparison", 100);
          if (byte.bits !== 8 || byte.byteLength !== 1 ||
              byte.toBigInt() !== 255n || byte.toNumber() !== 255 ||
              byte.toString() !== "255" || byte.isZero() ||
              !byte.equals(255n) || byte.compare(previous) !== 1) {
            rollback("u8 value mismatch", 101);
          }

          const overflow = byte.add(1);
          if (overflow.ok || Object.isExtensible(overflow.error) ||
              overflow.error.domain !== "uint" ||
              overflow.error.issue !== "overflow" ||
              overflow.error.bits !== 8) {
            rollback("overflow result mismatch", 102);
          }
          const underflow = UInt8.zero.subtract(1);
          if (underflow.ok || underflow.error.domain !== "uint" ||
              underflow.error.issue !== "underflow" ||
              underflow.error.bits !== 8) {
            rollback("underflow result mismatch", 103);
          }
          if (!byte.saturatingAdd(previous).equals(UInt8.max) ||
              !UInt8.zero.saturatingSubtract(byte).equals(UInt8.zero)) {
            rollback("saturating arithmetic mismatch", 104);
          }
          let rejectedSaturatingOperand = false;
          try {
            byte.saturatingAdd(256);
          } catch (error) {
            rejectedSaturatingOperand = error instanceof TypeError;
          }
          if (!rejectedSaturatingOperand) {
            rollback("saturating operand contract mismatch", 105);
          }
          let rejectedSaturatingWidth = false;
          try {
            byte.saturatingAdd(UInt16.zero);
          } catch (error) {
            rejectedSaturatingWidth = error instanceof RangeError;
          }
          if (!rejectedSaturatingWidth) {
            rollback("saturating width contract mismatch", 105);
          }

          const quotient = rollback.require(
            UInt64.mulDiv(UInt64.max, UInt64.max, UInt64.max),
            "mulDiv construction",
            112
          );
          if (!quotient.equals(UInt64.max)) {
            rollback("mulDiv exact result mismatch", 114);
          }
          const floor = rollback.require(
            UInt8.mulDiv(10, 3, 4),
            "mulDiv floor",
            116
          );
          if (!floor.equals(7)) rollback("mulDiv floor mismatch", 117);
          const mulDivOverflow = UInt64.mulDiv(UInt64.max, 2n, 1n);
          const divisionByZero = UInt64.mulDiv(UInt64.max, 1n, 0n);
          if (mulDivOverflow.ok ||
              mulDivOverflow.error.issue !== "overflow" ||
              divisionByZero.ok ||
              divisionByZero.error.issue !== "division-by-zero") {
            rollback("mulDiv failure mismatch", 118);
          }

          const outOfRange = UInt64.from(18446744073709551616n);
          const negative = UInt64.from(-1n);
          const unsafeNumber = UInt64.from(9007199254740992);
          for (const result of [outOfRange, negative, unsafeNumber]) {
            if (result.ok || result.error.domain !== "uint" ||
                result.error.issue !== "out-of-range" ||
                result.error.bits !== 64) {
              rollback("range result mismatch", 105);
            }
          }

          const safe = rollback.onFail(
            UInt64.from(9007199254740991n), "u64 construction", 106);
          const safeNumber = rollback.onFail(
            safe.toNumber(), "safe conversion", 107);
          if (safeNumber !== 9007199254740991) {
            rollback("safe conversion mismatch", 108);
          }
          const unsafeConversion = UInt64.max.toNumber();
          if (unsafeConversion.ok ||
              unsafeConversion.error.domain !== "uint" ||
              unsafeConversion.error.issue !== "out-of-range") {
            rollback("unsafe conversion mismatch", 109);
          }

          if (Object.isExtensible(byte) || Object.isExtensible(UInt8)) {
            rollback("UInt values are mutable", 110);
          }
          trace("uint", byte);
          accept("bounded UInt", 111);
        }
    """
    runner = HookRunner()

    result = runner.run(source)

    assert result.accepted, result.error
    assert result.return_code == 111
    assert result.return_msg == b"bounded UInt"
    assert runner.runtime.traces[0].value == "b'255'"
    assert [call.name for call in result.call_log] == ["trace", "accept"]


def test_state_accepts_provider_registered_serialized_types():
    source = """
        export function main(_reserved) {
          rollback.onFail(state.set("RICH", STBlob.fromHex("A1B2C3")));
          rollback.onFail(state.set("HASH", Hash256.from(new Uint8Array(32))));
          rollback.onFail(state.set("ACCT", AccountID.one));
          accept("rich bytes retained", 85);
        }
    """
    runner = HookRunner()

    result = runner.run(source)

    assert result.accepted, result.error
    assert result.return_msg == b"rich bytes retained"
    assert result.return_code == 85
    assert runner.runtime.state_db[b"RICH"] == bytes([0xA1, 0xB2, 0xC3])
    assert runner.runtime.state_db[b"HASH"] == bytes(32)
    assert runner.runtime.state_db[b"ACCT"] == bytes(19) + b"\x01"
    assert [call.name for call in result.call_log] == [
        "state_set",
        "state_set",
        "state_set",
        "accept",
    ]


def test_counterfeit_rich_input_is_rejected_without_executing_to_bytes():
    source = """
        export function main(_reserved) {
          let called = false;
          let rejected = false;
          try {
            state.set("KEY", {
              toBytes() { called = true; throw new Error("sentinel"); },
            });
          } catch (error) {
            rejected = error instanceof TypeError;
          }
          if (!rejected || called) rollback("counterfeit executed", 86);
          accept("counterfeit rejected", 87);
        }
    """

    result = HookRunner().run(source)

    assert result.accepted, result.error
    assert result.return_msg == b"counterfeit rejected"
    assert result.return_code == 87
    assert [call.name for call in result.call_log] == ["accept"]


def test_registered_rich_input_uses_native_converter_not_shadowed_method():
    source = """
        export function main(_reserved) {
          const keyBuffer = new Uint8Array([75, 69, 89, 33]).buffer;
          const key = new Uint8Array(keyBuffer);
          const value = STBlob.from([1, 2, 3]);
          Object.defineProperty(value, "toBytes", {
            value() {
              keyBuffer.transfer(0);
              return new Uint8Array([9]);
            },
          });
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


def test_readme_state_example_runs_against_xahau_provider():
    result = HookRunner(wasm_path=XAHAU_PROVIDER).run_file(XAHAU_STATE_TYPESCRIPT)

    assert result.accepted, result.error
    assert result.return_msg == b"Hi"
    assert result.return_code == 0
    assert result.state_writes
    assert [call.name for call in result.call_log] == [
        "state",
        "state_set",
        "state",
        "accept",
    ]


def test_readme_batch_example_runs_against_xahau_provider():
    result = HookRunner(wasm_path=XAHAU_PROVIDER).run_file(
        XAHAU_STATE_BATCH_TYPESCRIPT
    )

    assert result.accepted, result.error
    assert result.return_msg == b"first=1"
    assert result.return_code == 0
    assert len(result.state_writes) == 2
    assert [call.name for call in result.call_log] == [
        "state_set",
        "state_set",
        "state",
        "accept",
    ]


def test_typescript_public_api_reaches_the_real_host():
    source = """
        export function main(): never {
          const previous = rollback.onFail(state.get("TYPED"));
          if (previous !== undefined) rollback("state unexpectedly present", 92);

          rollback.onAnyFail([
            state.set("TYPED", STBlob.fromHex("A1B2C3")),
            state.set("TYPED-SECOND", new Uint8Array([4, 5, 6])),
          ]);
          const selected = rollback.onAllFail(
            [
              state.get("X".repeat(33)),
              state.get("TYPED"),
            ],
            "no typed candidate",
            96,
          );
          if (selected.length !== 1 || selected[0]?.toHex() !== "A1B2C3") {
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
        "state",
        "state",
        "accept",
    ]


def test_result_ok_or_uses_fallback_only_for_failure():
    source = """
        export function main(): never {
          const absent = state.get("MISSING").okOr(STBlob.fromHex("AA"));
          if (absent !== undefined) rollback("absence used fallback", 97);

          const fallback = state.get("X".repeat(33)).okOr(STBlob.fromHex("BB"));
          if (fallback === undefined || fallback.toHex() !== "BB") {
            rollback("failure missed fallback", 98);
          }
          accept("result fallback ok", 99);
        }
    """

    result = HookRunner().run_typescript(source)

    assert result.accepted, result.error
    assert result.return_code == 99
    assert result.return_msg == b"result fallback ok"


def test_result_ok_or_handle_receives_only_failures():
    source = """
        export function main(): never {
          let handled = 0;
          const absent = state.get("MISSING").okOrHandle(() => {
            handled += 1;
            return STBlob.fromHex("AA");
          });
          if (absent !== undefined) rollback("absence invoked handler", 100);

          const fallback = state.get("X".repeat(33)).okOrHandle(error => {
            handled += 1;
            if (error.domain !== "host") rollback("wrong error domain", 101);
            return STBlob.fromHex("CC");
          });
          if (handled !== 1 || fallback === undefined || fallback.toHex() !== "CC") {
            rollback("failure handler mismatch", 102);
          }
          accept("result handler ok", 103);
        }
    """

    result = HookRunner().run_typescript(source)

    assert result.accepted, result.error
    assert result.return_code == 103
    assert result.return_msg == b"result handler ok"


def test_result_ok_map_or_transforms_success_and_extracts_a_bare_value():
    source = '''
        export function main(): never {
          let calls = 0;
          const mapped = state.get("VALUE").okMapOr(value => {
            calls += 1;
            return value?.toHex();
          }, "fallback");
          if (mapped !== "A1B2") rollback("success mapping failed", 115);

          const absent = state.get("MISSING").okMapOr(
            value => value === undefined ? "absent" : "present",
            "failure"
          );
          if (absent !== "absent") rollback("undefined success lost", 116);

          const failed = state.get("X".repeat(33)).okMapOr(() => {
            calls += 1;
            return "unexpected";
          }, "fallback");
          if (failed !== "fallback" || calls !== 1) {
            rollback("failure extraction failed", 117);
          }
          accept("result map-or ok", 118);
        }
    '''
    runner = HookRunner()
    runner.runtime.state_db[b"VALUE"] = bytes.fromhex("A1B2")

    result = runner.run_typescript(source)

    assert result.accepted, result.error
    assert result.return_code == 118
    assert result.return_msg == b"result map-or ok"


def test_void_result_moot_declares_failure_irrelevant():
    result = HookRunner().run_typescript(
        '''
        export function main(): never {
          state.set("TRANSIENT", new Uint8Array([1])).moot();
          state.set("X".repeat(33), new Uint8Array([2])).moot();
          accept("result moot", 119);
        }
        '''
    )

    assert result.accepted, result.error
    assert result.return_code == 119
    assert result.return_msg == b"result moot"
    assert [call.name for call in result.call_log] == [
        "state_set",
        "state_set",
        "accept",
    ]


def test_value_result_cannot_be_made_moot_from_javascript():
    result = HookRunner().run(
        '''
        export function main() {
          const valueResult = state.get("MISSING");
          if (valueResult.moot !== undefined) {
            rollback("value Result exposes moot", 121);
          }
          const effectResult = state.set("TRANSIENT", new Uint8Array([1]));
          if (typeof effectResult.moot !== "function" ||
              !Object.isFrozen(Object.getPrototypeOf(effectResult))) {
            rollback("void Result lacks frozen moot capability", 122);
          }
          try {
            effectResult.moot.call(valueResult);
          } catch (error) {
            if (error instanceof TypeError &&
                error.message === "Result.moot: expected void-effect Result") {
              accept("value result protected", 120);
            }
          }
          rollback("borrowed moot accepted value Result", 123);
        }
        '''
    )

    assert result.accepted, result.error
    assert result.return_code == 120
    assert result.return_msg == b"value result protected"


def test_hook_account_is_the_declared_total_account_id():
    result = HookRunner().run_typescript(
        '''
        export function main(): never {
          const account: AccountID = hook.account();
          if (account.toHex().length !== 40 || account.toBytes().length !== 20) {
            rollback("invalid Hook account", 108);
          }
          accept("total Hook account", 109);
        }
        '''
    )

    assert result.accepted, result.error
    assert result.return_code == 109
    assert result.return_msg == b"total Hook account"
    assert [call.name for call in result.call_log] == ["hook_account", "accept"]
