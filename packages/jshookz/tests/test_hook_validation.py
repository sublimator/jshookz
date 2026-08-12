from jshookz.host import WasmHost
from jshookz.paths import XAHAU_HOOK_PROVIDER_WASM


def evaluate(source: str):
    host = WasmHost(wasm_path=XAHAU_HOOK_PROVIDER_WASM)
    host.init()
    try:
        return host.eval(source)
    finally:
        host.destroy()


def compile_bytecode(source: str, *, module: bool = True) -> bytes:
    host = WasmHost(wasm_path=XAHAU_HOOK_PROVIDER_WASM)
    host.init()
    try:
        return host.compile_source(source, module=module)
    finally:
        host.destroy()


def validate(bytecode: bytes):
    host = WasmHost(wasm_path=XAHAU_HOOK_PROVIDER_WASM)
    host.init()
    try:
        return host.validate_hook_bytecode(bytecode)
    finally:
        host.destroy()


def test_accepts_callable_hook_without_invoking_it():
    bytecode = compile_bytecode(
        'export function hook() { throw new Error("entry ran"); }'
    )

    result = validate(bytecode)

    assert result.valid
    assert not result.has_callback
    assert result.error is None


def test_callable_callback_sets_callback_bit():
    bytecode = compile_bytecode(
        "export function hook() {}\nexport function cbak() {}"
    )

    result = validate(bytecode)

    assert result.valid
    assert result.has_callback


def test_rejects_missing_hook_export():
    bytecode = compile_bytecode("export function other() {}")

    result = validate(bytecode)

    assert not result.valid
    assert result.error == "TypeError: Hook module has no exported hook entry point"


def test_rejects_non_callable_hook_export():
    bytecode = compile_bytecode("export const hook = 1;")

    result = validate(bytecode)

    assert not result.valid
    assert result.error == "TypeError: exported hook entry point is not a function"


def test_rejects_non_callable_callback_export():
    bytecode = compile_bytecode(
        "export function hook() {}\nexport const cbak = 2;"
    )

    result = validate(bytecode)

    assert not result.valid
    assert result.error == "TypeError: exported cbak entry point is not a function"


def test_rejects_non_module_bytecode():
    bytecode = compile_bytecode("1 + 1", module=False)

    result = validate(bytecode)

    assert not result.valid
    assert result.error == "TypeError: Hook bytecode must contain an ES module"


def test_rejects_malformed_bytecode():
    result = validate(b"not quickjs bytecode")

    assert not result.valid
    assert result.error


def test_uses_initialized_export_value_not_only_declared_name():
    bytecode = compile_bytecode(
        "export let hook = 1;\nhook = function initializedHook() {};"
    )

    result = validate(bytecode)

    assert result.valid
    assert not result.has_callback


def test_rejects_throwing_module_initialization():
    bytecode = compile_bytecode(
        'export function hook() {}\nthrow new Error("top-level failed");'
    )

    result = validate(bytecode)

    assert not result.valid
    assert result.error == "Error: top-level failed"


def test_rejects_pending_module_initialization():
    bytecode = compile_bytecode(
        "export function hook() {}\nawait Promise.resolve();"
    )

    result = validate(bytecode)

    assert not result.valid
    assert result.error == (
        "TypeError: pending module initialization is not supported"
    )


def test_rejects_host_calls_during_module_initialization():
    bytecode = compile_bytecode(
        "export function hook() {}\nvoid ledger.sequence;"
    )

    result = validate(bytecode)

    assert not result.valid
    assert "unavailable during module initialization" in result.error


def test_v1_bytes_accepts_typed_arrays_array_buffer_and_hex():
    result = evaluate(
        "JSON.stringify(["
        "STBlob.from(new Uint16Array([258])).toHex(),"
        "STBlob.from(new ArrayBuffer(2)).byteLength,"
        "STBlob.from('A0FF').toHex()"
        "]);"
    )

    assert result.exit_code == 0
    assert result.result_value == '["0201",2,"A0FF"]'


def test_v1_bytes_rejects_plain_arrays_and_data_view():
    result = evaluate(
        "JSON.stringify(["
        "(() => { try { STBlob.from([1, 2]); return false; } catch { return true; } })(),"
        "(() => { try { STBlob.from(new DataView(new ArrayBuffer(2))); "
        "return false; } catch { return true; } })()"
        "]);"
    )

    assert result.exit_code == 0
    assert result.result_value == "[true,true]"
