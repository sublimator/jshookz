# Contributing

The root CI workflow is the executable clean-checkout specification. Keep
generated files, runtime identities, and tests synchronized in the same
change.

## Toolchain

- CMake and Ninja
- wasi-sdk 32
- Binaryen 128
- Boost headers (native x-data table probe)
- Python 3.10+ and `uv`
- Node.js 22 and TypeScript 6.0.3

## Hook provider

```bash
uv sync --project python/jshookz --locked --group dev
python/jshookz/.venv/bin/jshookz build provider
python/jshookz/.venv/bin/pytest -q python/jshookz/tests
```

`jshookz build provider` emits the runtime-profile lock and the JSON
manifest. Do not commit the lock; xahaud pins it. It also exports
`build/xahau-provider-bundle/`, the complete directory xahaud consumes: the
sealed wasm, the JSON provenance, the API artifacts, `jshookz_provider.receipt`
(one `key value` per line, every pin xahaud needs, readable by any CMake with
`file(STRINGS)`), and `jshookz_provider.values.cpp` (the profile constants
xahaud compiles as-is, pinned by the receipt). Point
`XAHAU_QUICKJS_PROVIDER_BUNDLE_DIR` at it for a local build, or
`jshookz export-bundle provider -o <xahaud>/external/quickjs-provider` to
move xahaud's committed pin.

## Pins

A runtime or provider change moves committed pins: the generated raw Hook
ABI, the provider identity snapshot, the runtime observation snapshot, and
the recursive fuel snapshot. Regenerate all of them in one step and commit
them with the change that moved them, never as a separate relock commit:

```bash
scripts/relock.sh          # rebuild, reseal, rewrite every pin, list what moved
scripts/relock.sh --check  # the same gates read-only, as CI sees them
```

The API artifact manifest is checked, not rewritten, because the
declarations it hashes come from the projection tool.

Check mode also re-exports each built product's consumer bundle into a
temporary directory under `build/` and diffs every file against
`build/xahau-<product>-bundle/`, so a stale or hand-edited bundle is a red
like any stale pin.

## Raw Hook ABI and hostem

```bash
uv sync --project python/hostem --locked --group dev
python/hostem/.venv/bin/python \
  xahau/tools/generate_raw_hook_abi.py --check
tsc -p python/hostem/tsconfig.xahau-integration.json
python/hostem/.venv/bin/pytest -q python/hostem/tests
```

Edit the policy or generator, not generated output. The source of truth is the
pinned public Xahau `hook_api.macro`, parsed by hookz.

## Host C++ (`cpp/xahau-types`, `cpp/x-data`)

`xahau-types` is host-blind: no `hook_*` / `host_*`. That is why it links
on Mac. Host crossings stay in `cpp/provider/bindings/`.

Generalize bindings only from repeated live consumers. Reuse the
`jshookz::qjs` toolkit and named `BytePolicy` paths, but keep Result variants,
UInt magic dispatch, simple leaves, owned slice views, and exotic aggregates
as distinct registration and lifetime shapes. There is deliberately no
universal native-class registrar.

```bash
conan profile detect --exist-ok
conan install cpp --output-folder=build/cpp --build=missing \
  -s compiler.cppstd=23
cmake -S cpp -B build/cpp \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/build/cpp/conan_toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/cpp
ctest --test-dir build/cpp --output-on-failure
python/jshookz/.venv/bin/pytest -q cpp/x-data/tests
scripts/check-generated-definitions.sh
scripts/check-wasm-stack.sh
```

Existing `build/*` trees cache absolute source paths: reconfigure after
a layout change.

## Pull requests

- Keep unrelated work intact.
- Preserve ordinary negative Hook statuses as values.
- Never add WASI to the production profile.
- Include tests for result width, memory bounds, terminals, and deterministic
  behavior when changing an ABI boundary.
- Update `vendor/README.md` whenever vendored or generated-source provenance moves.
