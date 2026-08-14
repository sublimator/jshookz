#include "common.hpp"

namespace jshookz::provider {

void
registerBindings(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    bindings::registerLifecycle(ctx, global);
    bindings::registerControl(ctx, global);
    bindings::registerLedger(ctx, global);
    bindings::registerState(ctx, global);
    bindings::registerEmission(ctx, global);
    bindings::registerTrace(ctx, global);
    bindings::registerLegacy(ctx, global);
    JS_FreeValue(ctx, global);
}

}  // namespace jshookz::provider
