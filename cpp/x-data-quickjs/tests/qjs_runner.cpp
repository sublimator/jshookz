// Native test driver for the x-data-quickjs library. Not a product.

#include <jshookz/quickjs.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

extern "C" void register_protocol_functions(JSContext* ctx);

static std::string
read_file(char const* path)
{
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "error: cannot open %s\n", path);
        std::exit(2);
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

static void
print_exception(JSContext* ctx)
{
    JSValue exc = JS_GetException(ctx);
    char const* str = JS_ToCString(ctx, exc);
    std::fprintf(stderr, "[error] %s\n", str ? str : "exception");
    if (str)
        JS_FreeCString(ctx, str);
    JS_FreeValue(ctx, exc);
}

int
main(int argc, char** argv)
{
    char const* script = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--script" && i + 1 < argc)
            script = argv[++i];
    }
    if (!script) {
        std::fprintf(stderr, "usage: %s --script <path.js>\n", argv[0]);
        return 2;
    }

    JSRuntime* rt = JS_NewRuntime();
    if (!rt) {
        std::fprintf(stderr, "error: JS_NewRuntime failed\n");
        return 2;
    }
    JSContext* ctx = JS_NewContext(rt);
    if (!ctx) {
        std::fprintf(stderr, "error: JS_NewContext failed\n");
        JS_FreeRuntime(rt);
        return 2;
    }
    register_protocol_functions(ctx);
    if (JS_HasException(ctx)) {
        print_exception(ctx);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 1;
    }

    std::string code = read_file(script);
    JSValue val = JS_Eval(
        ctx, code.data(), code.size(), script, JS_EVAL_TYPE_GLOBAL);
    int rc = 0;
    if (JS_IsException(val)) {
        print_exception(ctx);
        rc = 1;
    } else if (!JS_IsUndefined(val)) {
        char const* str = JS_ToCString(ctx, val);
        if (str) {
            std::printf("[result] %s\n", str);
            JS_FreeCString(ctx, str);
        }
    }
    JS_FreeValue(ctx, val);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return rc;
}
