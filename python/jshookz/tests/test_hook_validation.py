import pytest

from jshookz.host import WasmHost
from jshookz.paths import XAHAU_HOOK_PROVIDER_WASM
from jshookz.xfl_profile import (
    XFLArithmeticProfile,
    decode_module_validation_result,
)


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


@pytest.mark.parametrize(
    ("word", "has_callback", "profile"),
    [
        (0x01000001, False, XFLArithmeticProfile.NONE),
        (0x01000003, True, XFLArithmeticProfile.NONE),
        (0x01000101, False, XFLArithmeticProfile.XAHAU_FLOAT_V1),
        (0x01000103, True, XFLArithmeticProfile.XAHAU_FLOAT_V1),
        (0x01000201, False, XFLArithmeticProfile.NEAREST_EVEN_V1),
        (0x01000203, True, XFLArithmeticProfile.NEAREST_EVEN_V1),
    ],
)
def test_module_validation_decoder_accepts_exact_legal_words(
    word: int,
    has_callback: bool,
    profile: XFLArithmeticProfile,
):
    result = decode_module_validation_result(word)

    assert result.has_callback is has_callback
    assert result.profile is profile


@pytest.mark.parametrize(
    "word",
    [
        -2,
        -1,
        0,
        1,
        3,
        0x00000001,
        0x02000001,
        0x01000000,
        0x01000002,
        0x01000005,
        0x01000041,
        0x01000301,
        0x0100FF01,
        0x01010001,
        0x017FFF01,
        0x81000001,
    ],
)
def test_module_validation_decoder_rejects_every_normative_malformed_word(word: int):
    with pytest.raises(ValueError):
        decode_module_validation_result(word)


@pytest.mark.parametrize(
    ("profile_name", "expected"),
    [
        ("xahauFloatV1", XFLArithmeticProfile.XAHAU_FLOAT_V1),
        ("nearestEvenV1", XFLArithmeticProfile.NEAREST_EVEN_V1),
    ],
)
def test_provider_validation_reports_minted_module_profile(
    profile_name: str,
    expected: XFLArithmeticProfile,
):
    bytecode = compile_bytecode(
        "export const hookConfig = defineHookConfig({"
        f"xflArithmetic: XFLProfile.{profile_name}"
        "}); export function main() {}"
    )

    result = validate(bytecode)

    assert result.valid
    assert result.profile is expected


def test_runtime_profile_namespace_and_config_are_frozen_and_literal_typed():
    result = evaluate(
        "JSON.stringify((() => {"
        "const one = defineHookConfig({xflArithmetic:XFLProfile.xahauFloatV1});"
        "const two = defineHookConfig({xflArithmetic:XFLProfile.nearestEvenV1});"
        "let constructed = false;"
        "try { new defineHookConfig({xflArithmetic:1}); } catch (error) {"
        "constructed = error instanceof TypeError; }"
        "return [XFLProfile.xahauFloatV1,XFLProfile.nearestEvenV1,"
        "Object.keys(XFLProfile),Object.isFrozen(XFLProfile),"
        "Object.isFrozen(defineHookConfig),Object.isFrozen(one),Object.isFrozen(two),"
        "Object.getPrototypeOf(one)===Object.prototype,"
        "Object.keys(one),one.xflArithmetic,two.xflArithmetic,constructed];"
        "})())"
    )

    assert result.exit_code == 0
    assert result.result_value == (
        '[1,2,["xahauFloatV1","nearestEvenV1"],true,true,true,true,true,'
        '["xflArithmetic"],1,2,true]'
    )


def test_xfl_profile_is_an_ordinary_namespace_not_a_runtime_classifier():
    result = evaluate(
        "JSON.stringify((()=>{"
        "let ordinaryTypeError=false;"
        "try{({}) instanceof XFLProfile}catch(error){"
        "ordinaryTypeError=error instanceof TypeError;}"
        "return [ordinaryTypeError,"
        "Object.prototype.hasOwnProperty.call(XFLProfile,Symbol.hasInstance),"
        "XFLProfile[Symbol.hasInstance]===undefined];"
        "})())"
    )

    assert result.exit_code == 0
    assert result.result_value == "[true,false,true]"


