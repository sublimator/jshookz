#!/usr/bin/env python3
"""Registered Amount fuel lanes: compile two-TU probe and check slopes.

Ceilings lock the 2026-08-21 reproduced slopes (retained 97, mask 142,
prebound 258, rebind 324, raw 423) with a few units of headroom. Raising
a ceiling or lowering a delta floor is a review trigger, not a silent
rebaseline. The full four-frame certified-index value is separately capped
at 3150 against its reproduced 3023 slope.
"""

from __future__ import annotations

import argparse
import os
import re
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

HELPERS = (
    "view_once_c",
    "raw_once_c",
    "mask_once_c",
    "parts_once_c",
    "retained_parts_c",
)

BUDGET = 80_000_000
INDEX_MODE = "certified_index_value"
INDEX_CEILING = 3_150

# Measured 97 / 142 / 258 / 324 / 423. Caps are review-locked.
CEILINGS = {
    "retained": 105,
    "mask": 155,
    "prebound": 270,
    "rebind": 340,
    "raw": 445,
}

# Measured 66 / 161 / 99. Floors are review-locked.
DELTA_FLOORS = {
    "rebind-prebound": 60,
    "prebound-retained": 145,
    "raw-rebind": 90,
}

FUNC_HDR = re.compile(r"^[0-9a-fA-F]+ <([^>]*)>:\s*$")
CALL_IDX = re.compile(r"\bcall\t(\d+)\b")
HELPER_COUNTS = re.compile(
    r"helper_counts view=(\d+) raw=(\d+) mask=(\d+) parts=(\d+) retained=(\d+)"
)
EXPECTED_HELPER = {
    "amount_view_repeat": "view",
    "amount_raw_recertify_repeat": "raw",
    "amount_mask_only_repeat": "mask",
    "amount_prebound_parts_repeat": "parts",
    "amount_retained_parts_repeat": "retained",
}


def read_uleb(buf: bytes, i: int) -> tuple[int, int]:
    n = 0
    shift = 0
    while True:
        b = buf[i]
        i += 1
        n |= (b & 0x7F) << shift
        if b < 0x80:
            return n, i
        shift += 7


def wasm_func_import_count(path: Path) -> int:
    data = path.read_bytes()
    if data[:4] != b"\0asm":
        raise ValueError(f"{path} is not wasm")
    i = 8
    while i < len(data):
        sid = data[i]
        i += 1
        size, i = read_uleb(data, i)
        body = data[i : i + size]
        i += size
        if sid != 2:
            continue
        n, j = read_uleb(body, 0)
        func_imports = 0
        for _ in range(n):
            ln, j = read_uleb(body, j)
            j += ln
            ln, j = read_uleb(body, j)
            j += ln
            kind = body[j]
            j += 1
            if kind == 0:
                _, j = read_uleb(body, j)
                func_imports += 1
            elif kind == 1:
                j += 1
                flags = body[j]
                j += 1
                _, j = read_uleb(body, j)
                if flags & 1:
                    _, j = read_uleb(body, j)
            elif kind == 2:
                flags = body[j]
                j += 1
                _, j = read_uleb(body, j)
                if flags & 1:
                    _, j = read_uleb(body, j)
            elif kind == 3:
                j += 2
            else:
                raise ValueError(f"unknown import kind {kind}")
        return func_imports
    return 0


def compile_wasm(
    src: Path, wasi: Path, out: Path, *, instrumented: bool = False
) -> None:
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
    if instrumented:
        common.append("-DCATL_XDATA_HELPER_CALL_COUNTS")
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
    with (
        tempfile.NamedTemporaryFile(delete=False) as out_file,
        tempfile.NamedTemporaryFile(delete=False) as err_file,
    ):
        out_path = Path(out_file.name)
        err_path = Path(err_file.name)
    try:
        wasi.stdout_file = str(out_path)
        wasi.stderr_file = str(err_path)
        linker = Linker(engine)
        linker.define_wasi()
        store.set_wasi(wasi)
        instance = linker.instantiate(store, module)
        instance.exports(store)["_start"](store)
        used = BUDGET - store.get_fuel()
        return used, out_path.read_text()
    finally:
        out_path.unlink(missing_ok=True)
        err_path.unlink(missing_ok=True)


def parse_objdump_functions(
    dump: str, import_funcs: int
) -> tuple[dict[int, str], dict[int, str]]:
    """Return (index->name, index->body) for local CODE functions."""
    names: dict[int, str] = {}
    bodies: dict[int, list[str]] = {}
    current: int | None = None
    local_i = -1
    for line in dump.splitlines():
        hdr = FUNC_HDR.match(line)
        if hdr:
            name = hdr.group(1)
            if name == "CODE":
                current = None
                continue
            local_i += 1
            idx = import_funcs + local_i
            names[idx] = name or f"func[{idx}]"
            bodies[idx] = []
            current = idx
            continue
        if current is not None:
            bodies[current].append(line)
    return names, {i: "\n".join(b) for i, b in bodies.items()}


def call_indices(body: str) -> set[int]:
    out: set[int] = set()
    for line in body.splitlines():
        if "call_indirect" in line:
            continue
        for m in CALL_IDX.finditer(line):
            out.add(int(m.group(1)))
    return out


