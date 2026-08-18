#include "account/account.hpp"
#include "js.hpp"

namespace jshookz::provider::types {
namespace qjs = jshookz::provider::qjs;
using hook::AccountID;

namespace {

JSClassID js_accountid_class_id;

void
js_accountid_finalizer(JSRuntime* rt, JSValue val)
{
    qjs::destroyOpaque<AccountID>(rt, val, js_accountid_class_id);
}

JSClassDef js_accountid_class = {
    .class_name = "AccountID",
    .finalizer = js_accountid_finalizer,
};

// @binding provider:AccountID.from
JSValue
js_accountid_from(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "AccountID.from() expects a byte value");
    auto bytes = qjs::ByteView::getBinding(
        ctx, argv[0], "AccountID.from", 0, qjs::BytePolicy::bytesLike);
    if (!bytes) {
        return qjs::byteInputTypeError(
            ctx, "AccountID.from()", qjs::BytePolicy::bytesLike);
    }
    if (bytes.size() != 20) {
        return JS_ThrowTypeError(
            ctx,
            "AccountID.from() needs exactly 20 bytes (got %u)",
            bytes.size());
    }
    return nativeNew<AccountID>(
        ctx, js_accountid_class_id, bytes.data(), bytes.size());
}

// @binding provider:AccountID.fromHex
JSValue
js_accountid_from_hex(
    JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(
            ctx, "AccountID.fromHex() expects a hex string");
    auto bytes = qjs::ByteView::getBinding(
        ctx, argv[0], "AccountID.fromHex", 0, qjs::BytePolicy::hexString);
    if (!bytes)
        return qjs::byteInputTypeError(
            ctx, "AccountID.fromHex()", qjs::BytePolicy::hexString);
    if (bytes.size() != 20)
        return JS_ThrowTypeError(
            ctx,
            "AccountID.fromHex() needs exactly 20 bytes (got %u)",
            bytes.size());
    return nativeNew<AccountID>(
        ctx, js_accountid_class_id, bytes.data(), bytes.size());
}

// @binding provider:AccountID.toHex
JSValue
js_accountid_to_hex(JSContext* ctx, JSValueConst this_val, int, JSValueConst*)
{
    auto* a = qjs::opaque<AccountID>(ctx, this_val, js_accountid_class_id);
    if (!a)
        return JS_EXCEPTION;
    char buf[41];
    a->to_hex(buf, sizeof(buf));
    buf[40] = '\0';
    return JS_NewString(ctx, buf);
}

// @binding provider:AccountID.toBytes
JSValue
js_accountid_to_bytes(
    JSContext* ctx, JSValueConst this_val, int, JSValueConst*)
{
    auto* a = qjs::opaque<AccountID>(ctx, this_val, js_accountid_class_id);
    if (!a)
        return JS_EXCEPTION;
    return qjs::uint8Array(
        ctx, std::span<std::uint8_t const>{a->data(), a->size()});
}

// @binding provider:AccountID.isZero
JSValue
js_accountid_is_zero(JSContext* ctx, JSValueConst this_val, int, JSValueConst*)
{
    auto* account =
        qjs::opaque<AccountID>(ctx, this_val, js_accountid_class_id);
    if (!account)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, account->is_zero());
}

// @binding provider:AccountID.equals
JSValue
js_accountid_equals(
    JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    auto* left =
        qjs::opaque<AccountID>(ctx, this_val, js_accountid_class_id);
    auto* right = qjs::opaque<AccountID>(
        ctx, argc > 0 ? argv[0] : JS_UNDEFINED, js_accountid_class_id);
    if (!left || !right)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, *left == *right);
}

//@@impl STAddress
JSCFunctionListEntry const proto[] = {
    JS_CFUNC_DEF("toHex", 0, js_accountid_to_hex),
    JS_CFUNC_DEF("toBytes", 0, js_accountid_to_bytes),
    JS_CFUNC_DEF("isZero", 0, js_accountid_is_zero),
    JS_CFUNC_DEF("equals", 1, js_accountid_equals),
};

//@@impl STAddress static
JSCFunctionListEntry const statics[] = {
    JS_CFUNC_DEF("from", 1, js_accountid_from),
    JS_CFUNC_DEF("fromHex", 1, js_accountid_from_hex),
};

// @binding provider:AccountID.zero
// @binding provider:AccountID.one
bool
install_account_constants(JSContext* ctx, JSValueConst factory)
{
    std::uint8_t zeroBytes[20] = {};
    std::uint8_t oneBytes[20] = {};
    oneBytes[19] = 1;
    qjs::OwnedValue zero(
        ctx,
        nativeNew<AccountID>(
            ctx, js_accountid_class_id, zeroBytes, sizeof(zeroBytes)));
    qjs::OwnedValue one(
        ctx,
        nativeNew<AccountID>(
            ctx, js_accountid_class_id, oneBytes, sizeof(oneBytes)));
    if (zero.isException() || one.isException())
        return false;
    if (!qjs::freezeObject(ctx, zero.get()) ||
        !qjs::freezeObject(ctx, one.get()))
        return false;
    return JS_DefinePropertyValueStr(
               ctx,
               factory,
               "zero",
               zero.release(),
               JS_PROP_ENUMERABLE) >= 0 &&
        JS_DefinePropertyValueStr(
               ctx, factory, "one", one.release(), JS_PROP_ENUMERABLE) >= 0;
}

}  // namespace

bool
registerAccountID(JSContext* ctx, JSValueConst global)
{
    return registerClass(
        ctx,
        global,
        "AccountID",
        &js_accountid_class_id,
        &js_accountid_class,
        proto,
        statics,
        qjs::ByteClassFamily::serializedType,
        js_accountid_to_bytes,
        install_account_constants);
}

JSValue
makeAccountIDBytes(
    JSContext* ctx, std::uint8_t const* bytes, std::uint32_t length)
{
    if (length != 20)
        return JS_ThrowInternalError(
            ctx, "AccountID construction requires 20 bytes");
    return nativeNew<AccountID>(ctx, js_accountid_class_id, bytes, length);
}

}  // namespace jshookz::provider::types

namespace jshookz::provider {

JSValue
makeAccountID(JSContext* ctx, std::uint8_t const* bytes, std::uint32_t length)
{
    return types::makeAccountIDBytes(ctx, bytes, length);
}

}  // namespace jshookz::provider
