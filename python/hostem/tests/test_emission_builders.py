from __future__ import annotations

import pytest

from hookz.runtime import HookRuntime
from hostem.runner import HookRunner


# Independently decoded and re-encoded with the native xahau-codec. These are
# static oracle outputs; the provider under test does not produce its own
# expected bytes. SigningPubKey follows xahaud HookAPI::prepare's emission
# convention: a 33-byte all-zero VL, not the also-accepted empty VL.
PAYMENT = bytes.fromhex(
    """
    120000
    2280000000
    2300000000
    2400000000
    2E00000000
    201A00000065
    201B00000069
    614000000000000001
    68400000000000000A
    7321000000000000000000000000000000000000000000000000000000000000000000
    81140000000000000000000000000000000000000000
    83141111111111111111111111111111111111111111
    ED
      202E00000001
      3D0000000000000001
      5BABABABABABABABABABABABABABABABABABABABABABABABABABABABABABABABAB
      5CCDCDCDCDCDCDCDCDCDCDCDCDCDCDCDCDCDCDCDCDCDCDCDCDCDCDCDCDCDCDCDCD
      5D0000000000000000000000000000000000000000000000000000000000000000
    E1
    """
)

HOOK_SET = bytes.fromhex(
    """
    120016
    2400000000
    201A00000065
    201B00000069
    68400000000000000A
    7321000000000000000000000000000000000000000000000000000000000000000000
    81140000000000000000000000000000000000000000
    ED
      202E00000001
      3D0000000000000001
      5BABABABABABABABABABABABABABABABABABABABABABABABABABABABABABABABAB
      5CCDCDCDCDCDCDCDCDCDCDCDCDCDCDCDCDCDCDCDCDCDCDCDCDCDCDCDCDCDCDCDCD
      5D0000000000000000000000000000000000000000000000000000000000000000
    E1
    FB
      EE
        2200000001
        501F2222222222222222222222222222222222222222222222222222222222222222
        50203333333333333333333333333333333333333333333333333333333333333333
        F013E0177018014E70190156E1F1
        F014E018501F4444444444444444444444444444444444444444444444444444444444444444
          85145555555555555555555555555555555555555555E1F1
      E1
      EE22000000017B00E1
      EE22000000017B00E1
      EE22000000017B00E1
    F1
    F013E017701804524F4F5470190556414C5545E1F1
    """
)


def _runtime() -> HookRuntime:
    runtime = HookRuntime()
    runtime.hook_account = bytes(20)
    runtime.ledger_seq_val = 100
    return runtime


def _call_names(result) -> list[str]:
    return [call.name for call in result.call_log]


def test_payment_builder_matches_native_codec_and_submits_identical_bytes() -> None:
    source = """
        function u32(value: number): UInt32 {
          const result = UInt32.from(value);
          if (!result.ok) rollback("invalid UInt32 fixture", 1);
          return result.value;
        }

        export function main(): never {
          rollback.onFail(emit.reserve(1), "cannot reserve");
          const built = rollback.onFail(
            emit.build.payment({
              destination: AccountID.fromHex("11".repeat(20)),
              amount: Amount.drops(1n),
              sourceTag: u32(0),
              destinationTag: u32(0),
              flags: u32(TransactionFlag.tfFullyCanonicalSig),
            }),
            "cannot build Payment",
            2,
          );
          if (built.kind !== TransactionType.Payment ||
              !(built.blob instanceof STBlob) ||
              Object.isExtensible(built)) {
            rollback("invalid emitted transaction", 3);
          }

          let structuralRejected = false;
          try {
            emit.tx({blob: built.blob, kind: built.kind} as any).okOr(undefined);
          } catch (error) {
            structuralRejected = error instanceof TypeError;
          }
          if (!structuralRejected) rollback("structural emission accepted", 4);

          rollback.onFail(emit.tx(built), "cannot submit Payment");
          accept("canonical Payment emitted");
        }
    """
    runtime = _runtime()

    result = HookRunner(runtime).run_typescript(source)

    assert result.accepted, result.error
    assert runtime.emitted_txns == [PAYMENT]
    assert _call_names(result) == [
        "etxn_reserve",
        "hook_account",
        "ledger_seq",
        "etxn_details",
        "etxn_fee_base",
        "emit",
        "accept",
    ]


