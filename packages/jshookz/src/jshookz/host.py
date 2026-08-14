"""Python Wasmtime embedding for compiling and validating Xahau Hooks."""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Protocol

from wasmtime import (
    Config, Engine, Func, FuncType, Linker, Memory, Module, Store, ValType,
)
from wasmtime import _ffi as wasmtime_ffi

from . import paths
from .schema import HOOK_API, HostAPI, HostFunction, WasmType


class HostHandler(Protocol):
    """Implement this to handle host function calls from JS."""

    def __getattr__(self, name: str) -> Any:
        """Called for any function not explicitly defined."""
        ...


@dataclass
class ContractResult:
    """Result of running a contract."""
    exit_code: int
    result_value: str | None = None
    gas_used: int = 0
    error: str | None = None
    logs: list[str] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return self.exit_code == 0


@dataclass(frozen=True)
class HookModuleValidation:
    """Structural result returned by the pinned provider's Hook validator."""

    valid: bool
    has_callback: bool = False
    error: str | None = None


class UnavailableHookHost:
    """Link the raw ABI while refusing calls without an explicit host.

    Compilation and host-free module validation only need the imports to link.
    Executing Hook host operations requires hookz or another concrete handler.
    """

    def __init__(self) -> None:
        self.logs: list[str] = []

    def __getattr__(self, name: str) -> Any:
        def unavailable(*_args: Any) -> int:
            raise RuntimeError(
                f"Xahau Hook host operation {name!r} has no bound implementation"
            )

        return unavailable


