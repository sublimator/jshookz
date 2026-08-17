from jshookz.host import WasmHost
from jshookz.paths import XAHAU_HOOK_PROVIDER_WASM


def test_sealed_provider_is_wizered():
    assert XAHAU_HOOK_PROVIDER_WASM.is_file()
    host = WasmHost(wasm_path=XAHAU_HOOK_PROVIDER_WASM)
    exports = host.instance.exports(host.store)
    assert exports["_initialize"] is not None
    try:
        exports["wizer.initialize"]
    except KeyError:
        pass
    else:
        raise AssertionError("wizer.initialize must not remain on the sealed wasm")
    try:
        host.init()
        result = host.eval("1 + 1")
    finally:
        host.destroy()
    assert result.ok
    assert result.result_value == "2"
