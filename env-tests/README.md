# Xahau Env tests

In-tree `*_test.cpp` files compiled into `rippled` via

```sh
cmake -S . -B build -DHOOKS_TEST_DIR=/path/to/jshookz/env-tests
```

or `HOOKS_TEST_DIR` in the environment. `hookz build-test-hooks` must be on
`PATH`. Coverage is not enabled from this directory.

Optional:

- `HOOKS_SOURCE_DIR` — `name=/path` pairs for `file:` hook sources
- `HOOKS_FORCE_RECOMPILE=1` — skip the hookz cache
- `HOOKS_TEST_ONLY=1` — drop xahaud's own `*_test.cpp` files, keep this dir
- `XAHAU_TEST_LOG=HooksTrace=trace` — per-partition test logs (`TestEnv`)

`TESTENV_LOGGING` is still accepted as an alias for `XAHAU_TEST_LOG`.
