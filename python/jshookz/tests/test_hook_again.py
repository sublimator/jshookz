from __future__ import annotations

from jshookz.host import WasmHost
from jshookz.paths import XAHAU_HOOK_PROVIDER_WASM


class _AgainHost:
    def __init__(self) -> None:
        self.status = 1
        self.calls = 0

    def hook_again(self) -> int:
        self.calls += 1
        return self.status

    def __getattr__(self, name: str):
        raise AssertionError(f"unexpected host call {name}")


def test_hook_again_statuses_align_with_provider_invocation_modes() -> None:
    handler = _AgainHost()
    host = WasmHost(handler=handler, wasm_path=XAHAU_HOOK_PROVIDER_WASM)
    host.init()
    try:
        bytecode = host.compile_source(
            """
              export function main() {
                const outcome = hook.again();
                const status = outcome.ok ? "ok" : outcome.error.code;
                throw new Error(`${hook.mode()}:${status}`);
              }
            """,
            module=True,
        )

        for reserved, status, expected in [
            (0, 1, "Error: strong:ok"),
            (0, -8, "Error: strong:-8"),
            (1, -9, "Error: weak:-9"),
            (2, -9, "Error: again:-9"),
        ]:
            handler.status = status
            result = host.run_hook_bytecode(bytecode, reserved=reserved)
            assert result.error == expected
    finally:
        host.destroy()

    assert handler.calls == 4