def test_enum_namespaces_are_frozen_ordinary_non_classifier_values():
    result = evaluate(
        "JSON.stringify((()=>{'use strict';"
        "const specs=[[TransactionType,'Payment',0,77],"
        "[TransactionResult,'telLOCAL_ERROR',-399,199],"
        "[HookReturnCode,'INVALID_FLOAT',-10024,46]];"
        "const failures=[];"
        "const fails=(label,operation)=>{try{operation();failures.push(label)}"
        "catch(error){if(!(error instanceof TypeError))failures.push(label+':type')}};"
        "for(const [value,member,literal,count] of specs){"
        "const keys=Reflect.ownKeys(value);"
        "if(typeof value!=='object'||Object.getPrototypeOf(value)!==Object.prototype)"
        "failures.push(member+':ordinary');"
        "if(!Object.isFrozen(value)||Object.isExtensible(value))"
        "failures.push(member+':frozen');"
        "if(keys.length!==count||value[member]!==literal)"
        "failures.push(member+':inventory');"
        "if(Object.hasOwn(value,'prototype')||"
        "Object.hasOwn(value,Symbol.hasInstance)||"
        "value[Symbol.hasInstance]!==undefined)failures.push(member+':classifier');"
        "fails(member+':assign',()=>{value[member]=123});"
        "fails(member+':define',()=>Object.defineProperty(value,member,{value:123}));"
        "fails(member+':delete',()=>{delete value[member]});"
        "fails(member+':prototype',()=>Object.setPrototypeOf(value,null));"
        "fails(member+':call',()=>Reflect.apply(value,null,[]));"
        "fails(member+':construct',()=>Reflect.construct(Object,[],value));"
        "fails(member+':instanceof',()=>void({} instanceof value));"
        "if(value[member]!==literal)failures.push(member+':mutated');"
        "}"
        "if(Object.hasOwn(TransactionType,'Invalid'))failures.push('Invalid');"
        "return failures})())"
    )

    assert result.exit_code == 0
    assert result.result_value == "[]"


def test_define_hook_config_rejects_traps_accessors_and_non_exact_shapes():
    result = evaluate(
        "JSON.stringify(["
        "undefined,null,{},"
        "{xflArithmetic:0},{xflArithmetic:1.5},{xflArithmetic:3},"
        "{xflArithmetic:1,extra:true},"
        "Object.create({xflArithmetic:1}),"
        "Object.create(null,{xflArithmetic:{value:1,enumerable:true}}),"
        "Object.defineProperty({},'xflArithmetic',{get(){return 1},enumerable:true}),"
        "new Proxy({xflArithmetic:1},{}),"
        "{xflArithmetic:1,[Symbol('extra')]:true}"
        "].map(value=>{try{defineHookConfig(value);return false}"
        "catch(error){return error instanceof TypeError}}));"
    )

    assert result.exit_code == 0
    assert result.result_value == (
        "[true,true,true,true,true,true,true,true,true,true,true,true]"
    )


@pytest.mark.parametrize(
    "config_source",
    [
        "{xflArithmetic:XFLProfile.xahauFloatV1}",
        "({...defineHookConfig({xflArithmetic:XFLProfile.xahauFloatV1})})",
        "new Proxy(defineHookConfig({xflArithmetic:XFLProfile.xahauFloatV1}),{})",
        "Object.create(defineHookConfig({xflArithmetic:XFLProfile.xahauFloatV1}))",
    ],
)
def test_provider_validation_rejects_every_structural_hook_config_forge(
    config_source: str,
):
    bytecode = compile_bytecode(
        f"export const hookConfig={config_source}; export function main() {{}}"
    )

    result = validate(bytecode)

    assert not result.valid
    assert result.error == (
        "TypeError: exported hookConfig was not minted by defineHookConfig"
    )


def test_post_definition_mutation_cannot_change_observed_profile():
    bytecode = compile_bytecode(
        "const minted=defineHookConfig({xflArithmetic:XFLProfile.xahauFloatV1});"
        "try { minted.xflArithmetic=XFLProfile.nearestEvenV1; } catch {}"
        "try { minted.extra=true; } catch {}"
        "export const hookConfig=minted; export function main(){return "
        "hookConfig.xflArithmetic}"
    )

    validation = validate(bytecode)

    assert validation.valid
    assert validation.profile is XFLArithmeticProfile.XAHAU_FLOAT_V1


def test_one_host_keeps_sequential_main_callback_and_failure_profiles_isolated():
    xahau_main = compile_bytecode(
        "export const hookConfig=defineHookConfig({xflArithmetic:"
        "XFLProfile.xahauFloatV1});export function main(){return "
        "hookConfig.xflArithmetic}"
    )
    nearest_callback = compile_bytecode(
        "export const hookConfig=defineHookConfig({xflArithmetic:"
        "XFLProfile.nearestEvenV1});export function main(){return -1}"
        "export function callback(){return hookConfig.xflArithmetic}"
    )
    throwing = compile_bytecode(
        "export const hookConfig=defineHookConfig({xflArithmetic:"
        "XFLProfile.xahauFloatV1});export function main(){throw new Error('A failed')}"
    )
    init_throwing = compile_bytecode(
        "export const hookConfig=defineHookConfig({xflArithmetic:"
        "XFLProfile.xahauFloatV1});export function main(){};throw new Error('init failed')"
    )

    host = WasmHost(wasm_path=XAHAU_HOOK_PROVIDER_WASM)
    host.init()
    try:
        assert host.run_hook_bytecode(xahau_main).result_value == "1"
        assert (
            host.run_hook_bytecode(nearest_callback, export="cbak").result_value
            == "2"
        )
        assert host.run_hook_bytecode(throwing).error == "Error: A failed"
        assert host.run_hook_bytecode(nearest_callback).result_value == "-1"
        assert host.run_hook_bytecode(init_throwing).error == "Error: init failed"
        assert host.validate_hook_bytecode(xahau_main).profile is (
            XFLArithmeticProfile.XAHAU_FLOAT_V1
        )
        assert (
            host.run_hook_bytecode(nearest_callback, export="cbak").result_value
            == "2"
        )
    finally:
        host.destroy()


