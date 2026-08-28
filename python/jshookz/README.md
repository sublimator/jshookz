# jshookz

Compiler, deployment-envelope, runtime-profile, and Python Wasmtime tooling
for deterministic JavaScript/TypeScript Xahau Hooks.

The wheel includes two TypeScript declarations:

- `xahau-quickjs-v1.d.ts` is the exact surface implemented by the sealed v1
  provider and is the compiler default.
- `hooks-api.d.ts` is the broader, aspirational public API specification. The
  `xahau-quickjs-v1.surface.json` drives the provider conformance probe, and
  `api-artifacts.json` closes and hashes the three shipped API artifacts.

Provider binaries are deliberately not embedded in the wheel. Set
`JSHOOKZ_PROVIDER_WASM` or pass `--wasm`, or build the sealed provider from a
source checkout with `jshookz build provider`.

The runtime profile names the native consensus-engine candidate exactly as
`wasmtime-native-c-api` 47.0.3. This Python package independently pins
`wasmtime` 47.0.1 as a non-consensus behavioral oracle; a green Python run is
not a claim that the two engine patch releases are byte-identical.

The project is pre-release and the v1 profile is not activated on a production
network.

The production compiler accepts TypeScript and JavaScript files. A TypeScript
entry may use static relative imports for local helpers, constants, and types.
The complete graph is checked with pinned TypeScript 6.0.3, emitted, and bundled
with pinned esbuild 0.28.2 into one import-free ESM before qjsc compilation.
Bare packages, dynamic `import()`, and `import.meta` are rejected; there is no
runtime module loader. Run `npm ci` beside this package to install both exact
frontend tools.

JavaScript is not an unchecked escape and remains deliberately single-file:
module imports are rejected, then the source runs through TypeScript `checkJs`
and the same typed Result ownership/dataflow gate as TypeScript. Every emitted
module is initialized and its live `main`/optional `callback` exports validated
by the provider before `compile_hook` returns; host operations are unavailable
during that initialization. Low-level `WasmHost.compile_source` remains a
diagnostic primitive, not a deployability check.

`compile-hook` and `package-hook` accept `--emit-js` for the final bundle and
`--emit-source-map` for an optional composed map back to authoring TypeScript
that contributes generated code. Erased type-only modules have no generated
segment and therefore no source-map entry. The map is diagnostic only: it is
never included in qjsc bytecode or the single-payload XQJS artifact.
