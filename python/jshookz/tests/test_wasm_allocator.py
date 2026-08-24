"""Direct gate for the sealed provider's standalone QuickJS allocator."""

from __future__ import annotations

import subprocess

import wasmtime

from jshookz.paths import REPO_ROOT


def test_wasm_allocator_exact_accounting_and_realloc_contract():
    build = REPO_ROOT / "build" / "xahau-provider"
    subprocess.run(
        [
            "cmake",
            "--build",
            str(build),
            "--target",
            "quickjs_wasm_allocator_probe",
        ],
        check=True,
    )
    engine = wasmtime.Engine()
    module = wasmtime.Module.from_file(
        engine, build / "quickjs_wasm_allocator_probe.wasm"
    )
    store = wasmtime.Store(engine)
    instance = wasmtime.Instance(store, module, [])
    probe = instance.exports(store)["quickjs_wasm_allocator_probe"]

    assert probe(store) == 0