def test_rejects_missing_main_export():
    bytecode = compile_bytecode("export function other() {}")

    result = validate(bytecode)

    assert not result.valid
    assert result.error == "TypeError: Hook module has no exported main entry point"


def test_rejects_non_callable_main_export():
    bytecode = compile_bytecode("export const main = 1;")

    result = validate(bytecode)

    assert not result.valid
    assert result.error == "TypeError: exported main entry point is not callable"


def test_rejects_non_callable_callback_export():
    bytecode = compile_bytecode("export function main() {}\nexport const callback = 2;")

    result = validate(bytecode)

    assert not result.valid
    assert result.error == "TypeError: exported callback entry point is not callable"


def test_rejects_class_entry_points_even_through_wrappers():
    for source in (
        "export class main {}",
        "class Entry {}\nexport const main = Entry.bind(null);",
        "class Entry {}\nexport const main = new Proxy(Entry, {});",
    ):
        result = validate(compile_bytecode(source))

        assert not result.valid
        assert result.error == "TypeError: exported main entry point is not callable"


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
    bytecode = compile_bytecode("export function main() {}\nawait Promise.resolve();")

    result = validate(bytecode)

    assert not result.valid
    assert result.error == ("TypeError: pending module initialization is not supported")


def test_rejects_host_calls_during_module_initialization():
    bytecode = compile_bytecode("export function main() {}\nvoid ledger.sequence;")

    result = validate(bytecode)

    assert not result.valid
    assert "unavailable during module initialization" in result.error


def test_v1_bytes_accepts_uint8_array_array_buffer_and_hex():
    result = evaluate(
        "JSON.stringify(["
        "STBlob.from(new Uint8Array(new Uint8Array([0,160,255,0]).buffer,1,2)).toHex(),"
        "STBlob.from(new ArrayBuffer(2)).byteLength,"
        "STBlob.fromHex('A0FF').toHex(),"
        "STBlob.fromHex('').byteLength"
        "]);"
    )

    assert result.exit_code == 0
    assert result.result_value == '["A0FF",2,"A0FF",0]'


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
        "(() => { try { STBlob.from(new Uint16Array([258])); return false; } "
        "catch (error) { return error instanceof TypeError; } })(),"
        "(() => { try { STBlob.from(new Float16Array([1])); return false; } "
        "catch (error) { return error instanceof TypeError; } })()"
        "]);"
    )

    assert result.exit_code == 0
    assert result.result_value == "[true,true]"


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


def test_legacy_native_factories_and_prototypes_are_frozen_at_registration():
    result = evaluate(
        "JSON.stringify(["
        "[STBlob, STBlob.from([])],"
        "[Hash256, Hash256.from(new Uint8Array(32))],"
        "[AccountID, AccountID.zero],"
        "[UInt8, UInt8.zero]"
        "].every(([factory, value]) => "
        "Object.isFrozen(factory) && Object.isFrozen(Object.getPrototypeOf(value))));"
    )

    assert result.exit_code == 0
    assert result.result_value == "true"


def test_provider_minted_value_prototypes_are_frozen_at_registration():
    result = evaluate(
        "JSON.stringify((() => {"
        "const object = util.decodeObject(STBlob.fromHex("
        "'61D4838D7EA4C680000000000000000000000000005553440000000000' +"
        "'B5F762798A53D543A014CAF8B297CFF8F2F937E8' +"
        "'8114B5F762798A53D543A014CAF8B297CFF8F2F937E8'));"
        "const paths = util.decodeObject(STBlob.fromHex("
        "'8114B5F762798A53D543A014CAF8B297CFF8F2F937E8' +"
        "'011201B5F762798A53D543A014CAF8B297CFF8F2F937E800')).Paths;"
        "return [object, object.Amount, object.Amount.value, paths].every("
        "  value => Object.isFrozen(Object.getPrototypeOf(value)));"
        "})())"
    )

    assert result.exit_code == 0
    assert result.result_value == "true"
