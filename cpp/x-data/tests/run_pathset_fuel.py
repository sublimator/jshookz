#!/usr/bin/env python3
"""PathSet one/many-hop fuel, call-route, and poisoned-control gates."""

from __future__ import annotations

import argparse
import importlib.metadata
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from run_amount_fuel import (
    BUDGET,
    call_indices,
    parse_objdump_functions,
    reachable_from_start,
    wasm_func_import_count,
)

PINNED_WASI_CLANG_PREFIX = "clang version 22.1.0-wasi-sdk"
PINNED_WASI_VERSION = (
    "32.0",
    "wasi-libc: 2fc32bc81b9f",
    "llvm: 4434dabb6991",
    "llvm-version: 22.1.0",
    "config: f992bcc08219",
)
PINNED_WASMTIME = "47.0.1"
SAMPLE_ITERATIONS = (2048, 4096)
CLEAN_REPEATS = 2

OPERATIONS = (
    "sequential",
    "measure_fill",
    "cached_length",
    "cached_at",
    "raw_recertify",
)
BANKS = ("one", "many")
MODES = tuple(
    f"pathset_{bank}_{operation}_repeat" for bank in BANKS for operation in OPERATIONS
)
HELPERS = (
    "pathset_sequential_once_c",
    "pathset_measure_fill_once_c",
    "pathset_cached_length_once_c",
    "pathset_cached_at_once_c",
    "pathset_raw_recertify_once_c",
)

HELPER_COUNTS = re.compile(
    r"helper_counts sequential=(\d+) measure_fill=(\d+) "
    r"cached_length=(\d+) cached_at=(\d+) raw=(\d+)"
)
ROUTE_COUNTS = re.compile(
    r"route_counts locate=(\d+) certify=(\d+) traverse=(\d+) "
    r"measure=(\d+) fill=(\d+)"
)
HELPER_LABELS = ("sequential", "measure_fill", "cached_length", "cached_at", "raw")
ROUTE_LABELS = ("locate", "certify", "traverse", "measure", "fill")
EXPECTED_ROUTES = {
    "sequential": {"traverse": 1},
    "measure_fill": {"traverse": 2, "measure": 1, "fill": 1},
    "cached_length": {},
    "cached_at": {},
    "raw_recertify": {"certify": 1, "traverse": 1},
}

# Reproduced wasm32 slopes, in mode order by bank, are:
# one: 258 / 662 / 202 / 271 / 1101
# many: 1004 / 2107 / 200 / 269 / 2390
# Caps leave roughly five percent headroom. Raising one is a review-triggering
# rebaseline, not a routine test repair.
CEILINGS = {
    "pathset_one_sequential_repeat": 275.0,
    "pathset_one_measure_fill_repeat": 700.0,
    "pathset_one_cached_length_repeat": 215.0,
    "pathset_one_cached_at_repeat": 285.0,
    "pathset_one_raw_recertify_repeat": 1160.0,
    "pathset_many_sequential_repeat": 1060.0,
    "pathset_many_measure_fill_repeat": 2220.0,
    "pathset_many_cached_length_repeat": 212.0,
    "pathset_many_cached_at_repeat": 283.0,
    "pathset_many_raw_recertify_repeat": 2510.0,
}
# Floors retain about 85-90% of the reproduced separations. A one-hop cached
# at is intentionally not ordered against a one-hop sequential walk: both are
# constant, single-hop metadata operations (271 versus 258). Many-hop scaling
# and every recertification/measurement separation remain mechanically gated.
DELTA_FLOORS = {
    "one_at_minus_length": 60.0,
    "one_measure_minus_sequential": 360.0,
    "one_raw_minus_sequential": 750.0,
    "many_at_minus_length": 60.0,
    "many_sequential_minus_at": 660.0,
    "many_measure_minus_sequential": 990.0,
    "many_raw_minus_sequential": 1240.0,
    "many_minus_one_sequential": 670.0,
    "many_minus_one_measure": 1300.0,
    "many_minus_one_raw": 1150.0,
}
CACHE_SPREAD_CEILINGS = {"cached_length": 4.0, "cached_at": 4.0}


