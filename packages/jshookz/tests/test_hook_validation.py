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


def test_accepts_callable_main_without_invoking_it():
    bytecode = compile_bytecode(
        'export function main() { throw new Error("entry ran"); }'
    )

    result = validate(bytecode)

    assert result.valid
    assert not result.has_callback
    assert result.error is None


def test_callable_callback_sets_callback_bit():
    bytecode = compile_bytecode(
        "export function main() {}\nexport function callback() {}"
    )

    result = validate(bytecode)

    assert result.valid
    assert result.has_callback


def test_rejects_missing_main_export():
    bytecode = compile_bytecode("export function other() {}")

    result = validate(bytecode)

    assert not result.valid
    assert result.error == "TypeError: Hook module has no exported main entry point"


def test_rejects_non_callable_main_export():
    bytecode = compile_bytecode("export const main = 1;")

    result = validate(bytecode)

    assert not result.valid
    assert result.error == "TypeError: exported main entry point is not a function"


def test_rejects_non_callable_callback_export():
    bytecode = compile_bytecode(
        "export function main() {}\nexport const callback = 2;"
    )

    result = validate(bytecode)

    assert not result.valid
    assert result.error == "TypeError: exported callback entry point is not a function"


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
        "export let main = 1;\nmain = function initializedMain() {};"
    )

    result = validate(bytecode)

    assert result.valid
    assert not result.has_callback


def test_rejects_throwing_module_initialization():
    bytecode = compile_bytecode(
        'export function main() {}\nthrow new Error("top-level failed");'
    )

    result = validate(bytecode)

    assert not result.valid
    assert result.error == "Error: top-level failed"


def test_rejects_pending_module_initialization():
    bytecode = compile_bytecode(
        "export function main() {}\nawait Promise.resolve();"
    )

    result = validate(bytecode)

    assert not result.valid
    assert result.error == (
        "TypeError: pending module initialization is not supported"
    )


def test_rejects_host_calls_during_module_initialization():
    bytecode = compile_bytecode(
        "export function main() {}\nvoid ledger.sequence;"
    )

    result = validate(bytecode)

    assert not result.valid
    assert "unavailable during module initialization" in result.error


def test_v1_bytes_accepts_typed_arrays_array_buffer_and_hex():
    result = evaluate(
        "JSON.stringify(["
        "STBlob.from(new Uint16Array([258])).toHex(),"
        "STBlob.from(new Uint8Array(new Uint8Array([0,160,255,0]).buffer,1,2)).toHex(),"
        "STBlob.from(new ArrayBuffer(2)).byteLength,"
        "STBlob.fromHex('A0FF').toHex(),"
        "STBlob.fromHex('').byteLength"
        "]);"
    )

    assert result.exit_code == 0
    assert result.result_value == '["0201","A0FF",2,"A0FF",0]'


def test_v1_bytes_accepts_strict_byte_arrays_and_rejects_ambiguous_inputs():
    result = evaluate(
        "JSON.stringify(["
        "STBlob.from([1, 2, 255]).toHex(),"
        "(() => { try { STBlob.from([256]); return false; } catch { return true; } })(),"
        "(() => { try { STBlob.from([1.5]); return false; } catch { return true; } })(),"
        "(() => { try { STBlob.from(new DataView(new ArrayBuffer(2))); "
        "return false; } catch { return true; } })(),"
        "(() => { try { STBlob.from('A0'); return false; } catch { return true; } })(),"
        "(() => { try { STBlob.fromHex('ABC'); return false; } catch { return true; } })(),"
        "(() => { try { STBlob.fromHex('GG'); return false; } catch { return true; } })()"
        "]);"
    )

    assert result.exit_code == 0
    assert result.result_value == '["0102FF",true,true,true,true,true,true]'


def test_v1_bytes_rejects_typed_arrays_omitted_from_bytes_like():
    result = evaluate(
        "JSON.stringify(["
        "(() => { try { STBlob.from(new Float16Array([1])); return false; } "
        "catch (error) { return error instanceof TypeError; } })()"
        "]);"
    )

    assert result.exit_code == 0
    assert result.result_value == "[true]"


def test_stblob_equals_accepts_the_declared_stblob_input():
    result = evaluate(
        "JSON.stringify(["
        "STBlob.fromHex('A0FF').equals(STBlob.fromHex('A0FF')) ,"
        "STBlob.fromHex('A0FF').equals(STBlob.fromHex('A000'))"
        "]);"
    )

    assert result.exit_code == 0
    assert result.result_value == "[true,false]"


def test_account_id_protocol_constants_have_exact_read_only_values():
    result = evaluate(
        "JSON.stringify(["
        "AccountID.zero.toHex(),"
        "AccountID.one.toHex(),"
        "Object.getOwnPropertyDescriptor(AccountID, 'zero').writable,"
        "Object.getOwnPropertyDescriptor(AccountID, 'one').writable"
        "]);"
    )

    assert result.exit_code == 0
    assert result.result_value == (
        '["' + "00" * 20 + '","' + "00" * 19 + '01",false,false]'
    )


def test_native_factories_and_prototypes_are_frozen_at_registration():
    result = evaluate(
        "JSON.stringify(["
        "[STBlob, STBlob.from([])],"
        "[Hash256, Hash256.from(new Uint8Array(32))],"
        "[AccountID, AccountID.zero],"
        "[XFL, XFL.fromRaw(0n)],"
        "[UInt8, UInt8.zero]"
        "].every(([factory, value]) => "
        "Object.isFrozen(factory) && Object.isFrozen(Object.getPrototypeOf(value))));"
    )

    assert result.exit_code == 0
    assert result.result_value == "true"
