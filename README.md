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

## Examples

### Accept

<!-- BEGIN GENERATED: xahau-accept.hook.ts -->
```ts
export function main(): never {
  accept("hello from TypeScript", 42);
}
```
<!-- END GENERATED: xahau-accept.hook.ts -->

### Read and write state

<!-- BEGIN GENERATED: xahau-state.hook.ts -->
```ts
export function main(): never {
  const previous = rollback.onFail(state.get("GREETING"), "state read failed");

  if (previous === undefined) {
    rollback.onFail(
      state.set("GREETING", new Uint8Array([72, 105])),
      "state write failed",
    );
  }

  const greeting = rollback.require(
    state.get("GREETING"),
    "state disappeared",
    1,
  );
  accept(greeting.toBytes(), 0);
}
```
<!-- END GENERATED: xahau-state.hook.ts -->

### Require a batch of host calls

<!-- BEGIN GENERATED: xahau-state-batch.hook.ts -->
```ts
export function main(): never {
  rollback.onAnyFail(
    [
      state.set("FIRST", new Uint8Array([1])),
      state.set("SECOND", new Uint8Array([2])),
    ],
    "batch write failed",
  );

  const first = rollback.require(
    state.get("FIRST"),
    "first value disappeared",
    1,
  );
  accept(`first=${first.byteAt(0)}`, 0);
}
```
<!-- END GENERATED: xahau-state-batch.hook.ts -->

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
surface is generated from the canonical API and checked in CI:
[`xahau-quickjs-v1.d.ts`](packages/jshookz/src/jshookz/types/xahau-quickjs-v1.d.ts).

The broader [`hooks-api.d.ts`](packages/jshookz/src/jshookz/types/hooks-api.d.ts)
is the API we are growing toward, not a claim that everything is implemented
in v1.

QuickJS lives under `engine/quickjs`, the Wasm provider under
`runtime/provider`, and the Xahau integration inputs under
`integrations/xahau`.

See [CONTRIBUTING.md](CONTRIBUTING.md) for the full build and
[VENDOR.md](VENDOR.md) for provenance.
