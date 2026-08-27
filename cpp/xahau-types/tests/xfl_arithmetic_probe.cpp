#include "js.hpp"
#include "result.hpp"
#include "xfl/xfl.hpp"
#include "xfl/xfl_arithmetic.hpp"
#include "xfl/xfl_profile_context.hpp"

#include "runtime_profile_limits.h"

#include <jshookz/qjs.hpp>

#include <array>
#include <bit>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string_view>

extern "C" bool register_cpp_types(JSContext* context);

namespace {

using hook::XFL;

bool
parseRaw(char const* text, std::uint64_t& value) noexcept
{
    char const* end = text + std::strlen(text);
    auto const result = std::from_chars(text, end, value);
    return result.ec == std::errc{} && result.ptr == end;
}

void
printResult(hook::XFLArithmeticResult const& result)
{
    if (!result.ok()) {
        constexpr std::array<char const*, 4> issues{
            nullptr,
            "overflow",
            "division-by-zero",
            "invalid",
        };
        static_assert(
            issues.size() ==
            static_cast<std::size_t>(hook::XFLArithmeticIssue::count));
        std::size_t const issue = static_cast<std::size_t>(result.issue);
        if (issue == 0 || issue >= issues.size())
            std::cout << "error:unmapped\n";
        else
            std::cout << "error:" << issues[issue] << '\n';
        return;
    }
    std::cout << static_cast<std::uint64_t>(result.value.raw()) << '\n';
}

int
runNative(std::string_view operation, std::uint64_t left, std::uint64_t right)
{
    XFL const a(std::bit_cast<std::int64_t>(left));
    XFL const b(std::bit_cast<std::int64_t>(right));
    if (operation == "add")
        printResult(hook::addXahauFloatV1(a, b));
    else if (operation == "subtract")
        printResult(hook::subtractXahauFloatV1(a, b));
    else if (operation == "multiply")
        printResult(hook::multiplyXahauFloatV1(a, b));
    else if (operation == "divide")
        printResult(hook::divideXahauFloatV1(a, b));
    else
        return 2;
    return 0;
}

JSValue
makeDecimal(JSContext* context, std::uint64_t raw)
{
    XFL const value(std::bit_cast<std::int64_t>(raw));
    return jshookz::provider::types::makeXFLDecimalParts(
        context,
        value.is_negative(),
        value.mantissa(),
        value.exponent());
}

bool
install(JSContext* context, char const* name, JSValue value)
{
    jshookz::qjs::OwnedValue owned(context, value);
    jshookz::qjs::OwnedValue global(context, JS_GetGlobalObject(context));
    return !owned.isException() && !global.isException() &&
        JS_SetPropertyStr(context, global.get(), name, owned.release()) >= 0;
}

int
runQuickJS(std::string_view operation, std::uint64_t left, std::uint64_t right)
{
    JSRuntime* runtime = JS_NewRuntime();
    if (runtime == nullptr)
        return 3;
    JSContext* context = JS_NewContext(runtime);
    if (context == nullptr) {
        JS_FreeRuntime(runtime);
        return 3;
    }

    int status = 3;
    if (!jshookz::provider::bindings::registerResult(context) ||
        !register_cpp_types(context))
        goto cleanup;

    {
        jshookz::provider::types::XFLProfileContext profile{
            true,
            catl::xdata::xahau_profile::xfl_arithmetic_profile_xahau_float_v1,
        };
        JS_SetContextOpaque(context, &profile);
        if (!install(context, "left", makeDecimal(context, left)) ||
            !install(context, "right", makeDecimal(context, right))) {
            JS_SetContextOpaque(context, nullptr);
            goto cleanup;
        }

        char const* expression = operation == "add"
            ? "left.add(right)"
            : operation == "subtract"
            ? "left.subtract(right)"
            : operation == "multiply"
            ? "left.multiply(right)"
            : operation == "divide" ? "left.divide(right)" : nullptr;
        if (expression == nullptr) {
            JS_SetContextOpaque(context, nullptr);
            status = 2;
            goto cleanup;
        }
        jshookz::qjs::OwnedValue result(
            context,
            JS_Eval(
                context,
                expression,
                std::strlen(expression),
                "<xfl-probe>",
                JS_EVAL_TYPE_GLOBAL));
        JS_SetContextOpaque(context, nullptr);
        if (result.isException())
            goto cleanup;

        jshookz::qjs::OwnedValue ok(
            context, JS_GetPropertyStr(context, result.get(), "ok"));
        if (ok.isException() || !JS_IsBool(ok.get()))
            goto cleanup;
        if (!JS_ToBool(context, ok.get())) {
            jshookz::qjs::OwnedValue error(
                context, JS_GetPropertyStr(context, result.get(), "error"));
            jshookz::qjs::OwnedValue issue(
                context, JS_GetPropertyStr(context, error.get(), "issue"));
            char const* text = JS_ToCString(context, issue.get());
            if (text == nullptr)
                goto cleanup;
            std::cout << "error:" << text << '\n';
            JS_FreeCString(context, text);
            status = 0;
            goto cleanup;
        }

        jshookz::qjs::OwnedValue value(
            context, JS_GetPropertyStr(context, result.get(), "value"));
        bool negative = false;
        std::uint64_t magnitude = 0;
        std::int32_t exponent = 0;
        if (!jshookz::provider::types::readXFLDecimalParts(
                context,
                value.get(),
                &negative,
                &magnitude,
                &exponent))
            goto cleanup;
        std::uint64_t const raw = magnitude == 0
            ? 0
            : static_cast<std::uint64_t>(
                  XFL::from_components(
                      negative,
                      exponent,
                      static_cast<std::int64_t>(magnitude))
                      .raw());
        std::cout << raw << '\n';
        status = 0;
    }

cleanup:
    JS_SetContextOpaque(context, nullptr);
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    return status;
}

}  // namespace

int
main(int argc, char** argv)
{
    if (argc != 5)
        return 2;
    std::uint64_t left = 0;
    std::uint64_t right = 0;
    if (!parseRaw(argv[3], left) || !parseRaw(argv[4], right))
        return 2;
    std::string_view const mode(argv[1]);
    std::string_view const operation(argv[2]);
    if (mode == "native")
        return runNative(operation, left, right);
    if (mode == "quickjs")
        return runQuickJS(operation, left, right);
    return 2;
}
