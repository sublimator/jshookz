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

`jshookz build provider` verifies the checked runtime-profile lock and emits
the JSON/CMake manifests. A changed provider hash requires an intentional new
profile decision; do not update the lock merely to make the check pass.

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

## x-data-quickjs (native)

```bash
uv sync --project cpp/x-data-quickjs --locked
cpp/x-data-quickjs/.venv/bin/pytest -q cpp/x-data-quickjs/tests
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
- Update `VENDOR.md` whenever vendored or generated-source provenance moves.
