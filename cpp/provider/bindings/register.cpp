#include "common.hpp"

namespace jshookz::provider {

bool
registerBindings(JSContext *ctx)
{
    qjs::OwnedValue global(ctx, JS_GetGlobalObject(ctx));
    if (global.isException())
        return false;
    return bindings::registerResult(ctx) &&
        bindings::registerHook(ctx, global.get()) &&
        bindings::registerControl(ctx, global.get()) &&
        bindings::registerLedger(ctx, global.get()) &&
        bindings::registerState(ctx, global.get()) &&
        bindings::registerEmission(ctx, global.get()) &&
#ifdef CONFIG_XAHAU_CONSENSUS_ENTROPY_PROVIDER
        bindings::registerEntropy(ctx, global.get()) &&
#endif
        bindings::registerTrace(ctx, global.get()) &&
        bindings::registerLegacy(ctx, global.get());
}

}  // namespace jshookz::provider
