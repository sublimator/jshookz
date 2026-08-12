# jshookz

Compile QuickJS to WebAssembly so JavaScript Hooks can be metered. Translate
JavaScript calls to the existing host functions used by C Hooks.

```text
TypeScript / JavaScript
        ↓
QuickJS bytecode
        ↓
metered QuickJS-in-Wasm
        ↓
existing Xahau Hook host functions
```

The aim is simple: add JavaScript Hooks without building a second Hook API or
replacing C Hooks.

This is pre-release software. The integration works end to end in tests, but
the QuickJS runtime is not activated on a production network.

Wasmtime is the intended execution engine: its AOT support and performance are
well suited to the relatively large QuickJS Wasm provider. Xahau needs only a
small dual-runtime seam—engine-neutral guest memory and result handling plus a
Wasmtime dispatcher—while reusing the existing C Hook host implementations.

## Example

```ts
export function hook(_reserved: number): never {
  trace("ledger", ledger.sequence);
  lifecycle.accept("hello from JavaScript", 0);
}
```

## Build and test

Prerequisites: wasi-sdk 32, Binaryen 128, CMake, Ninja, Python 3.10+, `uv`,
Node.js, and TypeScript.

```bash
uv sync --project packages/jshookz --locked --group dev
packages/jshookz/.venv/bin/jshookz build provider
packages/jshookz/.venv/bin/pytest -q packages/jshookz/tests
```

Compile and package a Hook:

```bash
packages/jshookz/.venv/bin/jshookz compile-hook hook.ts -o hook.qjsc
packages/jshookz/.venv/bin/jshookz package-hook hook.ts \
  --profile integrations/xahau/profiles/xahau-quickjs-v1.lock.json \
  -o hook.xqjs
```

## Current scope

The v1 runtime exposes 13 existing Hook host functions, including ledger
reads, tracing, state, emission, accept, and rollback. Its exact TypeScript
surface is
[`xahau-quickjs-v1.d.ts`](packages/jshookz/src/jshookz/types/xahau-quickjs-v1.d.ts).

The broader [`hooks-api.d.ts`](packages/jshookz/src/jshookz/types/hooks-api.d.ts)
is the API we are growing toward, not a claim that everything is implemented
in v1.

QuickJS lives under `engine/quickjs`, the Wasm provider under
`runtime/provider`, and the Xahau integration inputs under
`integrations/xahau`.

See [CONTRIBUTING.md](CONTRIBUTING.md) for the full build and
[VENDOR.md](VENDOR.md) for provenance.