def reachable_from_start(
    names: dict[int, str], bodies: dict[int, str]
) -> tuple[set[int], set[int]]:
    name_to_idx = {n: i for i, n in names.items()}
    if "_start" not in name_to_idx:
        raise ValueError("llvm-objdump listing has no _start")
    seen: set[int] = set()
    stack = [name_to_idx["_start"]]
    targets: set[int] = set()
    while stack:
        idx = stack.pop()
        if idx in seen:
            continue
        seen.add(idx)
        if idx not in bodies:
            continue
        called = call_indices(bodies[idx])
        targets.update(called)
        for callee in called:
            if callee not in seen:
                stack.append(callee)
    return seen, targets


def require_helpers_from_start(wasi: Path, wasm: Path) -> None:
    dump_bin = wasi / "bin" / "llvm-objdump"
    if not dump_bin.is_file():
        print(
            f"error: pinned wasi-sdk llvm-objdump not found at {dump_bin}",
            file=sys.stderr,
        )
        raise SystemExit(2)
    dump = subprocess.check_output(
        [str(dump_bin), "-d", str(wasm)],
        text=True,
        stderr=subprocess.STDOUT,
    )
    imports = wasm_func_import_count(wasm)
    names, bodies = parse_objdump_functions(dump, imports)
    try:
        reachable, targets = reachable_from_start(names, bodies)
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        raise SystemExit(1) from e
    reachable_names = {names[i] for i in reachable if i in names}
    target_names = {names[i] for i in targets if i in names}
    print(
        f"disassembler={dump_bin} imports={imports} "
        f"reachable_from_start={len(reachable)} "
        f"call_targets={len(targets)}"
    )
    for name in HELPERS:
        if name not in target_names:
            print(
                f"error: no call to {name} from _start callees "
                f"(reachable={sorted(reachable_names)[:16]})",
                file=sys.stderr,
            )
            raise SystemExit(1)
        print(f"helper_from_start {name}")


def require_mode_helper_counts(wasm: Path) -> None:
    n = 37
    labels = ("view", "raw", "mask", "parts", "retained")
    for mode in MODES:
        _, stdout = run_mode(wasm, mode, n)
        match = HELPER_COUNTS.search(stdout)
        if match is None:
            raise ValueError(f"{mode}: missing helper counts: {stdout!r}")
        got = dict(zip(labels, (int(v) for v in match.groups()), strict=True))
        expected = {label: 0 for label in labels}
        expected[EXPECTED_HELPER[mode]] = n
        if got != expected:
            raise ValueError(f"{mode}: helper calls {got}, expected {expected}")
        print(f"mode_helper {mode}={EXPECTED_HELPER[mode]}:{n}")


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
        print("error: wasi-sdk not found", file=sys.stderr)
        return 2
    try:
        import wasmtime  # noqa: F401
    except ImportError:
        print("error: wasmtime not installed", file=sys.stderr)
        return 2

    out = args.keep_wasm or Path(tempfile.mkdtemp()) / "scan_fuel.wasm"
    compile_wasm(args.src, wasi, out)
    require_helpers_from_start(wasi, out)
    counts_out = out.with_name(f"{out.stem}_counts.wasm")
    compile_wasm(args.src, wasi, counts_out, instrumented=True)
    try:
        require_mode_helper_counts(counts_out)
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    used_inv, out_inv = run_mode(out, "amount_invalid_setup", 1)
    if "FAIL certify_amount_span" not in out_inv or "FAIL invalid accepted" in out_inv:
        print(f"invalid setup did not reject: {out_inv!r}", file=sys.stderr)
        return 1
    print(f"invalid_setup used={used_inv} stdout={out_inv.strip()!r}")

    indexed_rows = {}
    for n in (2000, 4000):
        used, stdout = run_mode(out, INDEX_MODE, n)
        print(f"{INDEX_MODE} n={n} used={used} stdout={stdout.strip()!r}")
        if INDEX_MODE not in stdout or "FAIL" in stdout:
            print(f"indexed certificate probe failed: {stdout!r}", file=sys.stderr)
            return 1
        indexed_rows[n] = used
    indexed_slope = (indexed_rows[4000] - indexed_rows[2000]) / 2000
    print(f"slope {INDEX_MODE}={indexed_slope:.3f}")
    if indexed_slope > INDEX_CEILING:
        print(
            f"indexed certificate slope {indexed_slope} exceeds budget {INDEX_CEILING}",
            file=sys.stderr,
        )
        return 1

    rows: dict[tuple[str, int], int] = {}
    for mode in MODES:
        for n in (2000, 4000):
            used, stdout = run_mode(out, mode, n)
            print(f"{mode} n={n} used={used} stdout={stdout.strip()!r}")
            if (
                "FAIL" in stdout
                and "FAIL certify" not in stdout
                and any(line.startswith("FAIL") for line in stdout.splitlines())
            ):
                print("probe failed", file=sys.stderr)
                return 1
            if "coverage_32" not in stdout or mode not in stdout:
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
    ceilings = {
        "retained": (retained, CEILINGS["retained"]),
        "mask": (mask, CEILINGS["mask"]),
        "prebound": (prebound, CEILINGS["prebound"]),
        "rebind": (rebind, CEILINGS["rebind"]),
        "raw": (raw, CEILINGS["raw"]),
    }
    for name, (got, cap) in ceilings.items():
        if got > cap:
            print(f"{name} slope {got} exceeds budget {cap}", file=sys.stderr)
            return 1
    if rebind - prebound < DELTA_FLOORS["rebind-prebound"]:
        print("rebind-prebound delta collapsed", file=sys.stderr)
        return 1
    if prebound - retained < DELTA_FLOORS["prebound-retained"]:
        print("prebound-retained delta collapsed", file=sys.stderr)
        return 1
    if raw - rebind < DELTA_FLOORS["raw-rebind"]:
        print("raw-rebind delta collapsed", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
