# Local QuickJS patches

This directory vendors QuickJS `2025-09-13`. The table is the forward resync
register for changes maintained locally after that source import. Before
replacing vendored engine files, replay or deliberately retire every row and
run the named proof.

| Product commit | Local delta | Why it remains local | Primary proof |
| --- | --- | --- | --- |
| `b8d2222` | Fully designated `JSCFunctionListEntry` macros | Make the public macros valid ISO C++23 for native consumers. | Native C++ provider builds. |
| `dc18d5b` | Exact object byte-ingress helpers | Provider object decoding needs trap-aware own-property operations without JavaScript reflection wrappers. | `cpp/quickjs/tests/test_object_helpers.cpp`. |
| `b991f30`, `796a020` | Standalone-Wasm allocator accounting and optional resource probes | Exact requested-size accounting and non-product heap acceptance gates are specific to the sealed provider build. | `cpp/quickjs/tests/wasm_allocator_probe.c` and Python resource gates. |
| `89a158f` | Public non-trapping `JS_IsProxy` identity probe | `defineHookConfig` must reject proxies before any reflection can invoke a user trap; QuickJS's proxy class ID is otherwise private to `quickjs.c`. | `python/jshookz/tests/test_hook_validation.py`. |

Git history before this register remains the authority for any older local
delta not represented above; add a row whenever a future commit changes the
vendored engine rather than allowing another implicit patch to accumulate.
