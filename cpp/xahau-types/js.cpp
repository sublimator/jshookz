#include "js.hpp"

namespace jshookz::provider::types {
namespace qjs = jshookz::provider::qjs;

bool
registerClass(
    JSContext* ctx,
    JSValueConst global,
    char const* name,
    JSClassID* class_id,
    JSClassDef const* class_def,
    std::span<JSCFunctionListEntry const> prototypeFunctions,
    std::span<JSCFunctionListEntry const> staticFunctions,
    qjs::ByteClassFamily byteFamily,
    JSCFunction* toBytes,
    FactoryInitializer initializeFactory)
{
    JS_NewClassID(class_id);
    if (JS_NewClass(JS_GetRuntime(ctx), *class_id, class_def) < 0 ||
        !qjs::registerByteClass(*class_id, byteFamily, toBytes))
        return false;

    qjs::OwnedValue prototype(ctx, JS_NewObject(ctx));
    if (prototype.isException() ||
        !qjs::installFunctions(ctx, prototype.get(), prototypeFunctions) ||
        !qjs::freezeObject(ctx, prototype.get()))
        return false;
    JS_SetClassProto(ctx, *class_id, prototype.release());

    qjs::OwnedValue factory(ctx, JS_NewObject(ctx));
    if (factory.isException() ||
        !qjs::installFunctions(ctx, factory.get(), staticFunctions) ||
        (initializeFactory != nullptr &&
         !initializeFactory(ctx, factory.get())) ||
        !qjs::freezeObject(ctx, factory.get()))
        return false;
    return JS_SetPropertyStr(ctx, global, name, factory.release()) >= 0;
}

}  // namespace jshookz::provider::types

extern "C" bool
register_cpp_types(JSContext* ctx)
{
    namespace types = jshookz::provider::types;
    namespace qjs = jshookz::provider::qjs;

    qjs::resetByteClassRegistry();
    qjs::OwnedValue global(ctx, JS_GetGlobalObject(ctx));
    if (global.isException())
        return false;
    return types::registerSTBlob(ctx, global.get()) &&
        types::registerHash256(ctx, global.get()) &&
        types::registerAccountID(ctx, global.get()) &&
        types::registerXFL(ctx, global.get());
}
