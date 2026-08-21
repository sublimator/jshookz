#!/usr/bin/env python3
"""Registered Amount fuel lanes: compile two-TU probe and check slopes."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path


MODES = (
    "amount_mask_only_repeat",
    "amount_retained_parts_repeat",
    "amount_prebound_parts_repeat",
    "amount_view_repeat",
    "amount_raw_recertify_repeat",
)

BUDGET = 80_000_000


def compile_wasm(src: Path, wasi: Path, out: Path) -> None:
    clang = wasi / "bin" / "clang++"
    sysroot = wasi / "share" / "wasi-sysroot"
    includes = [
        f"-I{src / 'includes'}",
        f"-I{src / 'core' / 'includes'}",
        f"-I{src / 'base58' / 'includes'}",
        f"-I{src / 'generated'}",
        f"-I{src / 'stubs'}",
        f"-I{src / 'tests'}",
    ]
    common = [
        str(clang),
        f"--sysroot={sysroot}",
        "-std=c++23",
        "-O2",
        "-fno-exceptions",
        "-fno-rtti",
        "-DCATL_XDATA_NO_BOOST_JSON",
        *includes,
    ]
    tus = [
        src / "src" / "protocol.cpp",
        src / "src" / "embedded_protocol.cpp",
        src / "base58" / "src" / "base58.cpp",
        src / "core" / "src" / "types.cpp",
        src / "stubs" / "digest_stub.cpp",
        src / "tests" / "scan_fuel_once.cpp",
        src / "tests" / "scan_fuel_wasm.cpp",
    ]
    cmd = [
        *common,
        "-Wl,--export=view_once_c",
        "-Wl,--export=raw_once_c",
        "-Wl,--export=mask_once_c",
        "-Wl,--export=parts_once_c",
        "-Wl,--export=retained_parts_c",
        *[str(t) for t in tus],
        "-o",
        str(out),
    ]
    subprocess.check_call(cmd)


def run_mode(wasm: Path, mode: str, n: int) -> tuple[int, str]:
    from wasmtime import Config, Engine, Linker, Module, Store, WasiConfig

    cfg = Config()
    cfg.consume_fuel = True
    engine = Engine(cfg)
    module = Module.from_file(engine, str(wasm))
    store = Store(engine)
    store.set_fuel(BUDGET)
    wasi = WasiConfig()
    wasi.argv = ["scan_fuel", mode, str(n)]
    out = tempfile.NamedTemporaryFile(delete=False)
    err = tempfile.NamedTemporaryFile(delete=False)
    out.close()
    err.close()
    wasi.stdout_file = out.name
    wasi.stderr_file = err.name
    linker = Linker(engine)
    linker.define_wasi()
    store.set_wasi(wasi)
    instance = linker.instantiate(store, module)
    instance.exports(store)["_start"](store)
    used = BUDGET - store.get_fuel()
    stdout = Path(out.name).read_text()
    os.unlink(out.name)
    os.unlink(err.name)
    return used, stdout


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
    if not (wasi / "bin" / "clang++").exists():
        print("skip: wasi-sdk not found", file=sys.stderr)
        return 0
    try:
        import wasmtime  # noqa: F401
    except ImportError:
        print("skip: wasmtime not installed", file=sys.stderr)
        return 0

    out = args.keep_wasm or Path(tempfile.mkdtemp()) / "scan_fuel.wasm"
    compile_wasm(args.src, wasi, out)

    used_inv, out_inv = run_mode(out, "amount_invalid_setup", 1)
    if "FAIL certify_amount_span" not in out_inv or "FAIL invalid accepted" in out_inv:
        print(f"invalid setup did not reject: {out_inv!r}", file=sys.stderr)
        return 1
    print(f"invalid_setup used={used_inv} stdout={out_inv.strip()!r}")

    rows: dict[tuple[str, int], int] = {}
    for mode in MODES:
        for n in (2000, 4000):
            used, stdout = run_mode(out, mode, n)
            print(f"{mode} n={n} used={used} stdout={stdout.strip()!r}")
            if "FAIL" in stdout and "FAIL certify" not in stdout:
                # coverage_32 and mode name only
                if any(line.startswith("FAIL") for line in stdout.splitlines()):
                    print("probe failed", file=sys.stderr)
                    return 1
            if f"coverage_32" not in stdout or mode not in stdout:
                print(f"missing markers: {stdout!r}", file=sys.stderr)
                return 1
            rows[(mode, n)] = used

    slopes = {}
    for mode in MODES:
        slopes[mode] = (rows[(mode, 4000)] - rows[(mode, 2000)]) / 2000
        print(f"slope {mode}={slopes[mode]:.3f}")

    mask = slopes["amount_mask_only_repeat"]
    retained = slopes["amount_retained_parts_repeat"]
    prebound = slopes["amount_prebound_parts_repeat"]
    rebind = slopes["amount_view_repeat"]
    raw = slopes["amount_raw_recertify_repeat"]
    print(
        f"above_mask retained={retained - mask:.3f} "
        f"prebound={prebound - mask:.3f} rebind={rebind - mask:.3f} "
        f"raw={raw - mask:.3f}"
    )
    if not (retained < mask < prebound < rebind < raw):
        print(
            f"slope order retained={retained} mask={mask} "
            f"prebound={prebound} rebind={rebind} raw={raw}",
            file=sys.stderr,
        )
        return 1
    if raw - rebind < 1:
        print("raw-view collapsed", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