class WasmHost:
    """Python host that loads jshookz_provider.wasm via wasmtime."""

    def __init__(
        self,
        handler: Any = None,
        wasm_path: Path | None = None,
        api: HostAPI = HOOK_API,
        fuel: int = 0,
    ):
        self.handler = handler or UnavailableHookHost()
        self.wasm_path = wasm_path or paths.XAHAU_HOOK_PROVIDER_WASM
        self.api = api
        self.fuel = fuel
        self._validating_hook = False

        self._setup_engine()

    def _setup_engine(self):
        config = Config()
        # The Python binding does not expose this Wasmtime C-API setting,
        # but it is a consensus input for the QuickJS profile.  Keep the call
        # explicit and pinned instead of inheriting the engine default.
        wasmtime_ffi.wasmtime_config_cranelift_nan_canonicalization_set(
            config.ptr(), True
        )
        config.wasm_threads = False
        config.wasm_relaxed_simd = False
        config.wasm_memory64 = False
        config.wasm_multi_memory = False
        config.wasm_tail_call = False
        if self.fuel > 0:
            config.consume_fuel = True

        self.engine = Engine(config)
        self.store = Store(self.engine)

        if self.fuel > 0:
            self.store.set_fuel(self.fuel)

        self.linker = Linker(self.engine)

        # Register host functions from schema
        for fn in self.api.functions:
            self._register_host_function(fn)

        # Load and instantiate
        module = Module.from_file(self.engine, str(self.wasm_path))
        self.instance = self.linker.instantiate(self.store, module)
        self.memory = self.instance.exports(self.store)["memory"]

        # Call _initialize for cold reactor modules. Wizered modules may not
        # retain this export because initialization already ran at snapshot time.
        try:
            init = self.instance.exports(self.store)["_initialize"]
        except KeyError:
            init = None
        if init is not None:
            init(self.store)

    def _read_wasm_string(self, ptr: int, length: int) -> str:
        data = self.memory.data_ptr(self.store)
        raw = bytes(data[ptr:ptr + length])
        return raw.decode("utf-8")

    def _write_wasm_memory(self, ptr: int, data: bytes) -> int:
        mem_data = self.memory.data_ptr(self.store)
        for i, b in enumerate(data):
            mem_data[ptr + i] = b
        return len(data)

    def _wasm_malloc(self, size: int) -> int:
        malloc_fn = self.instance.exports(self.store)["malloc"]
        return malloc_fn(self.store, size)

    def _wasm_free(self, ptr: int):
        free_fn = self.instance.exports(self.store)["free"]
        free_fn(self.store, ptr)

    def _write_string_to_wasm(self, s: str) -> tuple[int, int]:
        data = s.encode("utf-8")
        ptr = self._wasm_malloc(len(data) + 1)
        self._write_wasm_memory(ptr, data + b"\x00")
        return ptr, len(data)

    def _register_host_function(self, fn: HostFunction):
        """Register a single host function with wasmtime."""
        # Build wasmtime param/return types
        param_types = []
        for name, wtype in fn.wasm_params:
            param_types.append({"i32": ValType.i32(), "i64": ValType.i64(),
                                "f64": ValType.f64()}[wtype])

        return_types = []
        if fn.returns in (WasmType.STRING, WasmType.BYTES):
            # String/bytes return: extra out_buf + out_len params, returns length
            param_types.extend([ValType.i32(), ValType.i32()])
            return_types.append(ValType.i32())
        elif fn.returns != WasmType.VOID:
            wrt = fn.wasm_return
            return_types.append({"i32": ValType.i32(), "i64": ValType.i64(),
                                 "f64": ValType.f64()}[wrt])

        ftype = FuncType(param_types, return_types)

        # Build the wrapper that calls the handler
        schema_fn = fn  # capture

        host_self = self  # capture

        def wrapper(*args):
            return host_self._dispatch_host_call(schema_fn, args)

        func = Func(self.store, ftype, wrapper)
        self.linker.define(self.store, "env", fn.import_name, func)

    def _dispatch_host_call(self, fn: HostFunction, wasm_args: tuple) -> Any:
        """Dispatch a WASM host call to the Python handler."""
        if self._validating_hook:
            raise RuntimeError(
                "Hook host calls are unavailable during module initialization"
            )

        # Unmarshal WASM args to Python args
        py_args = []
        wasm_idx = 0
        for p in fn.params:
            if p.type in (WasmType.STRING, WasmType.BYTES):
                ptr = wasm_args[wasm_idx]
                length = wasm_args[wasm_idx + 1]
                wasm_idx += 2
                if p.type == WasmType.STRING:
                    py_args.append(self._read_wasm_string(ptr, length))
                else:
                    data = self.memory.data_ptr(self.store)
                    py_args.append(bytes(data[ptr:ptr + length]))
            else:
                py_args.append(wasm_args[wasm_idx])
                wasm_idx += 1

        # Call the handler
        handler_fn = getattr(self.handler, fn.name, None)
        if handler_fn is None:
            raise AttributeError(f"Host handler has no method '{fn.name}'")

        result = handler_fn(*py_args)

        # Marshal return value
        if fn.returns == WasmType.VOID:
            return None

        if fn.returns in (WasmType.STRING, WasmType.BYTES):
            out_ptr = wasm_args[wasm_idx]
            out_len = wasm_args[wasm_idx + 1]

            if result is None:
                return 0

            if fn.returns == WasmType.STRING:
                data = result.encode("utf-8") if isinstance(result, str) else result
            else:
                data = result

            copy_len = min(len(data), out_len)
            self._write_wasm_memory(out_ptr, data[:copy_len])
            return copy_len

        return result

    def init(self):
        """Initialize QuickJS inside the WASM module.

        Note: init costs ~1.4M fuel. If using fuel metering, ensure
        enough fuel is available or add fuel before init.
        """
        qjs_init = self.instance.exports(self.store)["qjs_init"]
        qjs_init(self.store)

    def add_fuel(self, amount: int):
        """Add more fuel to the store."""
        self.store.set_fuel(self.store.get_fuel() + amount)

    def set_seed(self, seed: int):
        fn = self.instance.exports(self.store)["qjs_set_seed"]
        fn(self.store, seed)

    def set_memory_limit(self, limit: int):
        fn = self.instance.exports(self.store)["qjs_set_memory_limit"]
        fn(self.store, limit)

    def _eval_common(self, code: str, func_name: str = "qjs_eval") -> ContractResult:
        """Common eval logic for both global and module modes."""
        ptr, length = self._write_string_to_wasm(code)

        if self.fuel > 0:
            fuel_before = self.store.get_fuel()

        try:
            qjs_fn = self.instance.exports(self.store)[func_name]
            exit_code = qjs_fn(self.store, ptr, length)
        except Exception as e:
            if "fuel" in str(e).lower() or "all fuel" in str(e).lower():
                return ContractResult(exit_code=-1, error="out of gas",
                                      gas_used=self.fuel)
            raise
        finally:
            self._wasm_free(ptr)

        gas_used = 0
        if self.fuel > 0:
            fuel_after = self.store.get_fuel()
            gas_used = fuel_before - fuel_after

        # Read result
        get_ptr = self.instance.exports(self.store)["qjs_get_result_ptr"]
        get_len = self.instance.exports(self.store)["qjs_get_result_len"]
        rptr = get_ptr(self.store)
        rlen = get_len(self.store)

        result_value = None
        if rlen > 0:
            result_value = self._read_wasm_string(rptr, rlen)

        logs = []
        if hasattr(self.handler, "logs"):
            logs = list(self.handler.logs)

        return ContractResult(
            exit_code=exit_code,
            result_value=result_value,
            gas_used=gas_used,
            error=result_value if exit_code != 0 else None,
            logs=logs,
        )

    def eval(self, code: str) -> ContractResult:
        """Evaluate JS source code (global scope)."""
        return self._eval_common(code, "qjs_eval")

    def eval_module(self, code: str) -> ContractResult:
        """Evaluate JS module code (import/export enabled)."""
        return self._eval_common(code, "qjs_eval_module")

    def eval_bytecode(self, bytecode: bytes) -> ContractResult:
        """Evaluate precompiled bytecode."""
        ptr = self._wasm_malloc(len(bytecode))
        self._write_wasm_memory(ptr, bytecode)

        if self.fuel > 0:
            fuel_before = self.store.get_fuel()

        try:
            qjs_eval_bc = self.instance.exports(self.store)["qjs_eval_bytecode"]
            exit_code = qjs_eval_bc(self.store, ptr, len(bytecode))
        except Exception as e:
            if "fuel" in str(e).lower():
                return ContractResult(exit_code=-1, error="out of gas",
                                      gas_used=self.fuel)
            raise
        finally:
            self._wasm_free(ptr)

        gas_used = 0
        if self.fuel > 0:
            fuel_after = self.store.get_fuel()
            gas_used = fuel_before - fuel_after

        get_ptr = self.instance.exports(self.store)["qjs_get_result_ptr"]
        get_len = self.instance.exports(self.store)["qjs_get_result_len"]
        rptr = get_ptr(self.store)
        rlen = get_len(self.store)

        result_value = None
        if rlen > 0:
            result_value = self._read_wasm_string(rptr, rlen)

        return ContractResult(
            exit_code=exit_code,
            result_value=result_value,
            gas_used=gas_used,
            error=result_value if exit_code != 0 else None,
        )

    def run_hook_bytecode(
        self,
        bytecode: bytes,
        *,
        export: str = "hook",
        reserved: int = 0,
    ) -> ContractResult:
        """Invoke main/callback through the outer qjs_hook/qjs_cbak ABI."""
        if export not in {"hook", "cbak"}:
            raise ValueError("Hook export must be 'hook' or 'cbak'")

        ptr = self._wasm_malloc(len(bytecode))
        self._write_wasm_memory(ptr, bytecode)

        if self.fuel > 0:
            fuel_before = self.store.get_fuel()

        try:
            invoke = self.instance.exports(self.store)[f"qjs_{export}"]
            exit_code = invoke(self.store, ptr, len(bytecode), reserved)
        except Exception as error:
            if "fuel" in str(error).lower():
                return ContractResult(
                    exit_code=-1,
                    error="out of gas",
                    gas_used=self.fuel,
                )
            raise
        finally:
            self._wasm_free(ptr)

        gas_used = 0
        if self.fuel > 0:
            gas_used = fuel_before - self.store.get_fuel()

        get_ptr = self.instance.exports(self.store)["qjs_get_result_ptr"]
        get_len = self.instance.exports(self.store)["qjs_get_result_len"]
        result_ptr = get_ptr(self.store)
        result_len = get_len(self.store)
        result_value = (
            self._read_wasm_string(result_ptr, result_len)
            if result_len > 0
            else None
        )

        return ContractResult(
            exit_code=exit_code,
            result_value=result_value,
            gas_used=gas_used,
            error=result_value if exit_code != 0 else None,
        )

    def validate_hook_bytecode(self, bytecode: bytes) -> HookModuleValidation:
        """Validate Hook module initialization and callable entry exports.

        The provider evaluates module initialization in this disposable host
        instance, but never invokes hook or cbak. Xahau uses the same export
        to admit bytecode and derive callback presence.
        """
        ptr = self._wasm_malloc(len(bytecode))
        self._write_wasm_memory(ptr, bytecode)
        try:
            self._validating_hook = True
            validate = self.instance.exports(self.store)[
                "qjs_validate_hook_module"
            ]
            flags = validate(self.store, ptr, len(bytecode))
        except Exception as error:
            return HookModuleValidation(valid=False, error=str(error))
        finally:
            self._validating_hook = False
            self._wasm_free(ptr)

        if flags in {1, 3}:
            return HookModuleValidation(
                valid=True,
                has_callback=bool(flags & 2),
            )

        get_ptr = self.instance.exports(self.store)["qjs_get_result_ptr"]
        get_len = self.instance.exports(self.store)["qjs_get_result_len"]
        result_ptr = get_ptr(self.store)
        result_len = get_len(self.store)
        error = (
            self._read_wasm_string(result_ptr, result_len)
            if result_len > 0
            else "QuickJS Hook bytecode validation failed"
        )
        return HookModuleValidation(valid=False, error=error)

    def compile_source(self, source: str, *, module: bool = True) -> bytes:
        """Compile source with this exact QuickJS WASM provider.

        Hook source is an ES module because its entry points are exported.
        Compiling inside the provider pins bytecode production to the same
        QuickJS fork and build configuration that will later consume it.
        """
        ptr, length = self._write_string_to_wasm(source)
        function_name = "qjs_compile_module" if module else "qjs_compile"

        try:
            compile_fn = self.instance.exports(self.store)[function_name]
            bytecode_len = compile_fn(self.store, ptr, length)
            if bytecode_len < 0:
                get_ptr = self.instance.exports(self.store)["qjs_get_result_ptr"]
                get_len = self.instance.exports(self.store)["qjs_get_result_len"]
                error_ptr = get_ptr(self.store)
                error_len = get_len(self.store)
                error = self._read_wasm_string(error_ptr, error_len)
                raise RuntimeError(error or "QuickJS compilation failed")

            get_ptr = self.instance.exports(self.store)["qjs_get_bytecode_ptr"]
            get_len = self.instance.exports(self.store)["qjs_get_bytecode_len"]
            bytecode_ptr = get_ptr(self.store)
            stored_len = get_len(self.store)
            if stored_len != bytecode_len:
                raise RuntimeError(
                    "QuickJS bytecode length changed while retrieving output"
                )

            data = self.memory.data_ptr(self.store)
            return bytes(data[bytecode_ptr : bytecode_ptr + bytecode_len])
        finally:
            self._wasm_free(ptr)

    def destroy(self):
        """Clean up QuickJS context."""
        qjs_destroy = self.instance.exports(self.store)["qjs_destroy"]
        qjs_destroy(self.store)
