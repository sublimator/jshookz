"""Run a JavaScript Hook against hookz's raw Xahau host implementation."""

from __future__ import annotations

from pathlib import Path

from hookz.runtime import (
    HookAccepted,
    HookRejected,
    HookResult,
    HookRuntime,
)
from jshookz.generated_hook_raw import RAW_HOOK_ABI
from jshookz.host import WasmHost


_RAW_HOOK_NAMES = frozenset(row[0] for row in RAW_HOOK_ABI)


class _HookzProvider:
    """Delegate the selected raw Hook ABI directly to hookz."""

    def __init__(self, runtime: HookRuntime):
        self.runtime = runtime

    def __getattr__(self, name: str):
        if name in _RAW_HOOK_NAMES:
            return lambda *args: self.runtime.dispatch_host_call(name, *args)
        raise AttributeError(name)


def _find_terminal(error: BaseException):
    """Find a hook terminal if wasmtime wrapped the callback exception."""
    current: BaseException | None = error
    seen: set[int] = set()
    while current is not None and id(current) not in seen:
        if isinstance(current, (HookAccepted, HookRejected)):
            return current
        seen.add(id(current))
        current = current.__cause__ or current.__context__
    return None


class HookRunner:
    """One fresh QuickJS instance per JavaScript Hook delivery."""

    def __init__(
        self,
        runtime: HookRuntime | None = None,
        *,
        wasm_path: str | Path | None = None,
    ):
        self.runtime = runtime or HookRuntime()
        self.wasm_path = Path(wasm_path) if wasm_path is not None else None

    def run_file(self, path: str | Path) -> HookResult:
        return self.run(Path(path).read_text(), label=str(path))

    def run(self, source: str, *, label: str = "<hook>") -> HookResult:
        rt = self.runtime
        result = HookResult()
        rt.call_log = []
        rt.traces = []
        rt.checkpoints = []
        rt.dev_events = []
        rt._label = label
        rt._current_export = "hook"
        journal_mark = len(rt.state_journal)

        provider = _HookzProvider(rt)
        compiler = WasmHost(handler=provider, wasm_path=self.wasm_path)
        compiler.init()
        try:
            bytecode = compiler.compile_source(source, module=True)
        finally:
            compiler.destroy()

        host = WasmHost(handler=provider, wasm_path=self.wasm_path)

        try:
            with rt.bind_memory(host.memory, host.store):
                host.init()
                execution = host.run_hook_bytecode(
                    bytecode,
                    export="hook",
                    reserved=0,
                )
                if not execution.ok:
                    raise RuntimeError(
                        execution.error or "QuickJS Hook entry invocation failed"
                    )
        except Exception as error:
            terminal = _find_terminal(error)
            if isinstance(terminal, HookAccepted):
                result.accepted = True
                result.return_msg = terminal.msg
                result.return_code = terminal.code
            elif isinstance(terminal, HookRejected):
                result.rejected = True
                result.return_msg = terminal.msg
                result.return_code = terminal.code
            else:
                result.error = error
        else:
            result.error = RuntimeError(
                "JavaScript Hook returned without accept/rollback")

        result.call_log = list(rt.call_log)
        result.state_writes = rt.state_journal[journal_mark:]
        return result
