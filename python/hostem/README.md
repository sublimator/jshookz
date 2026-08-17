# hostem

`hostem` composes two runtime dependencies without making either one know its
consumer:

- `jshookz` owns QuickJS execution inside WASM.
- `hookz` owns raw Xahau Hook ABI semantics and execution evidence.
- `hostem` attaches the former's guest memory to the latter and translates a
  completed QuickJS evaluation into `hookz.runtime.HookResult`.

The workspace uses the sibling `jshookz` package editably and pins public
`hookz` commit `299a812d9badb5baf66da9fe73e7b1f740298822`. The executable
example is `examples/hello.hook.js`; it exercises several raw ABI shapes through
the generated projection of Xahau's `hook_api.macro` catalogue.

```bash
uv sync --project python/hostem --locked --group dev
python/hostem/.venv/bin/pytest -q python/hostem/tests
```

`hostem` is a fast semantic harness. Xahau's native `Env` remains authoritative
for ledger commit and rollback behavior.