def test_hook_set_builder_matches_native_codec_with_fillers_and_deletions() -> None:
    source = """
        export function main(): never {
          rollback.onFail(emit.reserve(1), "cannot reserve");
          const built = rollback.onFail(
            emit.build.hookSet({
              hooks: [
                {
                  position: 0,
                  hookHash: Hash256.fromHex("22".repeat(32)),
                  namespace: Hash256.fromHex("33".repeat(32)),
                  parameters: [{name: "N", value: "V"}],
                  grants: [{
                    hookHash: Hash256.fromHex("44".repeat(32)),
                    authorize: AccountID.fromHex("55".repeat(20)),
                  }],
                },
                {position: 1, hookHash: null},
                {position: 2, hookHash: null},
                {position: 3, hookHash: null},
              ],
              hookParameters: [{name: "ROOT", value: "VALUE"}],
            }),
            "cannot build SetHook",
            1,
          );
          if (built.kind !== TransactionType.SetHook) {
            rollback("wrong emitted transaction kind", 2);
          }
          rollback.onFail(emit.tx(built), "cannot submit SetHook");
          accept("canonical SetHook emitted");
        }
    """
    runtime = _runtime()

    result = HookRunner(runtime).run_typescript(source)

    assert result.accepted, result.error
    assert runtime.emitted_txns == [HOOK_SET]
    assert _call_names(result) == [
        "etxn_reserve",
        "hook_account",
        "ledger_seq",
        "etxn_details",
        "etxn_fee_base",
        "emit",
        "accept",
    ]


def test_builder_shape_failures_are_local_and_field_specific() -> None:
    source = """
        function expectEncode(
          result: emit.BuildResult,
          field: string,
        ): void {
          const outcome = result.okOrHandle((error) => error);
          if (!("stage" in outcome) ||
              outcome.domain !== "encode" ||
              outcome.issue !== "invalid-value" ||
              outcome.stage !== "encode" ||
              outcome.field !== field) {
            rollback(`wrong encode failure: ${field}`, 1);
          }
        }

        export function main(): never {
          const amount = Amount.drops(1n);
          const destination = AccountID.fromHex("11".repeat(20));
          const hash = Hash256.fromHex("22".repeat(32));
          const account = AccountID.fromHex("33".repeat(20));

          expectEncode(
            emit.build.payment({amount} as any),
            "destination",
          );
          expectEncode(
            emit.build.payment({
              destination,
              amount,
              hookParameters: [
                {name: "D", value: "one"},
                {name: "D", value: "two"},
              ],
            }),
            "hookParameters",
          );
          expectEncode(emit.build.hookSet({hooks: []}), "hooks");
          expectEncode(
            emit.build.hookSet({
              hooks: [
                {position: 1, hookHash: hash},
                {position: 1, hookHash: null},
              ],
            }),
            "hooks.position",
          );
          expectEncode(
            emit.build.hookSet({
              hooks: [{
                position: 0,
                hookHash: hash,
                grants: [
                  {hookHash: hash, authorize: account},
                  {hookHash: hash, authorize: account},
                ],
              }],
            }),
            "hooks.grants",
          );
          accept("local builder failures stayed local");
        }
    """
    runtime = _runtime()

    result = HookRunner(runtime).run_typescript(source)

    assert result.accepted, result.error
    assert _call_names(result) == ["accept"]


def test_builder_caps_accept_the_boundary_and_reject_one_over_locally() -> None:
    parameters = ",\n".join(
        f'{{name: "P{index}", value: "V{index}"}}' for index in range(16)
    )
    grants = ",\n".join(
        "{"
        f'hookHash: Hash256.fromHex("{index + 1:02X}".repeat(32)), '
        f'authorize: AccountID.fromHex("{index + 17:02X}".repeat(20))'
        "}"
        for index in range(8)
    )
    source = f"""
        function expectEncode(result: emit.BuildResult, field: string): void {{
          const outcome = result.okOrHandle((error) => error);
          if (!("stage" in outcome) || outcome.stage !== "encode" ||
              outcome.field !== field) {{
            rollback(`wrong cap failure: ${{field}}`, 1);
          }}
        }}

        export function main(): never {{
          const destination = AccountID.fromHex("AA".repeat(20));
          const amount = Amount.drops(1n);
          const hash = Hash256.fromHex("BB".repeat(32));

          expectEncode(
            emit.build.payment({{
              destination,
              amount,
              hookParameters: Array.from(
                {{length: 17}},
                (_, index) => ({{name: `P${{index}}`, value: "V"}}),
              ),
            }}),
            "hookParameters",
          );
          expectEncode(
            emit.build.hookSet({{
              hooks: Array.from(
                {{length: 11}},
                (_, position) => ({{position, hookHash: hash}}),
              ),
            }}),
            "hooks",
          );
          expectEncode(
            emit.build.hookSet({{
              hooks: [{{
                position: 0,
                hookHash: hash,
                grants: Array.from(
                  {{length: 9}},
                  (_, index) => ({{
                    hookHash: Hash256.fromHex(
                      ("0" + (index + 1).toString(16)).slice(-2).repeat(32),
                    ),
                    authorize: AccountID.fromHex(
                      ("0" + (index + 17).toString(16)).slice(-2).repeat(20),
                    ),
                  }}),
                ),
              }}],
            }}),
            "hooks.grants",
          );

          rollback.onFail(emit.reserve(1), "cannot reserve");
          rollback.onFail(
            emit.build.hookSet({{
              hooks: [{{
                position: 9,
                hookHash: hash,
                parameters: [{parameters}],
                grants: [{grants}],
              }}],
            }}),
            "boundary build failed",
            2,
          );
          accept("builder caps are exact");
        }}
    """
    runtime = _runtime()

    result = HookRunner(runtime).run_typescript(source)

    assert result.accepted, result.error
    assert _call_names(result) == [
        "etxn_reserve",
        "hook_account",
        "ledger_seq",
        "etxn_details",
        "etxn_fee_base",
        "accept",
    ]


