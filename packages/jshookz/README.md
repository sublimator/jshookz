# jshookz

Compiler, deployment-envelope, runtime-profile, and Python Wasmtime tooling
for deterministic JavaScript/TypeScript Xahau Hooks.

The wheel includes two TypeScript declarations:

- `xahau-quickjs-v1.d.ts` is the exact surface implemented by the sealed v1
  provider and is the compiler default.
- `hooks-api.d.ts` is the broader, aspirational public API specification.

Provider binaries are deliberately not embedded in the wheel. Set
`JSHOOKZ_PROVIDER_WASM` or pass `--wasm`, or build the sealed provider from a
source checkout with `jshookz build provider`.

The project is pre-release and the v1 profile is not activated on a production
network.
