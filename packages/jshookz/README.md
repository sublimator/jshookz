# jshookz

Compiler, deployment-envelope, runtime-profile, and Python Wasmtime tooling
for deterministic JavaScript/TypeScript Xahau Hooks.

The wheel includes two TypeScript declarations:

- `xahau-quickjs-v1.d.ts` is the exact surface implemented by the sealed v1
  provider and is the compiler default. Private `@publish-v1` annotations in
  the canonical declaration are its sole membership authority; selected
  signatures are copied without profile-side rewrites.
- `hooks-api.d.ts` is the broader, aspirational public API specification. The
  generated `xahau-quickjs-v1.surface.json` drives the provider conformance
  probe, and the checked `projection-receipt.json` binds all three public
  projections to the private canonical source without publishing the private
  annotations themselves.

Provider binaries are deliberately not embedded in the wheel. Set
`JSHOOKZ_PROVIDER_WASM` or pass `--wasm`, or build the sealed provider from a
source checkout with `jshookz build provider`.

The project is pre-release and the v1 profile is not activated on a production
network.