def test_self_grant_is_rejected_after_total_common_facts_only() -> None:
    source = """
        export function main(): never {
          const outcome = emit.build.hookSet({
            hooks: [{
              position: 0,
              hookHash: Hash256.fromHex("11".repeat(32)),
              grants: [{
                hookHash: Hash256.fromHex("22".repeat(32)),
                authorize: AccountID.fromHex("00".repeat(20)),
              }],
            }],
          }).okOrHandle((error) => error);
          if (!("stage" in outcome) || outcome.stage !== "encode" ||
              outcome.field !== "hooks.grants") {
            rollback("self grant was not rejected", 1);
          }
          accept("self grant rejected");
        }
    """
    runtime = _runtime()

    result = HookRunner(runtime).run_typescript(source)

    assert result.accepted, result.error
    assert _call_names(result) == ["hook_account", "ledger_seq", "accept"]


def test_missing_reservation_is_a_details_stage_host_failure() -> None:
    source = """
        export function main(): never {
          const outcome = emit.build.payment({
            destination: AccountID.fromHex("11".repeat(20)),
            amount: Amount.drops(1n),
          }).okOrHandle((error) => error);
          if (!("stage" in outcome) || outcome.domain !== "host" ||
              Number(outcome.code) !== -9 || outcome.stage !== "details") {
            rollback("missing reservation was not preserved", 1);
          }
          accept("missing reservation preserved");
        }
    """
    runtime = _runtime()

    result = HookRunner(runtime).run_typescript(source)

    assert result.accepted, result.error
    assert _call_names(result) == [
        "hook_account",
        "ledger_seq",
        "etxn_details",
        "accept",
    ]


@pytest.mark.parametrize(
    ("failed_stage", "status", "expected_calls"),
    [
        (
            "details",
            -77,
            ["etxn_reserve", "hook_account", "ledger_seq", "etxn_details"],
        ),
        (
            "fee",
            -88,
            [
                "etxn_reserve",
                "hook_account",
                "ledger_seq",
                "etxn_details",
                "etxn_fee_base",
            ],
        ),
    ],
)
def test_builder_preserves_exact_host_failure_stage(
    failed_stage: str,
    status: int,
    expected_calls: list[str],
) -> None:
    source = f"""
        export function main(): never {{
          rollback.onFail(emit.reserve(1), "cannot reserve");
          const outcome = emit.build.payment({{
            destination: AccountID.fromHex("11".repeat(20)),
            amount: Amount.drops(1n),
          }}).okOrHandle((error) => error);
          if (!("stage" in outcome) || outcome.domain !== "host" ||
              Number(outcome.code) !== {status} ||
              outcome.stage !== "{failed_stage}") {{
            rollback("wrong staged host failure", 1);
          }}
          accept("staged host failure preserved");
        }}
    """
    runtime = _runtime()
    runtime.handlers[
        f"etxn_{failed_stage}" if failed_stage == "details" else "etxn_fee_base"
    ] = lambda *_args: status

    result = HookRunner(runtime).run_typescript(source)

    assert result.accepted, result.error
    assert _call_names(result) == [*expected_calls, "accept"]


def test_invalid_positive_details_length_is_an_execution_invariant_failure() -> None:
    runtime = _runtime()
    runtime.handlers["etxn_details"] = lambda *_args: 115
    source = """
        export function main(): never {
          rollback.onFail(emit.reserve(1), "cannot reserve");
          emit.build.payment({
            destination: AccountID.fromHex("11".repeat(20)),
            amount: Amount.drops(1n),
          }).okOr(undefined);
          accept("unreachable");
        }
    """

    result = HookRunner(runtime).run_typescript(source)

    assert not result.accepted
    assert not result.rejected
    assert result.error is not None
    assert "etxn_details returned invalid field length 115" in str(result.error)
    assert _call_names(result) == [
        "etxn_reserve",
        "hook_account",
        "ledger_seq",
        "etxn_details",
    ]
