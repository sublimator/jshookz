from __future__ import annotations

from pathlib import Path

from jshookz.host import WasmHost


ROOT = Path(__file__).parents[3]


def _maximum_vl_transaction() -> bytes:
    payment = bytes.fromhex(
        "120000"
        "6140000000000F4240"
        "8114B5F762798A53D543A014CAF8B297CFF8F2F937E8"
        "8314841F44689750ED44FFB6A21950C8F29403915DFD"
    )
    maximum_blob = bytes.fromhex("701AFED417") + bytes(918_744)
    return payment[:12] + maximum_blob + payment[12:]


class _OriginatingTransactionHost:
    def __init__(self, transaction: bytes) -> None:
        self.transaction = transaction
        self.calls: list[str] = []
        self.slots: dict[int, bytes] = {}
        self.host: WasmHost | None = None

    def otxn_slot(self, requested: int) -> int:
        self.calls.append("otxn_slot")
        slot = requested or next(
            candidate for candidate in range(1, 256) if candidate not in self.slots
        )
        self.slots[slot] = self.transaction
        return slot

    def slot_size(self, slot: int) -> int:
        self.calls.append("slot_size")
        return len(self.slots[slot])

    def slot_clear(self, slot: int) -> int:
        self.calls.append("slot_clear")
        del self.slots[slot]
        return 1

    def slot(self, output: int, capacity: int, slot: int) -> int:
        self.calls.append("slot")
        data = self.slots[slot]
        assert self.host is not None
        assert capacity == len(data)
        self.host._write_wasm_memory(output, data)
        return len(data)

    def __getattr__(self, name: str):
        raise AssertionError(f"unexpected host call {name}")


def test_root_allocation_oom_occurs_with_no_live_host_slot(
    resource_probe_wasm: Path,
) -> None:
    compiler = WasmHost.profiled()
    compiler.init()
    try:
        bytecode = compiler.compile_source(
            "export function main() { otxn.object(); return 'unexpected'; }",
            module=True,
        )
    finally:
        compiler.destroy()

    handler = _OriginatingTransactionHost(_maximum_vl_transaction())
    host = WasmHost(handler=handler, wasm_path=resource_probe_wasm)
    handler.host = host
    host.init()
    try:
        current_size = host.instance.exports(host.store)[
            "qjs_resource_current_size"
        ](host.store)
        # Enough for module evaluation, deliberately less than the exact
        # 918,802-byte root allocation. The measurement slot must already be
        # gone before this allocation can fail.
        host.set_memory_limit(current_size + 512 * 1024)
        result = host.run_hook_bytecode(bytecode)
    finally:
        host.destroy()

    assert not result.ok
    assert result.error == "InternalError: out of memory"
    assert handler.calls == ["otxn_slot", "slot_size", "slot_clear"]
    assert handler.slots == {}
