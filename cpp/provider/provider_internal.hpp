#pragma once

#include <cstdint>

#include <quickjs.h>

extern "C" {
[[nodiscard]] bool register_cpp_types(JSContext* ctx);
void unregister_cpp_types(JSRuntime* runtime);
[[nodiscard]] bool register_uint_types(JSContext* ctx);
}

namespace jshookz::provider {

struct ActiveXFLArithmeticProfile
{
    bool active;
    std::uint32_t code;
};

class InvocationXFLArithmeticProfile
{
    JSContext* context_ = nullptr;

public:
    InvocationXFLArithmeticProfile() = default;
    ~InvocationXFLArithmeticProfile();

    InvocationXFLArithmeticProfile(
        InvocationXFLArithmeticProfile const&) = delete;
    InvocationXFLArithmeticProfile& operator=(
        InvocationXFLArithmeticProfile const&) = delete;

    [[nodiscard]] bool activate(JSContext* context, std::uint32_t code) noexcept;
};

[[nodiscard]] JSValue makeSTBlob(
    JSContext* ctx,
    std::uint8_t const* bytes,
    std::uint32_t length);
[[nodiscard]] JSValue makeHash256(
    JSContext* ctx,
    std::uint8_t const* bytes,
    std::uint32_t length);
[[nodiscard]] JSValue makeAccountID(
    JSContext* ctx,
    std::uint8_t const* bytes,
    std::uint32_t length);

[[nodiscard]] bool registerBindings(JSContext* ctx);
[[nodiscard]] bool installDeterministicSandbox(JSContext* ctx);
[[nodiscard]] bool registerXFLProfile(JSContext* ctx);
void destroyXFLProfile(JSContext* ctx) noexcept;
[[nodiscard]] int observeModuleXFLProfile(
    JSContext* ctx,
    JSValueConst moduleNamespace,
    std::uint32_t* code);
[[nodiscard]] ActiveXFLArithmeticProfile
activeXFLArithmeticProfile(JSContext* ctx) noexcept;
void setRandomSeed(std::uint32_t seed) noexcept;
void setCoverageEnabled(JSRuntime* runtime, JSContext* context, bool enabled);

}  // namespace jshookz::provider