def operation_for(mode: str) -> str:
    for operation in OPERATIONS:
        if (
            mode == f"pathset_one_{operation}_repeat"
            or mode == f"pathset_many_{operation}_repeat"
        ):
            return operation
    raise ValueError(f"unknown PathSet fuel mode: {mode}")


def validate_pins(wasi: Path) -> None:
    clang = wasi / "bin" / "clang++"
    if not clang.is_file():
        raise RuntimeError(f"wasi-sdk 32 clang++ not found: {clang}")
    version = subprocess.check_output(
        [str(clang), "--version"], text=True
    ).splitlines()[0]
    if not version.startswith(PINNED_WASI_CLANG_PREFIX):
        raise RuntimeError(
            f"wasi compiler drift: {version!r}, expected prefix "
            f"{PINNED_WASI_CLANG_PREFIX!r}"
        )
    version_file = wasi / "VERSION"
    wasi_version = tuple(version_file.read_text().splitlines())
    if wasi_version != PINNED_WASI_VERSION:
        raise RuntimeError(
            f"wasi-sdk drift: {wasi_version!r}, expected {PINNED_WASI_VERSION!r}"
        )
    wasmtime_version = importlib.metadata.version("wasmtime")
    if wasmtime_version != PINNED_WASMTIME:
        raise RuntimeError(
            f"wasmtime drift: {wasmtime_version}, expected {PINNED_WASMTIME}"
        )
    print(
        f"toolchain wasi_sdk={wasi_version[0]} llvm={wasi_version[2]} "
        f"wasmtime={wasmtime_version}"
    )


