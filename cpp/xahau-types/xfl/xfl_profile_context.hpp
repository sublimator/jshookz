#pragma once

#include "runtime_profile_limits.h"

#include <quickjs.h>

#include <cstdint>

namespace jshookz::provider::types {

struct XFLProfileContext
{
    bool active = false;
    std::uint32_t code =
        catl::xdata::xahau_profile::xfl_arithmetic_profile_none;
};

struct ActiveXFLArithmeticProfile
{
    bool active;
    std::uint32_t code;
};

[[nodiscard]] inline XFLProfileContext*
xflProfileContext(JSContext* context) noexcept
{
    return context == nullptr
        ? nullptr
        : static_cast<XFLProfileContext*>(JS_GetContextOpaque(context));
}

[[nodiscard]] inline ActiveXFLArithmeticProfile
activeXFLArithmeticProfile(JSContext* context) noexcept
{
    XFLProfileContext const* state = xflProfileContext(context);
    if (state == nullptr)
        return {
            false,
            catl::xdata::xahau_profile::xfl_arithmetic_profile_none,
        };
    return {state->active, state->code};
}

}  // namespace jshookz::provider::types
