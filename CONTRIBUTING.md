# Contributing

The root CI workflow is the executable clean-checkout specification. Keep
generated files, runtime identities, and tests synchronized in the same
change.

## Toolchain

- CMake and Ninja
- wasi-sdk 32
- Binaryen 128
- Boost headers
- Wasmtime C API 47.0.3
- Python 3.10+ and `uv`
- Node.js 22 and TypeScript 6.0.3
- OpenSSL development headers on non-macOS hosts

## Hook provider

```bash
uv sync --project packages/jshookz --locked --group dev
packages/jshookz/.venv/bin/jshookz build provider
packages/jshookz/.venv/bin/pytest -q packages/jshookz/tests
```

`jshookz build provider` verifies the checked runtime-profile lock and emits
the JSON/CMake manifests. A changed provider hash requires an intentional new
profile decision; do not update the lock merely to make the check pass.

## Raw Hook ABI and hostem

```bash
uv sync --project packages/hostem --locked --group dev
packages/hostem/.venv/bin/python \
  integrations/xahau/tools/generate_raw_hook_abi.py --check
tsc -p packages/hostem/tsconfig.xahau-integration.json
packages/hostem/.venv/bin/pytest -q packages/hostem/tests
```

Edit the policy or generator, not generated output. The source of truth is the
pinned public Xahau `hook_api.macro`, parsed by hookz.

## Codec and combined fixture

Set `WASI_SDK_PATH` and `BOOST_INCLUDE_DIR`, then:

```bash
cmake -S codec/xrpl -B build/codec -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$WASI_SDK_PATH/share/cmake/wasi-sdk.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DBOOST_INCLUDE_DIR="$BOOST_INCLUDE_DIR"
cmake --build build/codec

cmake -S codec/xrpl/fixture-provider -B build/codec-fixture -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$WASI_SDK_PATH/share/cmake/wasi-sdk.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DBOOST_INCLUDE_DIR="$BOOST_INCLUDE_DIR"
cmake --build build/codec-fixture
scripts/check-generated-definitions.sh
```

Configure `codec/xrpl/host` with either the Conan-generated Wasmtime CMake
package or `-DWASMTIME_ROOT` pointing at an official C-API archive, install
the locked differential dependency, then run the suite:

```bash
npm ci --prefix codec/xrpl/differential --ignore-scripts
uv sync --project codec/xrpl --locked
codec/xrpl/.venv/bin/pytest -q codec/xrpl/tests
scripts/check-wasm-stack.sh
```

The combined target is a test fixture, not a deployable Hook provider: it
retains generic embedding imports and its native host supplies WASI. Required
build artifacts fail tests when absent. Optional Wizer coverage may
skip when `wizer` is unavailable; other unexpected skips should be treated as
a broken release lane.

## Pull requests

- Keep unrelated work intact.
- Preserve ordinary negative Hook statuses as values.
- Never add WASI to the production profile.
- Include tests for result width, memory bounds, terminals, and deterministic
  behavior when changing an ABI boundary.
- Update `VENDOR.md` whenever vendored or generated-source provenance moves.