def compile_wasm(
    src: Path,
    wasi: Path,
    out: Path,
    *,
    instrumented: bool,
    poison: str | None = None,
) -> None:
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
        flags.extend(
            (
                "-DCATL_XDATA_PATHSET_HELPER_CALL_COUNTS",
                "-DCATL_XDATA_TEST_PATHSET_HOOK",
            )
        )
    if poison == "helper":
        flags.append("-DCATL_XDATA_PATHSET_WRONG_HELPER_CONTROL")
    elif poison == "rule":
        flags.append("-DCATL_XDATA_PATHSET_WRONG_RULE_CONTROL")
    elif poison == "sequential_certify":
        flags.append("-DCATL_XDATA_PATHSET_WRONG_SEQUENTIAL_CERTIFY_CONTROL")
    elif poison is not None:
        raise ValueError(f"unknown poison: {poison}")
    translation_units = [
        src / "src" / "protocol.cpp",
        src / "src" / "embedded_protocol.cpp",
        src / "base58" / "src" / "base58.cpp",
        src / "core" / "src" / "types.cpp",
        src / "stubs" / "digest_stub.cpp",
        src / "tests" / "pathset_fuel_once.cpp",
        src / "tests" / "pathset_fuel_wasm.cpp",
    ]
    command = [
        *flags,
        *[f"-Wl,--export={helper}" for helper in HELPERS],
        *[str(path) for path in translation_units],
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
    wasi.argv = ["pathset_fuel", mode, str(iterations)]
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
        # Invoke the WASI entry point directly; command wrappers are not metered.
        instance.exports(store)["_start"](store)
        stderr = stderr_path.read_text()
        if stderr:
            raise RuntimeError(f"{mode}: unexpected stderr: {stderr!r}")
        return BUDGET - store.get_fuel(), stdout_path.read_text()
    finally:
        stdout_path.unlink(missing_ok=True)
        stderr_path.unlink(missing_ok=True)


def require_helpers_from_start(wasi: Path, wasm: Path) -> None:
    objdump = wasi / "bin" / "llvm-objdump"
    if not objdump.is_file():
        raise RuntimeError(f"missing pinned llvm-objdump: {objdump}")
    dump = subprocess.check_output(
        [str(objdump), "-d", str(wasm)], text=True, stderr=subprocess.STDOUT
    )
    names, bodies = parse_objdump_functions(dump, wasm_func_import_count(wasm))
    reachable, _ = reachable_from_start(names, bodies)
    name_to_index = {name: index for index, name in names.items()}
    for helper in HELPERS:
        helper_index = name_to_index.get(helper)
        if helper_index is None:
            raise RuntimeError(f"disassembly has no helper symbol {helper}")
        callers = [
            names[index]
            for index in reachable
            if index in bodies and helper_index in call_indices(bodies[index])
        ]
        if helper_index not in reachable or not callers:
            raise RuntimeError(
                f"{helper} is not reached by a direct call in the _start graph"
            )
        print(f"helper_callgraph {helper} callers={','.join(sorted(callers))}")


def parse_counts(stdout: str) -> tuple[dict[str, int], dict[str, int]]:
    helper_match = HELPER_COUNTS.search(stdout)
    route_match = ROUTE_COUNTS.search(stdout)
    if helper_match is None or route_match is None:
        raise RuntimeError(f"missing helper/route counts: {stdout!r}")
    helpers = dict(
        zip(HELPER_LABELS, (int(value) for value in helper_match.groups()), strict=True)
    )
    routes = dict(
        zip(ROUTE_LABELS, (int(value) for value in route_match.groups()), strict=True)
    )
    return helpers, routes


def require_mode_counts(wasm: Path, mode: str, iterations: int) -> None:
    _, stdout = run_mode(wasm, mode, iterations)
    helpers, routes = parse_counts(stdout)
    operation = operation_for(mode)
    helper_label = "raw" if operation == "raw_recertify" else operation
    expected_helpers = {label: 0 for label in HELPER_LABELS}
    expected_helpers[helper_label] = iterations
    if helpers != expected_helpers:
        raise RuntimeError(
            f"{mode}: helper calls {helpers}, expected {expected_helpers}"
        )
    expected_routes = {label: 0 for label in ROUTE_LABELS}
    for label, per_call in EXPECTED_ROUTES[operation].items():
        expected_routes[label] = per_call * iterations
    if routes != expected_routes:
        raise RuntimeError(f"{mode}: rule routes {routes}, expected {expected_routes}")
    print(f"mode_route {mode} helper={helper_label}:{iterations} routes={routes}")


def require_normal_routes(wasm: Path) -> None:
    for mode in MODES:
        require_mode_counts(wasm, mode, 37)


def require_poison_rejected(
    wasm: Path, mode: str, expected_fragment: str, label: str
) -> None:
    try:
        require_mode_counts(wasm, mode, 37)
    except RuntimeError as error:
        if expected_fragment not in str(error):
            raise RuntimeError(
                f"{label} failed for the wrong reason: {error}"
            ) from error
        print(f"{label}_rejected {error}")
        return
    raise RuntimeError(f"{label} did not make the route gate fail")


def validate_markers(mode: str, stdout: str) -> None:
    bank_marker = "bank_one_hop" if "_one_" in mode else "bank_many_hop"
    if (
        "coverage_32" not in stdout
        or bank_marker not in stdout
        or mode not in stdout
        or "FAIL" in stdout
    ):
        raise RuntimeError(f"{mode}: invalid markers: {stdout!r}")


def measure_slopes(wasm: Path) -> dict[str, float]:
    low, high = SAMPLE_ITERATIONS
    rows: dict[tuple[str, int], int] = {}
    for mode in MODES:
        for iterations in SAMPLE_ITERATIONS:
            samples: list[int] = []
            stdout_samples: list[str] = []
            for _ in range(CLEAN_REPEATS):
                used, stdout = run_mode(wasm, mode, iterations)
                validate_markers(mode, stdout)
                samples.append(used)
                stdout_samples.append(stdout)
            if len(set(samples)) != 1 or len(set(stdout_samples)) != 1:
                raise RuntimeError(
                    f"{mode} n={iterations}: nondeterministic repeats {samples}"
                )
            rows[(mode, iterations)] = samples[0]
            print(
                f"clean_repeat {mode} n={iterations} repeats={CLEAN_REPEATS} "
                f"used={samples[0]}"
            )
    slopes = {
        mode: (rows[(mode, high)] - rows[(mode, low)]) / (high - low) for mode in MODES
    }
    for mode, slope in slopes.items():
        print(f"slope {mode}={slope:.3f} ceiling={CEILINGS[mode]:.3f}")
        if slope > CEILINGS[mode]:
            raise RuntimeError(
                f"{mode} slope {slope} exceeds absolute ceiling {CEILINGS[mode]}"
            )
    return slopes


def require_meaningful_deltas(slopes: dict[str, float]) -> None:
    def slope(bank: str, operation: str) -> float:
        return slopes[f"pathset_{bank}_{operation}_repeat"]

    deltas = {
        "one_at_minus_length": slope("one", "cached_at")
        - slope("one", "cached_length"),
        "one_measure_minus_sequential": slope("one", "measure_fill")
        - slope("one", "sequential"),
        "one_raw_minus_sequential": slope("one", "raw_recertify")
        - slope("one", "sequential"),
        "many_at_minus_length": slope("many", "cached_at")
        - slope("many", "cached_length"),
        "many_sequential_minus_at": slope("many", "sequential")
        - slope("many", "cached_at"),
        "many_measure_minus_sequential": slope("many", "measure_fill")
        - slope("many", "sequential"),
        "many_raw_minus_sequential": slope("many", "raw_recertify")
        - slope("many", "sequential"),
        "many_minus_one_sequential": slope("many", "sequential")
        - slope("one", "sequential"),
        "many_minus_one_measure": slope("many", "measure_fill")
        - slope("one", "measure_fill"),
        "many_minus_one_raw": slope("many", "raw_recertify")
        - slope("one", "raw_recertify"),
    }
    for name, value in deltas.items():
        floor = DELTA_FLOORS[name]
        print(f"delta {name}={value:.3f} floor={floor:.3f}")
        if value < floor:
            raise RuntimeError(f"delta {name}={value} is below floor {floor}")
    for operation, ceiling in CACHE_SPREAD_CEILINGS.items():
        spread = abs(slope("many", operation) - slope("one", operation))
        print(f"cache_spread {operation}={spread:.3f} ceiling={ceiling:.3f}")
        if spread > ceiling:
            raise RuntimeError(
                f"cached {operation} spread {spread} exceeds ceiling {ceiling}"
            )


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
    try:
        import wasmtime  # noqa: F401

        validate_pins(wasi)
        work = args.keep_wasm.parent if args.keep_wasm else Path(tempfile.mkdtemp())
        out = args.keep_wasm or work / "pathset_fuel.wasm"
        compile_wasm(args.src, wasi, out, instrumented=False)
        require_helpers_from_start(wasi, out)

        counts = work / "pathset_fuel_counts.wasm"
        compile_wasm(args.src, wasi, counts, instrumented=True)
        require_normal_routes(counts)

        helper_poison = work / "pathset_fuel_wrong_helper.wasm"
        compile_wasm(args.src, wasi, helper_poison, instrumented=True, poison="helper")
        require_poison_rejected(
            helper_poison,
            "pathset_many_cached_at_repeat",
            "helper calls",
            "wrong_helper_control",
        )

        rule_poison = work / "pathset_fuel_wrong_rule.wasm"
        compile_wasm(args.src, wasi, rule_poison, instrumented=True, poison="rule")
        require_poison_rejected(
            rule_poison,
            "pathset_many_cached_length_repeat",
            "rule routes",
            "wrong_rule_control",
        )

        sequential_poison = work / "pathset_fuel_wrong_sequential_certify.wasm"
        compile_wasm(
            args.src,
            wasi,
            sequential_poison,
            instrumented=True,
            poison="sequential_certify",
        )
        require_poison_rejected(
            sequential_poison,
            "pathset_many_sequential_repeat",
            "rule routes",
            "wrong_sequential_certify_control",
        )

        slopes = measure_slopes(out)
        require_meaningful_deltas(slopes)
        return 0
    except (
        ImportError,
        RuntimeError,
        subprocess.CalledProcessError,
        ValueError,
    ) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
