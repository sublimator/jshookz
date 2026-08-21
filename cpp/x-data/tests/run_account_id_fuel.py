#!/usr/bin/env python3
"""Registered AccountID typed-view fuel and dynamic helper-route gates."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from run_amount_fuel import (
    BUDGET,
    parse_objdump_functions,
    reachable_from_start,
    wasm_func_import_count,
)

MODES = (
    "account_id_retained_repeat",
    "account_id_prebound_repeat",
    "account_id_rebind_repeat",
    "account_id_raw_recertify_repeat",
)
HELPERS = (
    "account_id_retained_once_c",
    "account_id_prebound_once_c",
    "account_id_rebind_once_c",
    "account_id_raw_once_c",
)
EXPECTED_HELPER = dict(zip(MODES, ("retained", "prebound", "rebind", "raw")))
HELPER_COUNTS = re.compile(
    r"helper_counts retained=(\d+) prebound=(\d+) rebind=(\d+) raw=(\d+)"
)

# Reproduced wasm32 slopes are 138 / 152.344 / 219.031 / 1586.656. Raising a
# cap is a review-triggering rebaseline, not a routine test repair.
CEILINGS = {"retained": 145, "prebound": 160, "rebind": 230, "raw": 1670}


def compile_wasm(src: Path, wasi: Path, out: Path, *, instrumented: bool) -> None:
    clang = wasi / "bin" / "clang++"
    sysroot = wasi / "share" / "wasi-sysroot"
    flags = [
        str(clang),
        f"--sysroot={sysroot}",
        "-std=c++23",
        "-O2",
        "-fno-exceptions",
        "-fno-rtti",
        "-DCATL_XDATA_NO_BOOST_JSON",
        f"-I{src / 'includes'}",
        f"-I{src / 'core' / 'includes'}",
        f"-I{src / 'base58' / 'includes'}",
        f"-I{src / 'generated'}",
        f"-I{src / 'stubs'}",
        f"-I{src / 'tests'}",
    ]
    if instrumented:
        flags.append("-DCATL_XDATA_ACCOUNT_ID_HELPER_CALL_COUNTS")
    tus = [
        src / "src" / "protocol.cpp",
        src / "src" / "embedded_protocol.cpp",
        src / "base58" / "src" / "base58.cpp",
        src / "core" / "src" / "types.cpp",
        src / "stubs" / "digest_stub.cpp",
        src / "tests" / "account_id_fuel_once.cpp",
        src / "tests" / "account_id_fuel_wasm.cpp",
    ]
    command = [
        *flags,
        *[f"-Wl,--export={name}" for name in HELPERS],
        *[str(path) for path in tus],
        "-o",
        str(out),
    ]
    subprocess.check_call(command)


def run_mode(wasm: Path, mode: str, iterations: int) -> tuple[int, str]:
    from wasmtime import Config, Engine, Linker, Module, Store, WasiConfig

    config = Config()
    config.consume_fuel = True
    engine = Engine(config)
    module = Module.from_file(engine, str(wasm))
    store = Store(engine)
    store.set_fuel(BUDGET)
    wasi = WasiConfig()
    wasi.argv = ["account_id_fuel", mode, str(iterations)]
    with (
        tempfile.NamedTemporaryFile(delete=False) as stdout_file,
        tempfile.NamedTemporaryFile(delete=False) as stderr_file,
    ):
        stdout_path = Path(stdout_file.name)
        stderr_path = Path(stderr_file.name)
    try:
        wasi.stdout_file = str(stdout_path)
        wasi.stderr_file = str(stderr_path)
        linker = Linker(engine)
        linker.define_wasi()
        store.set_wasi(wasi)
        instance = linker.instantiate(store, module)
        instance.exports(store)["_start"](store)
        return BUDGET - store.get_fuel(), stdout_path.read_text()
    finally:
        stdout_path.unlink(missing_ok=True)
        stderr_path.unlink(missing_ok=True)


def require_helpers_from_start(wasi: Path, wasm: Path) -> None:
    dump_bin = wasi / "bin" / "llvm-objdump"
    if not dump_bin.is_file():
        raise RuntimeError(f"missing pinned llvm-objdump: {dump_bin}")
    dump = subprocess.check_output(
        [str(dump_bin), "-d", str(wasm)],
        text=True,
        stderr=subprocess.STDOUT,
    )
    names, bodies = parse_objdump_functions(dump, wasm_func_import_count(wasm))
    reachable, targets = reachable_from_start(names, bodies)
    del reachable
    target_names = {names[index] for index in targets if index in names}
    for helper in HELPERS:
        if helper not in target_names:
            raise RuntimeError(f"no call to {helper} from _start callees")
        print(f"helper_from_start {helper}")


def require_mode_helper_counts(wasm: Path) -> None:
    labels = ("retained", "prebound", "rebind", "raw")
    iterations = 37
    for mode in MODES:
        _, stdout = run_mode(wasm, mode, iterations)
        match = HELPER_COUNTS.search(stdout)
        if match is None:
            raise RuntimeError(f"{mode}: missing helper counts: {stdout!r}")
        got = dict(zip(labels, (int(v) for v in match.groups()), strict=True))
        expected = {label: 0 for label in labels}
        expected[EXPECTED_HELPER[mode]] = iterations
        if got != expected:
            raise RuntimeError(f"{mode}: helper calls {got}, expected {expected}")
        print(f"mode_helper {mode}={EXPECTED_HELPER[mode]}:{iterations}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--src", type=Path, required=True)
    parser.add_argument("--wasi", type=Path, default=None)
    parser.add_argument("--keep-wasm", type=Path, default=None)
    args = parser.parse_args()
    wasi = args.wasi or Path(
        os.environ.get(
            "WASI_SDK_PATH",
            str(Path.home() / ".local/share/mise/installs/wasi-sdk/32/wasi-sdk"),
        )
    )
    if not (wasi / "bin" / "clang++").is_file():
        print("error: wasi-sdk not found", file=sys.stderr)
        return 2
    try:
        import wasmtime  # noqa: F401
    except ImportError:
        print("error: wasmtime not installed", file=sys.stderr)
        return 2

    out = args.keep_wasm or Path(tempfile.mkdtemp()) / "account_id_fuel.wasm"
    try:
        compile_wasm(args.src, wasi, out, instrumented=False)
        require_helpers_from_start(wasi, out)
        counts = out.with_name(f"{out.stem}_counts.wasm")
        compile_wasm(args.src, wasi, counts, instrumented=True)
        require_mode_helper_counts(counts)

        _, invalid_stdout = run_mode(out, "account_id_invalid_setup", 1)
        if "invalid_rejected" not in invalid_stdout or "FAIL" in invalid_stdout:
            raise RuntimeError(f"invalid setup did not reject: {invalid_stdout!r}")
        print(f"invalid_setup stdout={invalid_stdout.strip()!r}")

        rows: dict[tuple[str, int], int] = {}
        sample_iterations = (2048, 4096)
        low_iterations, high_iterations = sample_iterations
        for mode in MODES:
            for iterations in sample_iterations:
                used, stdout = run_mode(out, mode, iterations)
                print(
                    f"{mode} n={iterations} used={used} "
                    f"stdout={stdout.strip()!r}"
                )
                if (
                    "coverage_32" not in stdout
                    or mode not in stdout
                    or "FAIL" in stdout
                ):
                    raise RuntimeError(f"{mode}: invalid markers: {stdout!r}")
                rows[(mode, iterations)] = used

        slopes = {
            mode: (
                rows[(mode, high_iterations)] - rows[(mode, low_iterations)]
            )
            / (high_iterations - low_iterations)
            for mode in MODES
        }
        for mode, slope in slopes.items():
            print(f"slope {mode}={slope:.3f}")
        retained = slopes["account_id_retained_repeat"]
        prebound = slopes["account_id_prebound_repeat"]
        rebind = slopes["account_id_rebind_repeat"]
        raw = slopes["account_id_raw_recertify_repeat"]
        if not (retained < prebound < rebind < raw):
            raise RuntimeError(
                f"slope order retained={retained} prebound={prebound} "
                f"rebind={rebind} raw={raw}"
            )
        measured = {
            "retained": retained,
            "prebound": prebound,
            "rebind": rebind,
            "raw": raw,
        }
        for name, slope in measured.items():
            if slope > CEILINGS[name]:
                raise RuntimeError(
                    f"{name} slope {slope} exceeds budget {CEILINGS[name]}"
                )
        return 0
    except (RuntimeError, subprocess.CalledProcessError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
