"""Python projection of the selected raw Xahau Hook ABI."""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum

from .generated_hook_raw import RAW_HOOK_ABI


class WasmType(Enum):
    """Types that can cross the WASM boundary."""
    VOID = "void"
    I32 = "i32"
    I64 = "i64"
    F64 = "f64"
    BOOL = "bool"       # i32 0/1
    STRING = "string"   # ptr(i32) + len(i32)
    BYTES = "bytes"     # ptr(i32) + len(i32)


@dataclass
class Param:
    name: str
    type: WasmType
    description: str = ""


@dataclass
class HostFunction:
    name: str                          # JS-visible name
    params: list[Param] = field(default_factory=list)
    returns: WasmType = WasmType.VOID
    description: str = ""
    # The C import name (defaults to "host_{name}")
    import_name: str = ""

    def __post_init__(self):
        if not self.import_name:
            self.import_name = f"host_{self.name}"

    @property
    def wasm_params(self) -> list[tuple[str, str]]:
        """Expand to WASM-level params (strings become ptr+len pairs)."""
        result = []
        for p in self.params:
            if p.type in (WasmType.STRING, WasmType.BYTES):
                result.append((f"{p.name}_ptr", "i32"))
                result.append((f"{p.name}_len", "i32"))
            elif p.type == WasmType.BOOL:
                result.append((p.name, "i32"))
            elif p.type == WasmType.I32:
                result.append((p.name, "i32"))
            elif p.type == WasmType.I64:
                result.append((p.name, "i64"))
            elif p.type == WasmType.F64:
                result.append((p.name, "f64"))
        return result

    @property
    def wasm_return(self) -> str | None:
        """WASM-level return type."""
        if self.returns == WasmType.VOID:
            return None
        if self.returns in (WasmType.STRING, WasmType.BYTES):
            return "i32"  # returns length, data written to out buffer
        if self.returns in (WasmType.BOOL, WasmType.I32):
            return "i32"
        if self.returns == WasmType.I64:
            return "i64"
        if self.returns == WasmType.F64:
            return "f64"
        return None


@dataclass
class HostAPI:
    """Complete host API definition."""
    name: str
    version: str
    functions: list[HostFunction] = field(default_factory=list)


def _raw_hook_functions() -> list[HostFunction]:
    """Materialize the selected macro-generated ABI as raw WASM imports."""
    wasm_type = {
        "int32_t": WasmType.I32,
        "uint32_t": WasmType.I32,
        "int64_t": WasmType.I64,
        "void_t": WasmType.VOID,
    }
    return [
        HostFunction(
            name=name,
            params=[
                Param(f"arg{index}", wasm_type[ctype])
                for index, ctype in enumerate(param_types)
            ],
            returns=wasm_type[return_type],
            import_name=name,
            description=f"Raw Xahau Hook ABI import: {name}",
        )
        for name, return_type, param_types, _amendment in RAW_HOOK_ABI
    ]


HOOK_API = HostAPI(
    name="xahau-hook-raw",
    version="1",
    functions=_raw_hook_functions(),
)
