from pathlib import Path

from jshookz.build import wizer_provider
from jshookz.host import WasmHost
from jshookz.paths import XAHAU_HOOK_PROVIDER_WASM


def test_wizer_is_a_nonvacuous_reproducible_wasm_to_wasm_step(tmp_path: Path):
    assert XAHAU_HOOK_PROVIDER_WASM.is_file()
    first = tmp_path / "a.wasm"
    second = tmp_path / "b.wasm"
    wizer_provider(XAHAU_HOOK_PROVIDER_WASM, first)
    wizer_provider(XAHAU_HOOK_PROVIDER_WASM, second)
    assert first.read_bytes() == second.read_bytes()
    assert first.stat().st_size > XAHAU_HOOK_PROVIDER_WASM.stat().st_size

    host = WasmHost(wasm_path=first)
    try:
        assert "_initialize" in host.instance.exports(host.store)
        host.init()
        result = host.eval("1 + 1")
    finally:
        host.destroy()
    assert result.ok
    assert result.result_value == "2"
