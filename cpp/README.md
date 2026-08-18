# cpp/

Two CMake projects. Shared inventories are the `*_sources.cmake` files
(source lists and include dirs). Host makes libraries from them. The
provider compiles the same TUs into one wasm.

| | |
|---|---|
| `cmake -S cpp` | native gtests (Conan / gtest) |
| `cpp/provider` + wasi-sdk | sealed `jshookz_provider.wasm` |

Do not `add_subdirectory(provider)` from the host tree. Wrong toolchain.

- `quickjs/` — vendored C engine
- `quickjs-cpp/` — JSValue RAII (`jshookz_quickjs`, `jshookz_qjs_src`)
- `xahau-types/` — host-blind JS types; no `hook_*` / `host_*`
- `x-data/` — protocol tables; host-tested, not in the sealed wasm
- `provider/` — wasm entry, bindings, host crossings
