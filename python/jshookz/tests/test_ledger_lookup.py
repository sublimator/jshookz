from __future__ import annotations

from pathlib import Path

from jshookz.host import WasmHost


ACCOUNT_HEX = "B5F762798A53D543A014CAF8B297CFF8F2F937E8"
ACCOUNT_KEYLET = bytes.fromhex(
    "0061"
    "2B6AC232AA4C4BE41BF49D2459FA4A0347E1B543A4C92FCEE0821C0201E2E9A8"
)


def _maximum_vl_account_root() -> bytes:
    maximum_message_key = bytes.fromhex("72FED417") + bytes(918_744)
    return (
        bytes.fromhex(
            "110061"
            "2200000000"
            "2400000007"
            "250000007B"
            "2D00000000"
            "550000000000000000000000000000000000000000000000000000000000000000"
            "62400000000EE6B280"
        )
        + maximum_message_key
        + bytes.fromhex("8114" + ACCOUNT_HEX)
    )


class _LedgerLookupHost:
    def __init__(self, value: bytes) -> None:
        self.value = value
        self.calls: list[str] = []
        self.slots: dict[int, bytes] = {}
        self.host: WasmHost | None = None

    def _read_memory(self, pointer: int, length: int) -> bytes:
        assert self.host is not None
        data = self.host.memory.data_ptr(self.host.store)
        return bytes(data[pointer : pointer + length])

    def slot_set(self, pointer: int, length: int, requested: int) -> int:
        self.calls.append("slot_set")
        assert self._read_memory(pointer, length) == ACCOUNT_KEYLET
        slot = requested or next(
            candidate for candidate in range(1, 256) if candidate not in self.slots
        )
        self.slots[slot] = self.value
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
        value = self.slots[slot]
        assert self.host is not None
        assert capacity == len(value)
        self.host._write_wasm_memory(output, value)
        return len(value)

    def __getattr__(self, name: str):
        raise AssertionError(f"unexpected host call {name}")


def test_root_allocation_oom_occurs_with_no_live_host_slot(
    resource_probe_wasm: Path,
) -> None:
    compiler = WasmHost.profiled()
    compiler.init()
    try:
        bytecode = compiler.compile_source(
            f'''
              export function main() {{
                const keylet = util.keylet.account(AccountID.fromHex("{ACCOUNT_HEX}"));
                ledger.lookup(keylet);
                return "unexpected";
              }}
            ''',
            module=True,
        )
    finally:
        compiler.destroy()

    handler = _LedgerLookupHost(_maximum_vl_account_root())
    host = WasmHost(handler=handler, wasm_path=resource_probe_wasm)
    handler.host = host
    host.init()
    try:
        current_size = host.instance.exports(host.store)[
            "qjs_resource_current_size"
        ](host.store)
        # Enough for module evaluation, deliberately less than the exact
        # 918,835-byte root allocation. The measurement slot must already be
        # gone before this allocation can fail.
        host.set_memory_limit(current_size + 512 * 1024)
        result = host.run_hook_bytecode(bytecode)
    finally:
        host.destroy()

    assert not result.ok
    assert result.error == "InternalError: out of memory"
    assert handler.calls == ["slot_set", "slot_size", "slot_clear"]
    assert handler.slots == {}
