"""Run a JavaScript or TypeScript Hook against hookz's Xahau host."""

from __future__ import annotations

import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Literal

from hookz.runtime import (
    HookAccepted,
    HookRejected,
    HookResult,
    HookRuntime,
)
from jshookz.generated_hook_raw import RAW_HOOK_ABI
from jshookz.host import WasmHost
from jshookz.hook_compiler import compile_hook


_RAW_HOOK_NAMES = frozenset(row[0] for row in RAW_HOOK_ABI)
ExecutionMode = Literal["strong", "weak", "again", "callback"]
_RESERVED_BY_MODE: dict[ExecutionMode, int] = {
    "strong": 0,
    "weak": 1,
    "again": 2,
    "callback": 0,
}


@dataclass
class HostemHookResult(HookResult):
    """Hook result plus invocation evidence owned by the Hostem model."""

    execution_mode: ExecutionMode = "strong"
    again_requested: bool = False


@dataclass
class _AgainInvocation:
    mode: ExecutionMode
    requested: bool = False

    def call(self) -> int:
        if self.requested:
            return -8  # ALREADY_SET
        if self.mode == "strong":
            self.requested = True
            return 1
        return -9  # PREREQUISITE_NOT_MET


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
    """One fresh QuickJS instance per JavaScript/TypeScript Hook delivery."""

    def __init__(
        self,
        runtime: HookRuntime | None = None,
        *,
        wasm_path: str | Path | None = None,
    ):
        self.runtime = runtime or HookRuntime()
        self.wasm_path = Path(wasm_path) if wasm_path is not None else None

    def run_file(
        self,
        path: str | Path,
        *,
        mode: ExecutionMode = "strong",
    ) -> HostemHookResult:
        source = Path(path)
        if source.suffix.lower() in {".ts", ".js", ".mjs"}:
            compiled = compile_hook(source, wasm_path=self.wasm_path)
            return self._run_bytecode(
                compiled.bytecode,
                label=str(source),
                mode=mode,
            )
        raise ValueError(
            f"unsupported Hook source extension {source.suffix!r}; "
            "expected .ts, .js, or .mjs"
        )

    def run_typescript(
        self,
        source: str,
        *,
        label: str = "<hook.ts>",
        mode: ExecutionMode = "strong",
    ) -> HostemHookResult:
        """Compile exact-v1 TypeScript, then execute its QuickJS bytecode."""
        with tempfile.TemporaryDirectory(prefix="hostem-hook-ts-") as temp:
            source_path = Path(temp) / "hook.ts"
            source_path.write_text(source)
            compiled = compile_hook(source_path, wasm_path=self.wasm_path)
        return self._run_bytecode(compiled.bytecode, label=label, mode=mode)

    def run(
        self,
        source: str,
        *,
        label: str = "<hook>",
        mode: ExecutionMode = "strong",
    ) -> HostemHookResult:
        """Run an unchecked JavaScript string as a low-level runtime diagnostic."""
        provider = _HookzProvider(self.runtime)
        compiler = WasmHost.profiled(handler=provider, wasm_path=self.wasm_path)
        compiler.init()
        try:
            bytecode = compiler.compile_source(source, module=True)
        finally:
            compiler.destroy()
        return self._run_bytecode(bytecode, label=label, mode=mode)

    def _run_bytecode(
        self,
        bytecode: bytes,
        *,
        label: str,
        mode: ExecutionMode,
    ) -> HostemHookResult:
        rt = self.runtime
        result = HostemHookResult(execution_mode=mode)
        rt.call_log = []
        rt.traces = []
        rt.checkpoints = []
        rt.dev_events = []
        rt._label = label
        rt._current_export = "cbak" if mode == "callback" else "hook"
        journal_mark = len(rt.state_journal)

        again = _AgainInvocation(mode)
        missing = object()
        previous_again = rt.handlers.get("hook_again", missing)
        provider = _HookzProvider(rt)
        host = WasmHost.profiled(handler=provider, wasm_path=self.wasm_path)
        if previous_again is missing:
            rt.handlers["hook_again"] = again.call

        try:
            with rt.bind_memory(host.memory, host.store):
                host.init()
                execution = host.run_hook_bytecode(
                    bytecode,
                    export="cbak" if mode == "callback" else "hook",
                    reserved=_RESERVED_BY_MODE[mode],
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
        finally:
            if previous_again is missing:
                del rt.handlers["hook_again"]
            else:
                rt.handlers["hook_again"] = previous_again

        result.call_log = list(rt.call_log)
        result.state_writes = rt.state_journal[journal_mark:]
        result.again_requested = again.requested
        return result
