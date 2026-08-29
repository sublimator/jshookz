#!/usr/bin/env python3
"""Pinned direct-Wasm fuel gates for recursive x-data provider-static routes.

The clean artifact measures one allocation-free certification pass, one
certification/index transaction, and canonical size/write over a retained
index.  The baseline lane is the declared negative control.  Numeric limits
were frozen from the first clean 2026-08-24 measurements made by this harness;
Amount, PathSet, and sealed-provider numbers are intentionally unrelated.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.metadata
import os
import re
import shlex
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

PINNED_WASI_CLANG_PREFIX = "clang version 22.1.0-wasi-sdk"
PINNED_WASI_VERSION = (
    "32.0",
    "wasi-libc: 2fc32bc81b9f",
    "llvm: 4434dabb6991",
    "llvm-version: 22.1.0",
    "config: f992bcc08219",
)
PINNED_WASMTIME_VERSION = "47.0.1"
PINNED_SOURCE_MANIFEST_SHA256 = (
    "8a6b94e338e69440e60ef2585663c7124fa9488dcb9531d8b6518008148c878c"
)
PINNED_ARTIFACT_SHA256 = (
    "16055ec2db4905a037b3aaaf8e021f9ce5a7038fb9639aad9f4fd866e261b4b9"
)
PINNED_COUNTER_ABI = "recursive-xdata-fuel-v1"

BUDGET = 100_000_000
ITERATIONS = (128, 256)
CLEAN_REPEATS = 3
ROUTE_CHECK_ITERATIONS = 37
BANKS = ("small", "large")
OPERATIONS = ("baseline", "certify", "index", "serialize")
MODES = tuple(
    f"recursive_{bank}_{operation}_repeat" for bank in BANKS for operation in OPERATIONS
)
HELPERS = (
    "recursive_baseline_once_c",
    "recursive_certify_once_c",
    "recursive_index_once_c",
    "recursive_serialize_once_c",
)
PROVIDER_STATIC_TUS = (
    "src/canonical_replacement.cpp",
    "src/canonical_serializer.cpp",
    "src/static_protocol.cpp",
    "src/recursive_index.cpp",
)
PROBE_TUS = (
    "tests/recursive_fuel_once.cpp",
    "tests/recursive_fuel_wasm.cpp",
)
COMPILE_FLAGS = (
    "-std=c++23",
    "-O2",
    "-fno-exceptions",
    "-fno-rtti",
    "-DCATL_XDATA_NO_BOOST_JSON",
)


@dataclass(frozen=True)
class Shape:
    elements: int
    byte_count: int
    scopes: int
    fields: int
    headers: int
    leaves: int
    index_bytes: int
    index_allocs: int
    index_frees: int
    fixture_sha256: str


SHAPES = {
    "small": Shape(
        elements=2,
        byte_count=48,
        scopes=4,
        fields=11,
        headers=14,
        leaves=8,
        index_bytes=300,
        index_allocs=2,
        index_frees=2,
        fixture_sha256=(
            "2f1f886c325c3c290fb4bd9b71672ddfb54e043086bb3181192af254b73e1e9b"
        ),
    ),
    "large": Shape(
        elements=24,
        byte_count=924,
        scopes=26,
        fields=99,
        headers=124,
        leaves=74,
        index_bytes=2412,
        index_allocs=8,
        index_frees=3,
        fixture_sha256=(
            "06ed94b5d173329850141cf0cb3d90a35b5dd3dd91715e8d72901a4e0d28b5a6"
        ),
    ),
}

# First clean slopes were 97 / 4507 / 12946 / 18569 for small and
# 97 / 31721 / 89192 / 176355 for large. Ceilings retain approximately five
# percent headroom. Any increase is a review-triggering rebaseline.
SLOPE_CEILINGS = {
    "recursive_small_baseline_repeat": 102.0,
    "recursive_small_certify_repeat": 4735.0,
    "recursive_small_index_repeat": 13600.0,
    "recursive_small_serialize_repeat": 19500.0,
    "recursive_large_baseline_repeat": 102.0,
    "recursive_large_certify_repeat": 33310.0,
    "recursive_large_index_repeat": 93660.0,
    "recursive_large_serialize_repeat": 185180.0,
}

# Absolute totals include deterministic fixture/index setup. These are not
# interchangeable with per-iteration slopes.
ABSOLUTE_CEILINGS = {
    ("recursive_small_baseline_repeat", 128): 208_000,
    ("recursive_small_baseline_repeat", 256): 221_000,
    ("recursive_small_certify_repeat", 128): 806_000,
    ("recursive_small_certify_repeat", 256): 1_411_000,
    ("recursive_small_index_repeat", 128): 1_949_000,
    ("recursive_small_index_repeat", 256): 3_689_000,
    ("recursive_small_serialize_repeat", 128): 2_710_000,
    ("recursive_small_serialize_repeat", 256): 5_206_000,
    ("recursive_large_baseline_repeat", 128): 208_000,
    ("recursive_large_baseline_repeat", 256): 221_000,
    ("recursive_large_certify_repeat", 128): 4_492_000,
    ("recursive_large_certify_repeat", 256): 8_755_000,
    ("recursive_large_index_repeat", 128): 12_276_000,
    ("recursive_large_index_repeat", 256): 24_263_000,
    ("recursive_large_serialize_repeat", 128): 24_083_000,
    ("recursive_large_serialize_repeat", 256): 47_784_000,
}

# Each production route must remain materially costlier than the three-byte
# baseline control. Floors retain roughly ninety percent of the clean deltas.
NEGATIVE_CONTROL_DELTA_FLOORS = {
    ("small", "certify"): 3_960.0,
    ("small", "index"): 11_560.0,
    ("small", "serialize"): 16_620.0,
    ("large", "certify"): 28_460.0,
    ("large", "index"): 80_180.0,
    ("large", "serialize"): 158_630.0,
}

# Clean large-minus-small deltas were 27,214 / 76,246 / 157,786. Both a
# floor and ceiling are frozen so a route cannot collapse or become nonlinear.
STRUCTURE_DELTA_BOUNDS = {
    "certify": (24_490.0, 29_940.0),
    "index": (68_620.0, 83_880.0),
    "serialize": (142_000.0, 173_570.0),
}
BASELINE_STRUCTURE_SPREAD_CEILING = 1.0
REPEAT_SPREAD_CEILING = 0

FUNC_HDR = re.compile(r"^[0-9a-fA-F]+ <([^>]*)>:\s*$")
CALL_IDX = re.compile(r"\bcall\t(\d+)\b")
COUNTER_ABI = re.compile(r"^counter_abi (\S+)$", re.MULTILINE)
SHAPE_LINE = re.compile(
    r"^shape (small|large) scopes=(\d+) fields=(\d+) headers=(\d+) "
    r"leaves=(\d+) index=(\d+)$",
    re.MULTILINE,
)
HELPER_COUNTS = re.compile(
    r"helper_counts baseline=(\d+) certify=(\d+) index=(\d+) serialize=(\d+)"
)
SCAN_COUNTS = re.compile(
    r"scan_counts passes=(\d+) scopes=(\d+) headers=(\d+) fields=(\d+) "
    r"leaves=(\d+)"
)
INDEX_COUNTS = re.compile(
    r"index_counts allocs=(\d+) frees=(\d+) bytes=(\d+) lookups=(\d+) "
    r"array=(\d+)"
)
SERIALIZER_COUNTS = re.compile(
    r"serializer_counts sizes=(\d+) writes=(\d+) bytes=(\d+)"
)
FIXTURE_HEX = re.compile(r"^fixture_hex (small|large) ([0-9a-f]+)$", re.MULTILINE)


class GateError(RuntimeError):
    pass


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def sha256_dependency(path: Path) -> str:
    data = path.read_bytes()
    if path.name == "runtime_profile_limits.h":
        # This generated header carries the full profile document hash as a
        # provenance comment. Raw-ABI/profile metadata changes can alter that
        # comment without changing any recursive-index constant or machine
        # code. Hash the semantic header so this fuel lane turns red only when
        # a compiled limit changes.
        data = re.sub(br"^// SHA-256: [0-9a-f]+\n", b"", data, flags=re.MULTILINE)
    return hashlib.sha256(data).hexdigest()


def validate_toolchain(wasi: Path) -> tuple[Path, Path]:
    clang = wasi / "bin" / "clang++"
    objdump = wasi / "bin" / "llvm-objdump"
    if not clang.is_file() or not objdump.is_file():
        raise GateError(f"wasi-sdk 32 tools not found under {wasi}")
    clang_version = subprocess.check_output(
        [str(clang), "--version"], text=True
    ).splitlines()[0]
    if not clang_version.startswith(PINNED_WASI_CLANG_PREFIX):
        raise GateError(f"wasi clang drift: {clang_version!r}")
    version = tuple((wasi / "VERSION").read_text().splitlines())
    if version != PINNED_WASI_VERSION:
        raise GateError(f"wasi-sdk revision drift: {version!r}")
    wasmtime_version = importlib.metadata.version("wasmtime")
    if wasmtime_version != PINNED_WASMTIME_VERSION:
        raise GateError(f"wasmtime drift: {wasmtime_version}")
    print(
        "toolchain "
        f"wasi_sdk={version[0]} llvm={version[2]} "
        f"wasmtime={wasmtime_version}"
    )
    print(f"compile_flags {' '.join(COMPILE_FLAGS)}")
    return clang, objdump


def include_flags(src: Path) -> list[str]:
    return [
        f"-I{src / 'includes'}",
        f"-I{src / 'core' / 'includes'}",
        f"-I{src / 'base58' / 'includes'}",
        f"-I{src / 'generated'}",
        f"-I{src / 'stubs'}",
        f"-I{src / 'tests'}",
    ]


def translation_units(src: Path) -> list[Path]:
    return [src / relative for relative in (*PROVIDER_STATIC_TUS, *PROBE_TUS)]


def validate_provider_static_inventory(src: Path) -> None:
    inventory = (src / "xdata_wasm_sources.cmake").read_text()
    match = re.search(
        r"set\(JSHOOKZ_XDATA_PROVIDER_STATIC_SOURCES\s+(.*?)\)",
        inventory,
        re.DOTALL,
    )
    if match is None:
        raise GateError("provider-static source inventory is missing")
    found = tuple(
        re.findall(r"\$\{CMAKE_CURRENT_LIST_DIR\}/([^\s)]+\.cpp)", match.group(1))
    )
    if found != PROVIDER_STATIC_TUS:
        raise GateError(
            f"provider-static inventory drift: {found}, expected {PROVIDER_STATIC_TUS}"
        )
    print(f"provider_static_inventory {','.join(found)}")


def dependency_manifest(src: Path, clang: Path, wasi: Path) -> tuple[str, list[str]]:
    dependencies: set[Path] = set()
    common = [
        str(clang),
        f"--sysroot={wasi / 'share' / 'wasi-sysroot'}",
        *COMPILE_FLAGS,
        *include_flags(src),
        "-MM",
    ]
    for tu in translation_units(src):
        output = subprocess.check_output([*common, str(tu)], text=True)
        flattened = output.replace("\\\n", " ")
        if ":" not in flattened:
            raise GateError(f"invalid dependency output for {tu}: {output!r}")
        for token in shlex.split(flattened.split(":", 1)[1]):
            candidate = Path(token)
            if not candidate.is_absolute():
                candidate = src / candidate
            candidate = candidate.resolve()
            try:
                candidate.relative_to(src.resolve())
            except ValueError:
                continue
            if candidate.is_file():
                dependencies.add(candidate)
    rows = []
    for path in sorted(dependencies):
        relative = path.relative_to(src).as_posix()
        rows.append(f"{relative}\0{sha256_dependency(path)}")
    digest = hashlib.sha256(("\n".join(rows) + "\n").encode()).hexdigest()
    return digest, rows


def validate_source_manifest(src: Path, clang: Path, wasi: Path) -> None:
    digest, rows = dependency_manifest(src, clang, wasi)
    if digest != PINNED_SOURCE_MANIFEST_SHA256:
        raise GateError(
            f"artifact source manifest drift: {digest}, "
            f"expected {PINNED_SOURCE_MANIFEST_SHA256}"
        )
    print(f"source_manifest sha256={digest} files={len(rows)}")
    for row in rows:
        relative, file_hash = row.split("\0", 1)
        print(f"source_sha256 {file_hash} {relative}")


def compile_wasm(
    src: Path,
    wasi: Path,
    clang: Path,
    output: Path,
    *,
    instrumented: bool,
    poison: str | None = None,
) -> None:
    command = [
        str(clang),
        f"--sysroot={wasi / 'share' / 'wasi-sysroot'}",
        *COMPILE_FLAGS,
        *include_flags(src),
    ]
    if instrumented:
        command.append("-DCATL_XDATA_RECURSIVE_FUEL_COUNTS")
    poison_definitions = {
        "certify_rescan": "CATL_XDATA_RECURSIVE_POISON_CERTIFY_RESCAN",
        "index_rescan": "CATL_XDATA_RECURSIVE_POISON_INDEX_RESCAN",
        "serializer_repeat": "CATL_XDATA_RECURSIVE_POISON_SERIALIZER_REPEAT",
    }
    if poison is not None:
        try:
            definition = poison_definitions[poison]
        except KeyError as error:
            raise GateError(f"unknown poison {poison}") from error
        command.append(f"-D{definition}")
    command.extend(f"-Wl,--export={helper}" for helper in HELPERS)
    command.extend(str(tu) for tu in translation_units(src))
    command.extend(("-o", str(output)))
    subprocess.check_call(command)


def read_uleb(data: bytes, offset: int) -> tuple[int, int]:
    value = 0
    shift = 0
    while True:
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7F) << shift
        if byte < 0x80:
            return value, offset
        shift += 7


def wasm_function_import_count(path: Path) -> int:
    data = path.read_bytes()
    if data[:4] != b"\0asm":
        raise GateError(f"not a Wasm artifact: {path}")
    offset = 8
    while offset < len(data):
        section = data[offset]
        offset += 1
        size, offset = read_uleb(data, offset)
        body = data[offset : offset + size]
        offset += size
        if section != 2:
            continue
        count, cursor = read_uleb(body, 0)
        functions = 0
        for _ in range(count):
            length, cursor = read_uleb(body, cursor)
            cursor += length
            length, cursor = read_uleb(body, cursor)
            cursor += length
            kind = body[cursor]
            cursor += 1
            if kind == 0:
                _, cursor = read_uleb(body, cursor)
                functions += 1
            elif kind == 1:
                cursor += 1
                flags = body[cursor]
                cursor += 1
                _, cursor = read_uleb(body, cursor)
                if flags & 1:
                    _, cursor = read_uleb(body, cursor)
            elif kind == 2:
                flags = body[cursor]
                cursor += 1
                _, cursor = read_uleb(body, cursor)
                if flags & 1:
                    _, cursor = read_uleb(body, cursor)
            elif kind == 3:
                cursor += 2
            else:
                raise GateError(f"unknown Wasm import kind {kind}")
        return functions
    return 0


def parse_objdump_functions(
    dump: str, imported_functions: int
) -> tuple[dict[int, str], dict[int, str]]:
    names: dict[int, str] = {}
    bodies: dict[int, list[str]] = {}
    current: int | None = None
    local_index = -1
    for line in dump.splitlines():
        header = FUNC_HDR.match(line)
        if header:
            name = header.group(1)
            if name == "CODE":
                current = None
                continue
            local_index += 1
            index = imported_functions + local_index
            names[index] = name or f"func[{index}]"
            bodies[index] = []
            current = index
        elif current is not None:
            bodies[current].append(line)
    return names, {index: "\n".join(lines) for index, lines in bodies.items()}


def call_indices(body: str) -> set[int]:
    calls: set[int] = set()
    for line in body.splitlines():
        if "call_indirect" not in line:
            calls.update(int(match.group(1)) for match in CALL_IDX.finditer(line))
    return calls


def require_direct_reachability(objdump: Path, wasm: Path) -> None:
    dump = subprocess.check_output(
        [str(objdump), "-d", str(wasm)], text=True, stderr=subprocess.STDOUT
    )
    names, bodies = parse_objdump_functions(dump, wasm_function_import_count(wasm))
    by_name = {name: index for index, name in names.items()}
    if "_start" not in by_name:
        raise GateError("disassembly has no _start")
    reachable: set[int] = set()
    stack = [by_name["_start"]]
    while stack:
        index = stack.pop()
        if index in reachable:
            continue
        reachable.add(index)
        stack.extend(call_indices(bodies.get(index, "")) - reachable)
    for helper in HELPERS:
        helper_index = by_name.get(helper)
        if helper_index is None or helper_index not in reachable:
            raise GateError(f"{helper} is not directly reachable from _start")
        callers = sorted(
            names[index]
            for index in reachable
            if helper_index in call_indices(bodies.get(index, ""))
        )
        if not callers:
            raise GateError(f"{helper} has no direct caller in the _start graph")
        print(f"helper_callgraph {helper} callers={','.join(callers)}")


class Meter:
    def __init__(self, wasm: Path):
        from wasmtime import Config, Engine, Module

        config = Config()
        config.consume_fuel = True
        self.engine = Engine(config)
        self.module = Module.from_file(self.engine, str(wasm))

    def run(self, mode: str, iterations: int) -> tuple[int, str]:
        from wasmtime import Linker, Store, WasiConfig

        store = Store(self.engine)
        store.set_fuel(BUDGET)
        wasi = WasiConfig()
        wasi.argv = ["recursive_fuel", mode, str(iterations)]
        with (
            tempfile.NamedTemporaryFile(delete=False) as stdout_file,
            tempfile.NamedTemporaryFile(delete=False) as stderr_file,
        ):
            stdout_path = Path(stdout_file.name)
            stderr_path = Path(stderr_file.name)
        try:
            wasi.stdout_file = str(stdout_path)
            wasi.stderr_file = str(stderr_path)
            linker = Linker(self.engine)
            linker.define_wasi()
            store.set_wasi(wasi)
            instance = linker.instantiate(store, self.module)
            instance.exports(store)["_start"](store)
            stderr = stderr_path.read_text()
            if stderr:
                raise GateError(f"{mode}: unexpected stderr: {stderr!r}")
            return BUDGET - store.get_fuel(), stdout_path.read_text()
        finally:
            stdout_path.unlink(missing_ok=True)
            stderr_path.unlink(missing_ok=True)


def mode_parts(mode: str) -> tuple[str, str]:
    match = re.fullmatch(r"recursive_(small|large)_(\w+)_repeat", mode)
    if match is None or match.group(2) not in OPERATIONS:
        raise GateError(f"unknown mode {mode}")
    return match.group(1), match.group(2)


def validate_common_output(stdout: str) -> None:
    abi = COUNTER_ABI.search(stdout)
    if abi is None or abi.group(1) != PINNED_COUNTER_ABI:
        raise GateError(f"counter ABI drift: {stdout!r}")
    shapes = {
        match.group(1): tuple(int(value) for value in match.groups()[1:])
        for match in SHAPE_LINE.finditer(stdout)
    }
    expected = {
        bank: (
            shape.scopes,
            shape.fields,
            shape.headers,
            shape.leaves,
            shape.index_bytes,
        )
        for bank, shape in SHAPES.items()
    }
    if shapes != expected:
        raise GateError(f"fixture shape drift: {shapes}, expected {expected}")


def validate_mode_output(mode: str, stdout: str) -> None:
    validate_common_output(stdout)
    bank, _ = mode_parts(mode)
    marker = (
        f"coverage fixture={bank} elements={SHAPES[bank].elements} "
        f"bytes={SHAPES[bank].byte_count}"
    )
    if marker not in stdout or mode not in stdout or "FAIL" in stdout:
        raise GateError(f"{mode}: invalid probe markers: {stdout!r}")


def parse_counts(stdout: str) -> dict[str, tuple[int, ...]]:
    patterns = {
        "helper": HELPER_COUNTS,
        "scan": SCAN_COUNTS,
        "index": INDEX_COUNTS,
        "serializer": SERIALIZER_COUNTS,
    }
    parsed: dict[str, tuple[int, ...]] = {}
    for label, pattern in patterns.items():
        match = pattern.search(stdout)
        if match is None:
            raise GateError(f"missing {label} route counts: {stdout!r}")
        parsed[label] = tuple(int(value) for value in match.groups())
    return parsed


def expected_counts(
    bank: str, operation: str, iterations: int
) -> dict[str, tuple[int, ...]]:
    shape = SHAPES[bank]
    helpers = tuple(
        iterations if candidate == operation else 0 for candidate in OPERATIONS
    )
    zeros_scan = (0, 0, 0, 0, 0)
    zeros_index = (0, 0, 0, 0, 0)
    zeros_serializer = (0, 0, 0)
    expected = {
        "helper": helpers,
        "scan": zeros_scan,
        "index": zeros_index,
        "serializer": zeros_serializer,
    }
    if operation in ("certify", "index"):
        expected["scan"] = (
            iterations,
            iterations * shape.scopes,
            iterations * shape.headers,
            iterations * shape.fields,
            iterations * shape.leaves,
        )
    if operation == "index":
        expected["index"] = (
            iterations * shape.index_allocs,
            iterations * shape.index_frees,
            iterations * shape.index_bytes,
            iterations * 2,
            iterations,
        )
    if operation == "serialize":
        expected["serializer"] = (
            iterations,
            iterations,
            iterations * shape.byte_count,
        )
    return expected


def require_mode_counts(meter: Meter, mode: str, iterations: int) -> None:
    _, stdout = meter.run(mode, iterations)
    validate_mode_output(mode, stdout)
    got = parse_counts(stdout)
    bank, operation = mode_parts(mode)
    expected = expected_counts(bank, operation, iterations)
    if got != expected:
        labels = [label for label in expected if got[label] != expected[label]]
        raise GateError(
            f"{mode}: {','.join(labels)} route counts {got}, expected {expected}"
        )
    print(f"route_counts {mode} n={iterations} {got}")


def validate_fixtures(meter: Meter) -> None:
    for bank, shape in SHAPES.items():
        _, stdout = meter.run(f"dump_{bank}", 0)
        validate_common_output(stdout)
        match = FIXTURE_HEX.search(stdout)
        if match is None or match.group(1) != bank:
            raise GateError(f"missing {bank} fixture dump")
        fixture = bytes.fromhex(match.group(2))
        digest = hashlib.sha256(fixture).hexdigest()
        if len(fixture) != shape.byte_count or digest != shape.fixture_sha256:
            raise GateError(
                f"{bank} fixture drift: bytes={len(fixture)} sha256={digest}"
            )
        print(f"fixture {bank} bytes={len(fixture)} sha256={digest}")


def measure_clean(meter: Meter) -> tuple[dict[tuple[str, int], int], dict[str, float]]:
    rows: dict[tuple[str, int], int] = {}
    for mode in MODES:
        for iterations in ITERATIONS:
            samples: list[int] = []
            outputs: list[str] = []
            for _ in range(CLEAN_REPEATS):
                used, stdout = meter.run(mode, iterations)
                validate_mode_output(mode, stdout)
                samples.append(used)
                outputs.append(stdout)
            spread = max(samples) - min(samples)
            if spread > REPEAT_SPREAD_CEILING or len(set(outputs)) != 1:
                raise GateError(
                    f"{mode} n={iterations}: nondeterministic repeats {samples}"
                )
            rows[(mode, iterations)] = samples[0]
            ceiling = ABSOLUTE_CEILINGS[(mode, iterations)]
            print(
                f"clean_repeat {mode} n={iterations} repeats={CLEAN_REPEATS} "
                f"used={samples[0]} spread={spread} absolute_ceiling={ceiling}"
            )
            if samples[0] > ceiling:
                raise GateError(
                    f"{mode} n={iterations}: fuel {samples[0]} exceeds {ceiling}"
                )
    low, high = ITERATIONS
    slopes = {
        mode: (rows[(mode, high)] - rows[(mode, low)]) / (high - low) for mode in MODES
    }
    for mode, slope in slopes.items():
        ceiling = SLOPE_CEILINGS[mode]
        print(f"slope {mode}={slope:.3f} ceiling={ceiling:.3f}")
        if slope > ceiling:
            raise GateError(f"{mode}: slope {slope} exceeds {ceiling}")
    return rows, slopes


def require_delta_and_scale_gates(slopes: dict[str, float]) -> None:
    def slope(bank: str, operation: str) -> float:
        return slopes[f"recursive_{bank}_{operation}_repeat"]

    for (bank, operation), floor in NEGATIVE_CONTROL_DELTA_FLOORS.items():
        delta = slope(bank, operation) - slope(bank, "baseline")
        print(
            f"negative_control_delta bank={bank} route={operation} "
            f"value={delta:.3f} floor={floor:.3f}"
        )
        if delta < floor:
            raise GateError(
                f"{bank} {operation}: negative-control delta {delta} below {floor}"
            )
    baseline_spread = abs(slope("large", "baseline") - slope("small", "baseline"))
    print(
        f"baseline_structure_spread={baseline_spread:.3f} "
        f"ceiling={BASELINE_STRUCTURE_SPREAD_CEILING:.3f}"
    )
    if baseline_spread > BASELINE_STRUCTURE_SPREAD_CEILING:
        raise GateError(
            f"baseline unexpectedly depends on fixture size: {baseline_spread}"
        )
    for operation, (floor, ceiling) in STRUCTURE_DELTA_BOUNDS.items():
        delta = slope("large", operation) - slope("small", operation)
        print(
            f"structure_delta route={operation} value={delta:.3f} "
            f"floor={floor:.3f} ceiling={ceiling:.3f}"
        )
        if not floor <= delta <= ceiling:
            raise GateError(
                f"{operation}: structure delta {delta} outside [{floor}, {ceiling}]"
            )


def require_poison_route_red(
    meter: Meter, mode: str, expected_label: str, poison: str
) -> None:
    try:
        require_mode_counts(meter, mode, ROUTE_CHECK_ITERATIONS)
    except GateError as error:
        if expected_label not in str(error):
            raise GateError(f"{poison} failed for the wrong reason: {error}") from error
        print(f"poison_route_red {poison} {error}")
        return
    raise GateError(f"{poison} did not turn the route checker red")


def require_poison_fuel_red(meter: Meter, mode: str, poison: str) -> None:
    low, high = ITERATIONS
    samples = {}
    for iterations in ITERATIONS:
        used, stdout = meter.run(mode, iterations)
        validate_mode_output(mode, stdout)
        samples[iterations] = used
    slope = (samples[high] - samples[low]) / (high - low)
    slope_red = slope > SLOPE_CEILINGS[mode]
    absolute_red = any(
        samples[iterations] > ABSOLUTE_CEILINGS[(mode, iterations)]
        for iterations in ITERATIONS
    )
    if not slope_red or not absolute_red:
        raise GateError(
            f"{poison} did not turn both fuel checks red: "
            f"slope={slope} slope_red={slope_red} absolute_red={absolute_red}"
        )
    print(
        f"poison_fuel_red {poison} mode={mode} slope={slope:.3f} "
        f"clean_ceiling={SLOPE_CEILINGS[mode]:.3f} samples={samples}"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--src", type=Path, required=True)
    parser.add_argument("--wasi", type=Path, default=None)
    parser.add_argument("--keep-wasm", type=Path, default=None)
    args = parser.parse_args()
    src = args.src.resolve()
    wasi = (
        args.wasi
        or Path(
            os.environ.get(
                "WASI_SDK_PATH",
                str(Path.home() / ".local/share/mise/installs/wasi-sdk/32/wasi-sdk"),
            )
        )
    ).resolve()
    try:
        clang, objdump = validate_toolchain(wasi)
        validate_provider_static_inventory(src)
        validate_source_manifest(src, clang, wasi)
        with tempfile.TemporaryDirectory() as temporary:
            work = args.keep_wasm.parent if args.keep_wasm else Path(temporary)
            work.mkdir(parents=True, exist_ok=True)
            clean = args.keep_wasm or work / "recursive_fuel.wasm"
            compile_wasm(src, wasi, clang, clean, instrumented=False)
            artifact_hash = sha256_file(clean)
            if artifact_hash != PINNED_ARTIFACT_SHA256:
                raise GateError(
                    f"clean artifact drift: {artifact_hash}, "
                    f"expected {PINNED_ARTIFACT_SHA256}"
                )
            print(
                f"artifact sha256={artifact_hash} bytes={clean.stat().st_size} "
                f"budget={BUDGET}"
            )
            require_direct_reachability(objdump, clean)
            clean_meter = Meter(clean)
            validate_fixtures(clean_meter)

            counted = work / "recursive_fuel_counts.wasm"
            compile_wasm(src, wasi, clang, counted, instrumented=True)
            counted_meter = Meter(counted)
            for mode in MODES:
                require_mode_counts(counted_meter, mode, ROUTE_CHECK_ITERATIONS)

            poison_cases = (
                ("certify_rescan", "recursive_large_certify_repeat", "scan"),
                ("index_rescan", "recursive_large_index_repeat", "scan"),
                (
                    "serializer_repeat",
                    "recursive_large_serialize_repeat",
                    "serializer",
                ),
            )
            for poison, mode, expected_label in poison_cases:
                counted_poison = work / f"recursive_fuel_{poison}_counts.wasm"
                compile_wasm(
                    src,
                    wasi,
                    clang,
                    counted_poison,
                    instrumented=True,
                    poison=poison,
                )
                require_poison_route_red(
                    Meter(counted_poison), mode, expected_label, poison
                )
                fuel_poison = work / f"recursive_fuel_{poison}.wasm"
                compile_wasm(
                    src,
                    wasi,
                    clang,
                    fuel_poison,
                    instrumented=False,
                    poison=poison,
                )
                require_poison_fuel_red(Meter(fuel_poison), mode, poison)

            _, slopes = measure_clean(clean_meter)
            require_delta_and_scale_gates(slopes)
        print("PASS recursive direct-Wasm fuel/complexity gate")
        return 0
    except (
        GateError,
        ImportError,
        OSError,
        subprocess.CalledProcessError,
        ValueError,
    ) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
