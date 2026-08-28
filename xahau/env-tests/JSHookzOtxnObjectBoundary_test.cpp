//------------------------------------------------------------------------------
/*
    Native xahaud proof for the current working-tree otxn.object provider.

    Build outputs are supplied by the producer-side runner. This file contains
    no fixture identity pins: the completion receipt records exact identities.
*/
//==============================================================================

#include "JSHookzOtxnObjectBoundary_test_hooks.h"
#include <test/jtx.h>
#include <test/jtx/hook.h>
#include <xrpl/basics/scope.h>
#include <xrpl/beast/unit_test/suite.h>
#include <xrpl/hook/Enum.h>
#include <xrpl/hook/HookArtifact.h>
#include <xrpl/protocol/Protocol.h>
#include <xrpld/app/hook/QuickJSHookRuntime.h>
#include <xrpld/app/tx/applySteps.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace ripple::test {

class JSHookzOtxnObjectBoundary_test : public beast::unit_test::suite {
  static Blob readBinary(char const *path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, {}};
  }

public:
  void run() override {
    using namespace jtx;

    auto const &largeTransactionWasm =
        jshookzotxnobjectboundary_test_wasm.at(R"[test.hook](
#include "hookapi.h"

// Keep the maximum Hook body in a real data section. Post-hoc custom-section
// padding is not an accepted witness: SetHook's guard validator rejects every
// custom section before preflight can succeed.
static volatile uint8_t accepted_large_padding[65318] = {
    [0 ... 65317] = 0x5A,
};

int64_t hook(uint32_t reserved) {
  _g(1, 1);
  if (accepted_large_padding[reserved & 0] != 0x5A)
    return rollback(SBUF("large transaction padding changed"), 1);
  return accept(SBUF("large transaction witness"), reserved);
}
)[test.hook]");

    auto const &callbackBytecode =
        jshookzotxnobjectboundary_test_wasm.at(R"[test.tshook](
function requireType(transaction: STObject, expected: TransactionType): void {
  const transactionType = rollback.requirePresent(
    transaction.get(Field.TransactionType),
    "originating transaction has no TransactionType",
    70,
  );
  rollback.when(transactionType !== expected, "unexpected transaction type", 71);
}

function requireCachedObject(expected: TransactionType): void {
  const first = otxn.object();
  const second = otxn.object();
  rollback.when(first !== second, "originating transaction was not cached", 72);
  requireType(first, expected);
}

export function main(): never {
  requireCachedObject(TransactionType.Payment);
  accept("main observed Payment", 201);
}

export function callback(info: CallbackInfo): never {
  rollback.when(!info.failureBitSet, "expected failed-emission callback", 73);
  requireCachedObject(TransactionType.AccountSet);
  accept("callback observed EmitFailure", 203);
}
)[test.tshook]");

    auto const *providerPath = std::getenv("XAHAU_QJS_PROVIDER_WASM");
    auto const *artifactPath = std::getenv("XAHAU_QJS_OTXN_OBJECT_XQJS");
    auto const *bytecodePath = std::getenv("XAHAU_QJS_OTXN_OBJECT_QJSC");
    auto const required =
        std::getenv("XAHAU_REQUIRE_QJS_PROVIDER_TESTS") != nullptr;
    if (!providerPath || !artifactPath || !bytecodePath) {
      testcase("Current working-tree artifacts absent");
      BEAST_EXPECT(!required);
      return;
    }

    auto provider = readBinary(providerPath);
    auto artifactBytes = readBinary(artifactPath);
    auto bytecode = readBinary(bytecodePath);
    BEAST_EXPECT(!provider.empty());
    BEAST_EXPECT(!artifactBytes.empty());
    BEAST_EXPECT(!bytecode.empty());
    if (provider.empty() || artifactBytes.empty() || bytecode.empty())
      return;

    testcase("Load current packaged Hook bytes");
    auto const artifact = hook::artifact::parse(makeSlice(artifactBytes));
    BEAST_EXPECT(!!artifact);
    if (!artifact)
      return;
    BEAST_EXPECT(artifact->payload.size() == bytecode.size());
    BEAST_EXPECT(std::equal(artifact->payload.begin(), artifact->payload.end(),
                            bytecode.begin(), bytecode.end()));

    auto runtime = hook::findQuickJSRuntime(*artifact);
    if (!runtime) {
      auto const providerError =
          hook::setQuickJSProviderForTests(std::move(provider));
      BEAST_EXPECTS(!providerError, providerError.value_or(""));
      if (providerError)
        return;
      runtime = hook::findQuickJSRuntime(*artifact);
    }
    BEAST_EXPECT(!!runtime);
    if (!runtime)
      return;

    auto const validation =
        hook::validateQuickJSBytecodeForTests(runtime, bytecode);
    BEAST_EXPECT(!validation.error);
    BEAST_EXPECT(!validation.hasCallback);
    if (validation.error)
      return;
    auto const callbackValidation =
        hook::validateQuickJSBytecodeForTests(runtime, callbackBytecode);
    BEAST_EXPECT(!callbackValidation.error);
    BEAST_EXPECT(callbackValidation.hasCallback);
    if (callbackValidation.error)
      return;

    auto const alice = Account{"alice"};
    auto const bob = Account{"bob"};
    auto const carol = Account{"carol"};
    auto const features = supported_amendments();
    Env env{*this, features | featureJSHooks};
    env.fund(XRP(10000), alice, bob);
    env.close();

    testcase("Observe one cold acquisition and warm local access");
    auto const payment =
        env.jt(pay(bob, alice, XRP(1)), fee(XRP(100)), ter(tesSUCCESS));
    auto const serializedSize = payment.stx->getSerializer().getDataLength();
    std::vector<hook::quickjs::QuickJSHostCallObservationForTests> observations;
    {
      OpenView view{*env.current()};
      ApplyContext applyContext{env.app(),
                                view,
                                *payment.stx,
                                tesSUCCESS,
                                env.current()->fees().base,
                                tapNONE,
                                env.journal};
      hook::HookStateMap stateMap;
      auto hookContext =
          makeStubHookContext(applyContext, alice.id(), bob.id(), {}, stateMap);
      hook::quickjs::setQuickJSHostCallObservationsForTests(&observations);
      scope_exit stopObserving{[] {
        hook::quickjs::setQuickJSHostCallObservationsForTests(nullptr);
      }};
      hook::executeQuickJSBytecode(hookContext, runtime, bytecode, false, 0,
                                   env.journal);
      BEAST_EXPECT(hookContext.result.exitType == hook_api::ExitType::ACCEPT);
      BEAST_EXPECT(hookContext.slot.empty());
    }

    std::array<std::string_view, 9> const expectedCalls{
        "otxn_type", "otxn_slot",  "slot_size",    "slot_clear", "otxn_slot",
        "slot",      "slot_clear", "hook_account", "accept"};
    BEAST_EXPECT(observations.size() == expectedCalls.size());
    if (observations.size() != expectedCalls.size()) {
      log << "observed otxn.object calls:";
      for (auto const &observation : observations)
        log << ' ' << observation.name;
      log << '\n';
      return;
    }

    std::uint64_t acquisitionCost = 0;
    for (std::size_t index = 0; index < observations.size(); ++index) {
      auto const &observation = observations[index];
      BEAST_EXPECT(observation.invocation == 1);
      BEAST_EXPECT(observation.name == expectedCalls[index]);
      BEAST_EXPECT(observation.dispatched);
      BEAST_EXPECT(observation.hostWorkBefore - observation.hostWorkAfter ==
                   observation.cost);
      if (index > 0)
        BEAST_EXPECT(observation.hostWorkBefore ==
                     observations[index - 1].hostWorkAfter);
      if (index >= 1 && index <= 6)
        acquisitionCost += observation.cost;
    }
    BEAST_EXPECT(observations[1].liveSlots == 1);
    BEAST_EXPECT(observations[1].liveSlotSerializedBytes == serializedSize);
    BEAST_EXPECT(observations[2].liveSlots == 1);
    BEAST_EXPECT(observations[3].liveSlots == 0);
    BEAST_EXPECT(observations[4].liveSlots == 1);
    BEAST_EXPECT(observations[5].declaredBytes == serializedSize);
    BEAST_EXPECT(observations[5].cost == serializedSize + 1);
    BEAST_EXPECT(observations[5].liveSlots == 1);
    BEAST_EXPECT(observations[6].liveSlots == 0);
    BEAST_EXPECT(acquisitionCost == serializedSize + 6);

    testcase("Run packaged incoming-XAH policy through Env");
    Env policyEnv{*this, features | featureJSHooks};
    policyEnv.fund(XRP(10000), alice, bob, carol);
    policyEnv.close();
    IOU const USD{carol["USD"]};
    policyEnv(trust(alice, USD(1000)), ter(tesSUCCESS));
    policyEnv.close();

    auto policyHook = hsoVersioned(artifactBytes, 1);
    policyHook[jss::Flags] = hsfOVERRIDE;
    policyEnv(jtx::hook(alice, {{policyHook}}, 0), fee(XRP(10)),
              ter(tesSUCCESS));
    policyEnv.close();

    auto const expectPolicyResult = [&](hook_api::ExitType expectedResult,
                                        std::uint64_t expectedCode,
                                        std::string_view expectedMessage) {
      auto const metadata = policyEnv.meta();
      BEAST_EXPECT(!!metadata);
      if (!metadata || !metadata->isFieldPresent(sfHookExecutions))
        return;
      auto const executions = metadata->getFieldArray(sfHookExecutions);
      BEAST_EXPECT(executions.size() == 1);
      if (executions.size() != 1)
        return;
      auto const &execution = executions[0];
      BEAST_EXPECT(execution.getFieldU8(sfHookResult) ==
                   static_cast<std::uint8_t>(expectedResult));
      BEAST_EXPECT(execution.getFieldU64(sfHookReturnCode) == expectedCode);
      auto const message = execution.getFieldVL(sfHookReturnString);
      BEAST_EXPECT(std::string(message.begin(), message.end()) ==
                   expectedMessage);
    };

    policyEnv(noop(alice), fee(XRP(100)), ter(tesSUCCESS));
    expectPolicyResult(hook_api::ExitType::ACCEPT, 0,
                       "not an incoming Payment");
    policyEnv.close();

    policyEnv(pay(bob, alice, XRP(1)), fee(XRP(100)), ter(tesSUCCESS));
    expectPolicyResult(hook_api::ExitType::ACCEPT, 0,
                       "incoming native XAH accepted");
    policyEnv.close();

    policyEnv(pay(alice, bob, XRP(1)), fee(XRP(100)), ter(tecHOOK_REJECTED));
    expectPolicyResult(hook_api::ExitType::ROLLBACK, 20,
                       "Payment is not addressed to this Hook account");
    policyEnv.close();

    policyEnv(pay(carol, alice, USD(1)), fee(XRP(100)), ter(tecHOOK_REJECTED));
    expectPolicyResult(hook_api::ExitType::ROLLBACK, 22,
                       "Payment Amount must be native XAH");
    policyEnv.close();

    policyEnv(pay(bob, alice, drops(100'000'001)), fee(XRP(100)),
              ter(tecHOOK_REJECTED));
    expectPolicyResult(hook_api::ExitType::ROLLBACK, 23,
                       "Payment Amount is outside the accepted range");
    policyEnv.close();

    testcase("Release live slot owners after uncatchable traps");
    auto const proveOwnerRelease = [&](std::uint64_t budget,
                                       std::string_view trappedOperation) {
      std::vector<hook::quickjs::QuickJSHostCallObservationForTests>
          trappedObservations;
      std::weak_ptr<STObject const> trappedOwner;
      {
        OpenView view{*env.current()};
        ApplyContext applyContext{env.app(),
                                  view,
                                  *payment.stx,
                                  tesSUCCESS,
                                  env.current()->fees().base,
                                  tapNONE,
                                  env.journal};
        hook::HookStateMap stateMap;
        hook::quickjs::setQuickJSHostCallObservationsForTests(
            &trappedObservations);
        hook::quickjs::setQuickJSHostWorkBudgetForTests(budget);
        scope_exit resetTestSeams{[] {
          hook::quickjs::setQuickJSHostWorkBudgetForTests(std::nullopt);
          hook::quickjs::setQuickJSHostCallObservationsForTests(nullptr);
        }};
        {
          auto hookContext = makeStubHookContext(applyContext, alice.id(),
                                                 bob.id(), {}, stateMap);
          hook::executeQuickJSBytecode(hookContext, runtime, bytecode, false, 0,
                                       env.journal);
          BEAST_EXPECT(hookContext.result.exitType ==
                       hook_api::ExitType::WASM_ERROR);
          auto const trapped = std::find_if(
              trappedObservations.begin(), trappedObservations.end(),
              [](auto const &observation) { return !observation.dispatched; });
          BEAST_EXPECT(trapped != trappedObservations.end());
          if (trapped != trappedObservations.end()) {
            BEAST_EXPECT(trapped->name == trappedOperation);
            BEAST_EXPECT(trapped->liveSlots == 1);
            BEAST_EXPECT(trapped->liveSlotSerializedBytes == serializedSize);
            trappedOwner = trapped->liveSlotOwner;
            BEAST_EXPECT(!trappedOwner.expired());
          }
        }
        BEAST_EXPECT(trappedOwner.expired());
      }
      BEAST_EXPECT(trappedOwner.expired());
    };

    // otxn.type and otxn_slot consume one unit each; slot_size traps while
    // the measurement slot is live.
    proveOwnerRelease(2, "slot_size");
    // The final clear traps after the copy cost, while the copy slot lives.
    proveOwnerRelease(serializedSize + 6, "slot_clear");

    testcase("Acquire an accepted maximal SetHook-chain transaction");
    BEAST_EXPECT(largeTransactionWasm.size() == hook::maxHookWasmSize());
    if (largeTransactionWasm.size() != hook::maxHookWasmSize())
      return;

    STTx const largeTransaction{
        ttHOOK_SET, [&](STObject &object) {
          object.setAccountID(sfAccount, alice.id());
          object.setFieldU32(sfSequence, 1);
          object.setFieldAmount(sfFee, XRPAmount{10});
          object.setFieldVL(sfSigningPubKey, Blob{});
          STArray hooks{sfHooks};
          for (std::size_t hookIndex = 0;
               hookIndex < hook::maxHookChainLength(); ++hookIndex) {
            STObject hookObject{sfHook};
            hookObject.setFieldVL(sfCreateCode, largeTransactionWasm);
            uint256 hookNamespace;
            hookNamespace.data()[31] = static_cast<std::uint8_t>(hookIndex + 1);
            hookObject.setFieldH256(sfHookNamespace, hookNamespace);

            STArray hookParameters{sfHookParameters};
            for (std::size_t parameterIndex = 0; parameterIndex < 16;
                 ++parameterIndex) {
              STObject parameter{sfHookParameter};
              Blob name(32, static_cast<std::uint8_t>('n'));
              name[30] = static_cast<std::uint8_t>(hookIndex);
              name[31] = static_cast<std::uint8_t>(parameterIndex);
              parameter.setFieldVL(sfHookParameterName, name);
              parameter.setFieldVL(sfHookParameterValue, Blob(256, 0x5aU));
              hookParameters.emplace_back(std::move(parameter));
            }
            hookObject.setFieldArray(sfHookParameters,
                                     std::move(hookParameters));

            STArray hookGrants{sfHookGrants};
            for (std::size_t grantIndex = 0; grantIndex < 8; ++grantIndex) {
              STObject grant{sfHookGrant};
              uint256 grantHash;
              grantHash.data()[30] = static_cast<std::uint8_t>(hookIndex);
              grantHash.data()[31] = static_cast<std::uint8_t>(grantIndex + 1);
              grant.setFieldH256(sfHookHash, grantHash);
              grant.setAccountID(sfAuthorize, bob.id());
              grant.setFieldU32(sfFlags, 0);
              hookGrants.emplace_back(std::move(grant));
            }
            hookObject.setFieldArray(sfHookGrants, std::move(hookGrants));

            uint256 outgoing;
            outgoing.data()[31] = static_cast<std::uint8_t>(hookIndex + 1);
            uint256 incoming = outgoing;
            incoming.data()[30] = 1;
            hookObject.setFieldH256(sfHookOnOutgoing, outgoing);
            hookObject.setFieldH256(sfHookOnIncoming, incoming);
            hookObject.setFieldH256(sfHookCanEmit, uint256{beast::zero});
            hookObject.setFieldU16(sfHookApiVersion, 0);
            Blob hookName(16, static_cast<std::uint8_t>('h'));
            hookName[15] = static_cast<std::uint8_t>(
                static_cast<unsigned char>('a') + hookIndex);
            hookObject.setFieldVL(sfHookName, hookName);
            hookObject.setFieldU32(sfFlags, hsfOVERRIDE | hsfCOLLECT);
            hooks.emplace_back(std::move(hookObject));
          }
          object.setFieldArray(sfHooks, std::move(hooks));
        }};
    auto const largeSerializedSize =
        largeTransaction.getSerializer().getDataLength();
    BEAST_EXPECT(largeSerializedSize == 709'953);
    BEAST_EXPECT(largeSerializedSize < txMaxSizeBytes);
    std::string largeTransactionReason;
    BEAST_EXPECT(passesLocalChecks(largeTransaction, largeTransactionReason));
    auto const largePreflight =
        preflight(env.app(), env.current()->rules(), largeTransaction,
                  tapDRY_RUN, env.journal);
    BEAST_EXPECT(largePreflight.ter == tesSUCCESS);
    log << "accepted large SetHook transaction bytes: " << largeSerializedSize
        << '\n';

    std::vector<hook::quickjs::QuickJSHostCallObservationForTests>
        largeObservations;
    {
      OpenView view{*env.current()};
      ApplyContext applyContext{env.app(),
                                view,
                                largeTransaction,
                                tesSUCCESS,
                                env.current()->fees().base,
                                tapNONE,
                                env.journal};
      hook::HookStateMap stateMap;
      auto hookContext =
          makeStubHookContext(applyContext, alice.id(), bob.id(), {}, stateMap);
      hook::quickjs::setQuickJSHostCallObservationsForTests(&largeObservations);
      scope_exit stopObserving{[] {
        hook::quickjs::setQuickJSHostCallObservationsForTests(nullptr);
      }};
      hook::executeQuickJSBytecode(hookContext, runtime, callbackBytecode,
                                   false, 0, env.journal);
      BEAST_EXPECT(hookContext.result.exitType == hook_api::ExitType::ROLLBACK);
      BEAST_EXPECT(hookContext.slot.empty());
    }
    std::array<std::string_view, 7> const expectedLargeCalls{
        "otxn_slot", "slot_size",  "slot_clear", "otxn_slot",
        "slot",      "slot_clear", "rollback"};
    BEAST_EXPECT(largeObservations.size() == expectedLargeCalls.size());
    if (largeObservations.size() == expectedLargeCalls.size()) {
      std::uint64_t largeAcquisitionCost = 0;
      for (std::size_t index = 0; index < expectedLargeCalls.size(); ++index) {
        BEAST_EXPECT(largeObservations[index].name ==
                     expectedLargeCalls[index]);
        BEAST_EXPECT(largeObservations[index].dispatched);
        if (index < 6)
          largeAcquisitionCost += largeObservations[index].cost;
      }
      BEAST_EXPECT(largeObservations[4].declaredBytes == largeSerializedSize);
      BEAST_EXPECT(largeObservations[4].cost == largeSerializedSize + 1);
      BEAST_EXPECT(largeAcquisitionCost == largeSerializedSize + 6);
    }

    testcase("Select EmitFailure and isolate invocation caches");
    auto const accountSet = env.jt(noop(bob), fee(XRP(10)), ter(tesSUCCESS));
    auto const accountSetSize = accountSet.stx->getSerializer().getDataLength();
    std::vector<hook::quickjs::QuickJSHostCallObservationForTests>
        callbackObservations;
    hook::quickjs::setQuickJSHostCallObservationsForTests(
        &callbackObservations);
    scope_exit stopCallbackObserving{
        [] { hook::quickjs::setQuickJSHostCallObservationsForTests(nullptr); }};

    {
      OpenView view{*env.current()};
      ApplyContext applyContext{env.app(),
                                view,
                                *payment.stx,
                                tesSUCCESS,
                                env.current()->fees().base,
                                tapNONE,
                                env.journal};
      hook::HookStateMap stateMap;
      auto hookContext =
          makeStubHookContext(applyContext, alice.id(), bob.id(), {}, stateMap);
      hook::executeQuickJSBytecode(hookContext, runtime, callbackBytecode,
                                   false, 0, env.journal);
      BEAST_EXPECT(hookContext.result.exitType == hook_api::ExitType::ACCEPT);
      BEAST_EXPECT(hookContext.result.exitCode == 201);
      BEAST_EXPECT(hookContext.slot.empty());
    }

    {
      OpenView view{*env.current()};
      ApplyContext applyContext{env.app(),
                                view,
                                *payment.stx,
                                tesSUCCESS,
                                env.current()->fees().base,
                                tapNONE,
                                env.journal};
      hook::HookStateMap stateMap;
      StubHookContext callbackStub;
      callbackStub.emitFailure = static_cast<STObject const &>(*accountSet.stx);
      auto hookContext = makeStubHookContext(applyContext, alice.id(), bob.id(),
                                             callbackStub, stateMap);
      hook::executeQuickJSBytecode(hookContext, runtime, callbackBytecode, true,
                                   1, env.journal);
      BEAST_EXPECT(hookContext.result.exitType == hook_api::ExitType::ACCEPT);
      BEAST_EXPECT(hookContext.result.exitCode == 203);
      BEAST_EXPECT(hookContext.slot.empty());
    }

    std::array<std::string_view, 7> const expectedCallbackCalls{
        "otxn_slot", "slot_size",  "slot_clear", "otxn_slot",
        "slot",      "slot_clear", "accept"};
    BEAST_EXPECT(callbackObservations.size() ==
                 expectedCallbackCalls.size() * 2);
    if (callbackObservations.size() == expectedCallbackCalls.size() * 2) {
      for (std::size_t invocation = 0; invocation < 2; ++invocation) {
        auto const expectedSize =
            invocation == 0 ? serializedSize : accountSetSize;
        std::uint64_t acquisitionCost = 0;
        for (std::size_t call = 0; call < expectedCallbackCalls.size();
             ++call) {
          auto const &observation =
              callbackObservations[invocation * expectedCallbackCalls.size() +
                                   call];
          BEAST_EXPECT(observation.invocation == invocation + 1);
          BEAST_EXPECT(observation.name == expectedCallbackCalls[call]);
          BEAST_EXPECT(observation.dispatched);
          if (call < 6)
            acquisitionCost += observation.cost;
        }
        auto const &copy =
            callbackObservations[invocation * expectedCallbackCalls.size() + 4];
        BEAST_EXPECT(copy.declaredBytes == expectedSize);
        BEAST_EXPECT(copy.liveSlotSerializedBytes == expectedSize);
        BEAST_EXPECT(acquisitionCost == expectedSize + 6);
      }
    }
  }
};

BEAST_DEFINE_TESTSUITE(JSHookzOtxnObjectBoundary, app, ripple);

} // namespace ripple::test
