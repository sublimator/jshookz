#include "provider_internal.hpp"

#include "runtime_profile_limits.h"
#include "quickjs.hpp"
#include "xfl/xfl_profile_context.hpp"

#include <cstdint>

namespace jshookz::provider {
namespace {

using qjs::OwnedValue;
namespace profile = catl::xdata::xahau_profile;

struct HookConfigState
{
    std::uint32_t code;
};

HookConfigState const xahauFloatConfig{
    profile::xfl_arithmetic_profile_xahau_float_v1};
HookConfigState const nearestEvenConfig{
    profile::xfl_arithmetic_profile_nearest_even_v1};

JSClassID hookConfigClassId = 0;
JSClassDef const hookConfigClass{
    .class_name = "HookConfig",
};

bool
sameObject(JSValueConst left, JSValueConst right) noexcept
{
    return JS_IsObject(left) && JS_IsObject(right) &&
        JS_VALUE_GET_PTR(left) == JS_VALUE_GET_PTR(right);
}

JSValue
objectPrototype(JSContext* context)
{
    OwnedValue global(context, JS_GetGlobalObject(context));
    if (global.isException())
        return global.release();
    OwnedValue object(context, JS_GetPropertyStr(context, global.get(), "Object"));
    if (object.isException())
        return object.release();
    return JS_GetPropertyStr(context, object.get(), "prototype");
}

bool
supportedProfile(std::uint32_t code) noexcept
{
    return code == profile::xfl_arithmetic_profile_xahau_float_v1 ||
        code == profile::xfl_arithmetic_profile_nearest_even_v1;
}

HookConfigState const*
profileState(std::uint32_t code) noexcept
{
    if (code == profile::xfl_arithmetic_profile_xahau_float_v1)
        return &xahauFloatConfig;
    if (code == profile::xfl_arithmetic_profile_nearest_even_v1)
        return &nearestEvenConfig;
    return nullptr;
}

JSValue
configTypeError(JSContext* context)
{
    return JS_ThrowTypeError(
        context,
        "defineHookConfig: expected exactly { xflArithmetic: "
        "XFLProfile.xahauFloatV1 | XFLProfile.nearestEvenV1 }");
}

bool
readExactInputProfile(
    JSContext* context,
    JSValueConst input,
    std::uint32_t* code)
{
    if (!JS_IsObject(input) || JS_IsProxy(input))
        return false;

    OwnedValue actualPrototype(context, JS_GetPrototype(context, input));
    OwnedValue expectedPrototype(context, objectPrototype(context));
    if (actualPrototype.isException() || expectedPrototype.isException() ||
        !sameObject(actualPrototype.get(), expectedPrototype.get()))
        return false;

    JSPropertyEnum* properties = nullptr;
    std::uint32_t propertyCount = 0;
    if (JS_GetOwnPropertyNames(
            context,
            &properties,
            &propertyCount,
            input,
            JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK) < 0)
        return false;

    JSAtom const expectedAtom = JS_NewAtom(context, "xflArithmetic");
    bool const exactProperty = expectedAtom != JS_ATOM_NULL &&
        propertyCount == 1 && properties[0].atom == expectedAtom;
    JSPropertyDescriptor descriptor{
        .flags = 0,
        .value = JS_UNDEFINED,
        .getter = JS_UNDEFINED,
        .setter = JS_UNDEFINED,
    };
    int descriptorStatus = 0;
    if (exactProperty)
        descriptorStatus = JS_GetOwnProperty(
            context, &descriptor, input, expectedAtom);
    if (expectedAtom != JS_ATOM_NULL)
        JS_FreeAtom(context, expectedAtom);
    JS_FreePropertyEnum(context, properties, propertyCount);

    bool valid = exactProperty && descriptorStatus == 1 &&
        (descriptor.flags & JS_PROP_ENUMERABLE) != 0 &&
        JS_IsUndefined(descriptor.getter) &&
        JS_IsUndefined(descriptor.setter) && JS_IsNumber(descriptor.value);
    double numericProfile = 0;
    if (valid && JS_ToFloat64(context, &numericProfile, descriptor.value) < 0)
        valid = false;
    std::uint32_t observed = profile::xfl_arithmetic_profile_none;
    if (valid &&
        numericProfile ==
            static_cast<double>(profile::xfl_arithmetic_profile_xahau_float_v1))
        observed = profile::xfl_arithmetic_profile_xahau_float_v1;
    else if (
        valid && numericProfile == static_cast<double>(
                     profile::xfl_arithmetic_profile_nearest_even_v1))
        observed = profile::xfl_arithmetic_profile_nearest_even_v1;
    else
        valid = false;
    if (valid)
        valid = supportedProfile(observed);

    JS_FreeValue(context, descriptor.value);
    JS_FreeValue(context, descriptor.getter);
    JS_FreeValue(context, descriptor.setter);
    if (!valid)
        return false;
    *code = observed;
    return true;
}

JSValue
// @binding provider:defineHookConfig
defineHookConfig(
    JSContext* context,
    JSValueConst,
    int argc,
    JSValueConst* argv)
{
    std::uint32_t code = profile::xfl_arithmetic_profile_none;
    if (argc != 1 || !readExactInputProfile(context, argv[0], &code)) {
        if (JS_HasException(context))
            return JS_EXCEPTION;
        return configTypeError(context);
    }

    HookConfigState const* state = profileState(code);
    if (state == nullptr)
        return configTypeError(context);
    OwnedValue config(context, JS_NewObjectClass(context, hookConfigClassId));
    if (config.isException())
        return config.release();
    JS_SetOpaque(config.get(), const_cast<HookConfigState*>(state));
    if (JS_DefinePropertyValueStr(
            context,
            config.get(),
            "xflArithmetic",
            JS_NewUint32(context, code),
            JS_PROP_ENUMERABLE) < 0 ||
        !qjs::freezeObject(context, config.get()))
        return JS_EXCEPTION;
    return config.release();
}

bool
publishXFLProfile(JSContext* context, JSValueConst global)
{
    // @binding provider:XFLProfile.xahauFloatV1
    // @binding provider:XFLProfile.nearestEvenV1
    OwnedValue namespaceObject(context, JS_NewObject(context));
    if (namespaceObject.isException() ||
        JS_DefinePropertyValueStr(
            context,
            namespaceObject.get(),
            "xahauFloatV1",
            JS_NewUint32(
                context, profile::xfl_arithmetic_profile_xahau_float_v1),
            JS_PROP_ENUMERABLE) < 0 ||
        JS_DefinePropertyValueStr(
            context,
            namespaceObject.get(),
            "nearestEvenV1",
            JS_NewUint32(
                context, profile::xfl_arithmetic_profile_nearest_even_v1),
            JS_PROP_ENUMERABLE) < 0 ||
        !qjs::freezeObject(context, namespaceObject.get()))
        return false;
    return JS_DefinePropertyValueStr(
        context,
        global,
        "XFLProfile",
        namespaceObject.release(),
        JS_PROP_ENUMERABLE) >= 0;
}

bool
publishDefineHookConfig(JSContext* context, JSValueConst global)
{
    OwnedValue function(
        context,
        JS_NewCFunction(context, defineHookConfig, "defineHookConfig", 1));
    if (function.isException() || !qjs::freezeObject(context, function.get()))
        return false;
    return JS_DefinePropertyValueStr(
        context,
        global,
        "defineHookConfig",
        function.release(),
        JS_PROP_ENUMERABLE) >= 0;
}

} // namespace

bool
registerXFLProfile(JSContext* context)
{
    if (context == nullptr || JS_GetContextOpaque(context) != nullptr)
        return false;
    if (!::jshookz::qjs::defineClass(
            JS_GetRuntime(context), &hookConfigClassId, &hookConfigClass))
        return false;

    OwnedValue prototype(context, objectPrototype(context));
    if (prototype.isException())
        return false;
    JS_SetClassProto(context, hookConfigClassId, prototype.release());

    auto* state = static_cast<types::XFLProfileContext*>(
        js_mallocz(context, sizeof(types::XFLProfileContext)));
    if (state == nullptr)
        return false;
    state->active = false;
    state->code = profile::xfl_arithmetic_profile_none;
    JS_SetContextOpaque(context, state);

    OwnedValue global(context, JS_GetGlobalObject(context));
    if (global.isException() ||
        !publishXFLProfile(context, global.get()) ||
        !publishDefineHookConfig(context, global.get()))
        return false;
    return true;
}

void
destroyXFLProfile(JSContext* context) noexcept
{
    if (context == nullptr)
        return;
    auto* state = types::xflProfileContext(context);
    JS_SetContextOpaque(context, nullptr);
    if (state != nullptr)
        js_free(context, state);
}

int
observeModuleXFLProfile(
    JSContext* context,
    JSValueConst moduleNamespace,
    std::uint32_t* code)
{
    if (context == nullptr || code == nullptr)
        return -1;
    OwnedValue config(
        context, JS_GetPropertyStr(context, moduleNamespace, "hookConfig"));
    if (config.isException())
        return -1;
    if (JS_IsUndefined(config.get())) {
        *code = profile::xfl_arithmetic_profile_none;
        return 0;
    }
    auto const* state = static_cast<HookConfigState const*>(
        JS_GetOpaque(config.get(), hookConfigClassId));
    if (state == nullptr || !supportedProfile(state->code)) {
        JS_ThrowTypeError(
            context,
            "exported hookConfig was not minted by defineHookConfig");
        return -1;
    }
    *code = state->code;
    return 0;
}

bool
InvocationXFLArithmeticProfile::activate(
    JSContext* context,
    std::uint32_t code) noexcept
{
    types::XFLProfileContext* state = types::xflProfileContext(context);
    if (context_ != nullptr || state == nullptr || state->active ||
        (code != profile::xfl_arithmetic_profile_none &&
         !supportedProfile(code)))
        return false;
    state->active = true;
    state->code = code;
    context_ = context;
    return true;
}

InvocationXFLArithmeticProfile::~InvocationXFLArithmeticProfile()
{
    if (context_ == nullptr)
        return;
    types::XFLProfileContext* state = types::xflProfileContext(context_);
    if (state != nullptr) {
        state->active = false;
        state->code = profile::xfl_arithmetic_profile_none;
    }
}

ActiveXFLArithmeticProfile
activeXFLArithmeticProfile(JSContext* context) noexcept
{
    return types::activeXFLArithmeticProfile(context);
}

} // namespace jshookz::provider
