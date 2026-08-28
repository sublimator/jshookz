# Xahau Env tests

In-tree `*_test.cpp` files compiled into `rippled` via

```sh
cmake -S . -B build -DHOOKS_TEST_DIR=/path/to/jshookz/xahau/env-tests
```

or `HOOKS_TEST_DIR` in the environment. `hookz build-test-hooks` must be on
`PATH`. Coverage is not enabled from this directory.

Optional:

- `HOOKS_SOURCE_DIR` — `name=/path` pairs for `file:` hook sources
- `HOOKS_FORCE_RECOMPILE=1` — skip the hookz cache
- `HOOKS_TEST_ONLY=1` — drop xahaud's own `*_test.cpp` files, keep this dir
- `XAHAU_TEST_LOG=HooksTrace=trace` — per-partition test logs (`TestEnv`)

`TESTENV_LOGGING` is still accepted as an alias for `XAHAU_TEST_LOG`.

## Ownership boundary

Serious provider-specific native proofs live here, beside the provider that
owns their Hook sources, build pipeline, and expected behavior. They exercise
the current working-tree provider and Hook outputs; exact commits and artifact
identities belong in the completion receipt, not in test constants. This
includes exhaustive policy matrices, crossing and budget observations,
boundary-size witnesses, callback cases, and provider fault or lifetime probes.

The xahaud consumer branch retains only:

- production integration and admission behavior;
- one small permanent smoke test for the public boundary; and
- the minimum `ENABLE_TESTS`-only observation seams an external Env test needs
  to witness private native state.

Do not copy producer-owned Hook sources, generated fixture headers, dedicated
fixture generators, or exhaustive provider test cases into `src/test/` in
xahaud. Add an external `*_test.cpp` here and exercise it through
`HOOKS_TEST_DIR` instead. Generated headers and packaged artifacts remain
working-tree build outputs rather than checked fixture-level pins.
