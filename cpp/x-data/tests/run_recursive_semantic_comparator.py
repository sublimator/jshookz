#!/usr/bin/env python3
"""Reproduce full-vs-semantic-skip recursive scanner measurements.

This is diagnostic tooling, not a product parser.  It compiles the release
scanner unchanged, applies the retained semantic-only mutant to a temporary
copy, and measures both artifacts over the same pinned oracle corpus.  The
mutant skips only payload predicates whose extent is already bounded: AccountID
and Vector256 shape, Amount and Number canonicality, Issue validity, and bridge
door width.  Header/VL framing, wire bounds, field lookup, container traversal,
terminators, duplicates, limits, extent discovery, PathSet walking, index
compaction, and final index validation remain intact.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.metadata
import json
import os
import platform
import re
import shlex
import statistics
import subprocess
import sys
import tempfile
import time
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
PINNED_ORACLE_COMMIT = "cb829d7657607643f0bdc29c65f9a41fbd86a688"
PINNED_ORACLE_SHA256 = (
    "5653a415429a773be7d1cf50cef6f2de2f525b4e1c02243a76ee8561124a1df9"
)
PINNED_VALID_CORPUS_SHA256 = (
    "ddd33400bbd7215e069e5f63e4a1c82a4b43e11ead450d8e94704728592d929b"
)
PINNED_CONTROL_CORPUS_SHA256 = (
    "d4455924bb2c062e63170a7f2ca98d5b6c836cca7cc50448ca1c495da8af2841"
)
PINNED_SOURCE_MANIFEST_SHA256 = (
    "f1b74d0743317ce5bf0938e6df69b4e7353af51dec57abbd8558508de0123f25"
)
PINNED_MUTANT_PATCH_SHA256 = (
    "968b068ea6f7b748bd47168f2a0afa76291d9a5adc2a48f74b676c86867d6202"
)
PINNED_FULL_ARTIFACT_SHA256 = (
    "f6a748b55185db8055fcdb3b0e15668a0c53c88d89505d719ff8f8016e6dd6d1"
)
PINNED_MUTANT_ARTIFACT_SHA256 = (
    "a4048220039ef0546e49a9ba2e98b6d02b3ba8b365b423477211d83c23385f9c"
)
PINNED_COMPARATOR_ABI = "recursive-semantic-comparator-v1"

CONTROL_IDS = (
    "stobject-account-vl-1",
    "stobject-account-vl-19",
    "stobject-account-vl-21",
    "stobject-iou-zero-issuer",
    "stobject-iou-native-currency",
    "stobject-iou-tiny-mantissa",
)
COMPILE_FLAGS = (
    "-std=c++23",
    "-O2",
    "-fno-exceptions",
    "-fno-rtti",
    "-DCATL_XDATA_NO_BOOST_JSON",
)
FUEL_BUDGET = 1_000_000_000
FUEL_ITERATIONS = (32, 64)
FUEL_REPEATS = 3
WALL_ITERATIONS = 256
WALL_WARMUPS = 2
WALL_REPEATS = 9
VALID_MODES = ("baseline", "certify", "index")

MODE_LINE = re.compile(
    r"^mode (\w+) iterations=(\d+) cases=(\d+) accepted=(\d+) "
    r"checksum=([0-9a-f]+)$",
    re.MULTILINE,
)
HEAP_LINE = re.compile(
    r"^heap calls=(\d+) frees=(\d+) requested=(\d+) peak=(\d+) live=(\d+)$",
    re.MULTILINE,
)


class ComparatorError(RuntimeError):
    pass


@dataclass(frozen=True)
class CorpusCase:
    identifier: str
    blob: bytes


@dataclass(frozen=True)
class Run:
    fuel: int
    wall_ns: int
    mode: str
    iterations: int
    cases: int
    accepted: int
    checksum: str
    heap: tuple[int, int, int, int, int]
    stdout: str


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    return sha256_bytes(path.read_bytes())


def require_pin(label: str, observed: str, expected: str) -> None:
    if re.fullmatch(r"[0-9a-f]{64}", expected) is None:
        raise ComparatorError(f"{label} has no exact SHA-256 pin")
    if observed != expected:
        raise ComparatorError(f"{label} drift: {observed}, expected {expected}")


def validate_toolchain(wasi: Path) -> Path:
    clang = wasi / "bin" / "clang++"
    if not clang.is_file():
        raise ComparatorError(f"wasi clang not found under {wasi}")
    clang_version = subprocess.check_output(
        [str(clang), "--version"], text=True
    ).splitlines()[0]
    if not clang_version.startswith(PINNED_WASI_CLANG_PREFIX):
        raise ComparatorError(f"wasi clang drift: {clang_version!r}")
    version = tuple((wasi / "VERSION").read_text().splitlines())
    if version != PINNED_WASI_VERSION:
        raise ComparatorError(f"wasi-sdk revision drift: {version!r}")
    wasmtime_version = importlib.metadata.version("wasmtime")
    if wasmtime_version != PINNED_WASMTIME_VERSION:
        raise ComparatorError(f"wasmtime drift: {wasmtime_version}")
    print(
        "toolchain "
        f"wasi_sdk={version[0]} llvm={version[2]} "
        f"wasmtime={wasmtime_version} python={platform.python_version()}"
    )
    print(f"compile_flags {' '.join(COMPILE_FLAGS)}")
    cpu = platform.processor() or "unknown"
    if platform.system() == "Darwin":
        try:
            cpu = subprocess.check_output(
                ["sysctl", "-n", "machdep.cpu.brand_string"], text=True
            ).strip()
        except (OSError, subprocess.CalledProcessError):
            pass
    print(
        "wall_environment "
        f"system={platform.system()} release={platform.release()} "
        f"machine={platform.machine()} cpu={cpu!r}"
    )
    return clang


def corpus_digest(cases: list[CorpusCase]) -> str:
    encoded = "\n".join(f"{case.identifier}\0{case.blob.hex()}" for case in cases)
    return sha256_bytes((encoded + "\n").encode())


def load_corpus(src: Path) -> tuple[list[CorpusCase], list[CorpusCase]]:
    path = src / "tests" / "oracle_corpus.json"
    require_pin("oracle_sha256", sha256_file(path), PINNED_ORACLE_SHA256)
    root = json.loads(path.read_text())
    if root.get("oracle_commit") != PINNED_ORACLE_COMMIT:
        raise ComparatorError("oracle corpus commit drift")
    by_id = {item["id"]: item for item in root["cases"]}
    valid = [
        CorpusCase(item["id"], bytes.fromhex(item["blob"]))
        for item in root["cases"]
        if item["codec_type"] == "stobject"
        and item["expect"] == "accept"
        and not item.get("trailing_ok", False)
    ]
    controls = []
    for identifier in CONTROL_IDS:
        item = by_id.get(identifier)
        if (
            item is None
            or item["codec_type"] != "stobject"
            or item["expect"] != "reject"
        ):
            raise ComparatorError(
                f"semantic control missing or reclassified: {identifier}"
            )
        controls.append(CorpusCase(identifier, bytes.fromhex(item["blob"])))
    require_pin("valid_corpus_sha256", corpus_digest(valid), PINNED_VALID_CORPUS_SHA256)
    require_pin(
        "control_corpus_sha256",
        corpus_digest(controls),
        PINNED_CONTROL_CORPUS_SHA256,
    )
    print(
        "corpus "
        f"oracle_commit={PINNED_ORACLE_COMMIT} "
        f"valid_cases={len(valid)} valid_bytes={sum(len(c.blob) for c in valid)} "
        f"valid_sha256={corpus_digest(valid)} "
        f"controls={len(controls)} control_bytes={sum(len(c.blob) for c in controls)} "
        f"control_sha256={corpus_digest(controls)}"
    )
    return valid, controls


def byte_array(name: str, data: bytes) -> str:
    values = ", ".join(f"0x{value:02x}" for value in data)
    return f"inline constexpr std::uint8_t {name}[] = {{{values}}};\n"


def write_corpus_header(
    path: Path, valid: list[CorpusCase], controls: list[CorpusCase]
) -> None:
    chunks = [
        "#pragma once\n\n",
        "#include <array>\n#include <cstdint>\n\n",
        "struct ComparatorCase {\n",
        "  char const *identifier;\n",
        "  std::uint8_t const *bytes;\n",
        "  std::uint32_t size;\n",
        "};\n\n",
    ]
    for prefix, cases in (("valid", valid), ("control", controls)):
        for ordinal, case in enumerate(cases):
            chunks.append(byte_array(f"comparator_{prefix}_{ordinal}", case.blob))
        chunks.append(
            f"\ninline constexpr std::array<ComparatorCase, {len(cases)}> "
            f"comparator_{'valid_cases' if prefix == 'valid' else 'semantic_controls'}{{{{\n"
        )
        for ordinal, case in enumerate(cases):
            escaped_id = case.identifier.replace('"', '\\"')
            chunks.append(
                f'  {{"{escaped_id}", comparator_{prefix}_{ordinal}, '
                f"sizeof(comparator_{prefix}_{ordinal})}},\n"
            )
        chunks.append("}};\n\n")
    path.write_text("".join(chunks))


def include_flags(src: Path, generated: Path) -> list[str]:
    return [
        f"-I{generated}",
        f"-I{src / 'includes'}",
        f"-I{src / 'core' / 'includes'}",
        f"-I{src / 'base58' / 'includes'}",
        f"-I{src / 'generated'}",
        f"-I{src / 'stubs'}",
        f"-I{src / 'tests'}",
    ]


def full_translation_units(src: Path) -> tuple[Path, ...]:
    return (
        src / "src" / "static_protocol.cpp",
        src / "src" / "recursive_index.cpp",
        src / "tests" / "recursive_semantic_comparator_wasm.cpp",
    )


def dependency_manifest(
    src: Path, generated: Path, clang: Path, wasi: Path
) -> tuple[str, list[str]]:
    dependencies: set[Path] = set()
    common = [
        str(clang),
        f"--sysroot={wasi / 'share' / 'wasi-sysroot'}",
        *COMPILE_FLAGS,
        *include_flags(src, generated),
        "-MM",
    ]
    for translation_unit in full_translation_units(src):
        output = subprocess.check_output([*common, str(translation_unit)], text=True)
        flattened = output.replace("\\\n", " ")
        if ":" not in flattened:
            raise ComparatorError(f"invalid dependency output for {translation_unit}")
        for token in shlex.split(flattened.split(":", 1)[1]):
            candidate = Path(token)
            if not candidate.is_absolute():
                candidate = src / candidate
            candidate = candidate.resolve()
            try:
                relative = candidate.relative_to(src)
            except ValueError:
                continue
            if candidate.is_file():
                dependencies.add(src / relative)
    rows = [
        f"{path.relative_to(src).as_posix()}\0{sha256_file(path)}"
        for path in sorted(dependencies)
    ]
    return sha256_bytes(("\n".join(rows) + "\n").encode()), rows


def apply_mutant(src: Path, work: Path) -> Path:
    original = src / "src" / "recursive_index.cpp"
    mutant = work / "recursive_index_semantic_skip.cpp"
    mutant.write_bytes(original.read_bytes())
    patch_path = src / "tests" / "recursive_semantic_skip.patch"
    require_pin(
        "mutant_patch_sha256", sha256_file(patch_path), PINNED_MUTANT_PATCH_SHA256
    )
    process = subprocess.run(
        ["patch", "--batch", "--fuzz=0", str(mutant), str(patch_path)],
        text=True,
        capture_output=True,
        check=False,
    )
    if process.returncode != 0:
        raise ComparatorError(
            "semantic mutant did not apply exactly: "
            f"stdout={process.stdout!r} stderr={process.stderr!r}"
        )
    return mutant


def compile_wasm(
    src: Path,
    generated: Path,
    wasi: Path,
    clang: Path,
    scanner: Path,
    output: Path,
) -> None:
    command = [
        str(clang),
        f"--sysroot={wasi / 'share' / 'wasi-sysroot'}",
        *COMPILE_FLAGS,
        *include_flags(src, generated),
        str(src / "src" / "static_protocol.cpp"),
        str(scanner),
        str(src / "tests" / "recursive_semantic_comparator_wasm.cpp"),
        "-o",
        str(output),
    ]
    print(
        "compile "
        + " ".join(
            "<work>" + value[len(str(output.parent)) :]
            if value.startswith(str(output.parent))
            else value
            for value in command
        )
    )
    subprocess.check_call(command)


class Meter:
    def __init__(self, wasm: Path):
        from wasmtime import Config, Engine, Module

        config = Config()
        config.consume_fuel = True
        self.engine = Engine(config)
        self.module = Module.from_file(self.engine, str(wasm))

    def run(self, mode: str, iterations: int) -> Run:
        from wasmtime import Linker, Store, WasiConfig

        store = Store(self.engine)
        store.set_fuel(FUEL_BUDGET)
        wasi = WasiConfig()
        wasi.argv = ["recursive_semantic_comparator", mode, str(iterations)]
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
            started = time.perf_counter_ns()
            instance.exports(store)["_start"](store)
            wall_ns = time.perf_counter_ns() - started
            stderr = stderr_path.read_text()
            if stderr:
                raise ComparatorError(f"{mode}: unexpected stderr: {stderr!r}")
            stdout = stdout_path.read_text()
            if f"comparator_abi {PINNED_COMPARATOR_ABI}\n" not in stdout:
                raise ComparatorError(f"{mode}: comparator ABI missing")
            mode_match = MODE_LINE.search(stdout)
            heap_match = HEAP_LINE.search(stdout)
            if mode_match is None or heap_match is None:
                raise ComparatorError(f"{mode}: malformed output: {stdout!r}")
            return Run(
                fuel=FUEL_BUDGET - store.get_fuel(),
                wall_ns=wall_ns,
                mode=mode_match.group(1),
                iterations=int(mode_match.group(2)),
                cases=int(mode_match.group(3)),
                accepted=int(mode_match.group(4)),
                checksum=mode_match.group(5),
                heap=tuple(int(value) for value in heap_match.groups()),
                stdout=stdout,
            )
        finally:
            stdout_path.unlink(missing_ok=True)
            stderr_path.unlink(missing_ok=True)


def require_valid_run(run: Run, mode: str, iterations: int, case_count: int) -> None:
    expected = case_count * iterations
    if (
        run.mode != mode
        or run.iterations != iterations
        or run.cases != case_count
        or run.accepted != expected
        or run.heap[-1] != 0
    ):
        raise ComparatorError(f"invalid {mode} result: {run}")


def require_controls(full: Meter, mutant: Meter, control_count: int) -> tuple[Run, Run]:
    full_run = full.run("control", 1)
    mutant_run = mutant.run("control", 1)
    if full_run.cases != control_count or full_run.accepted != 0:
        raise ComparatorError(
            f"full scanner did not reject semantic controls: {full_run}"
        )
    if mutant_run.cases != control_count or mutant_run.accepted != control_count:
        raise ComparatorError(
            f"semantic mutant is not active on every control: {mutant_run}"
        )
    print(
        "mutation_control "
        f"full_accepted={full_run.accepted}/{control_count} "
        f"skip_accepted={mutant_run.accepted}/{control_count}"
    )
    return full_run, mutant_run


def measure_fuel(
    meters: dict[str, Meter], case_count: int
) -> dict[str, dict[str, float]]:
    rows: dict[str, dict[str, float]] = {name: {} for name in meters}
    observed_outputs: dict[tuple[str, int], str] = {}
    low, high = FUEL_ITERATIONS
    for variant, meter in meters.items():
        for mode in VALID_MODES:
            totals: dict[int, int] = {}
            for iterations in FUEL_ITERATIONS:
                samples = [meter.run(mode, iterations) for _ in range(FUEL_REPEATS)]
                for sample in samples:
                    require_valid_run(sample, mode, iterations, case_count)
                fuels = [sample.fuel for sample in samples]
                if (
                    len(set(fuels)) != 1
                    or len({sample.stdout for sample in samples}) != 1
                ):
                    raise ComparatorError(
                        f"{variant} {mode} n={iterations}: nondeterministic {fuels}"
                    )
                previous = observed_outputs.setdefault(
                    (mode, iterations), samples[0].stdout
                )
                if previous != samples[0].stdout:
                    raise ComparatorError(
                        f"full and semantic-skip outputs differ for valid {mode} corpus"
                    )
                totals[iterations] = fuels[0]
                print(
                    "fuel_sample "
                    f"variant={variant} mode={mode} n={iterations} "
                    f"repeats={FUEL_REPEATS} used={fuels[0]} spread=0"
                )
            rows[variant][mode] = (totals[high] - totals[low]) / (high - low)
            print(
                f"fuel_slope variant={variant} mode={mode} "
                f"per_corpus={rows[variant][mode]:.3f}"
            )
    for mode in ("certify", "index"):
        full = rows["full"][mode]
        skip = rows["semantic_skip"][mode]
        saving = full - skip
        percent = saving * 100 / full
        print(
            f"fuel_comparison mode={mode} full={full:.3f} skip={skip:.3f} "
            f"saving={saving:.3f} percent={percent:.4f}"
        )
    return rows


def measure_heap(meters: dict[str, Meter], case_count: int) -> None:
    results = {name: meter.run("index", 1) for name, meter in meters.items()}
    for name, run in results.items():
        require_valid_run(run, "index", 1, case_count)
        calls, frees, requested, peak, live = run.heap
        print(
            f"heap_comparison variant={name} calls={calls} frees={frees} "
            f"requested={requested} peak={peak} live={live}"
        )
    if results["full"].heap != results["semantic_skip"].heap:
        raise ComparatorError("semantic skip changed index allocation behavior")


def measure_wall(meters: dict[str, Meter], case_count: int) -> None:
    outputs: dict[str, str] = {}
    medians: dict[str, dict[str, float]] = {name: {} for name in meters}
    for variant, meter in meters.items():
        for mode in ("certify", "index"):
            for _ in range(WALL_WARMUPS):
                warmup = meter.run(mode, WALL_ITERATIONS)
                require_valid_run(warmup, mode, WALL_ITERATIONS, case_count)
            samples = []
            for _ in range(WALL_REPEATS):
                sample = meter.run(mode, WALL_ITERATIONS)
                require_valid_run(sample, mode, WALL_ITERATIONS, case_count)
                samples.append(sample.wall_ns / WALL_ITERATIONS)
                prior = outputs.setdefault(mode, sample.stdout)
                if prior != sample.stdout:
                    raise ComparatorError(
                        f"full and semantic-skip outputs differ during wall {mode}"
                    )
            median = statistics.median(samples)
            medians[variant][mode] = median
            deviations = [abs(sample - median) for sample in samples]
            mad = statistics.median(deviations)
            print(
                f"wall_comparison variant={variant} mode={mode} "
                f"iterations={WALL_ITERATIONS} warmups={WALL_WARMUPS} "
                f"samples={WALL_REPEATS} median_ns_per_corpus={median:.3f} "
                f"mad={mad:.3f} min={min(samples):.3f} max={max(samples):.3f}"
            )
    for mode in ("certify", "index"):
        full = medians["full"][mode]
        skip = medians["semantic_skip"][mode]
        saving = full - skip
        print(
            f"wall_delta mode={mode} full={full:.3f} skip={skip:.3f} "
            f"saving={saving:.3f} percent={saving * 100 / full:.4f}"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--src", type=Path, required=True)
    parser.add_argument("--wasi", type=Path, default=None)
    parser.add_argument("--keep-artifacts", type=Path, default=None)
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
        clang = validate_toolchain(wasi)
        valid, controls = load_corpus(src)
        with tempfile.TemporaryDirectory() as temporary:
            work = args.keep_artifacts or Path(temporary)
            work.mkdir(parents=True, exist_ok=True)
            generated = work / "generated"
            generated.mkdir(parents=True, exist_ok=True)
            write_corpus_header(
                generated / "recursive_semantic_comparator_corpus.h",
                valid,
                controls,
            )
            source_digest, source_rows = dependency_manifest(
                src, generated, clang, wasi
            )
            require_pin(
                "source_manifest_sha256",
                source_digest,
                PINNED_SOURCE_MANIFEST_SHA256,
            )
            print(f"source_manifest sha256={source_digest} files={len(source_rows)}")
            mutant_scanner = apply_mutant(src, work)
            full_wasm = work / "recursive_semantic_full.wasm"
            mutant_wasm = work / "recursive_semantic_skip.wasm"
            compile_wasm(
                src,
                generated,
                wasi,
                clang,
                src / "src" / "recursive_index.cpp",
                full_wasm,
            )
            compile_wasm(src, generated, wasi, clang, mutant_scanner, mutant_wasm)
            full_hash = sha256_file(full_wasm)
            mutant_hash = sha256_file(mutant_wasm)
            require_pin("full_artifact_sha256", full_hash, PINNED_FULL_ARTIFACT_SHA256)
            require_pin(
                "mutant_artifact_sha256", mutant_hash, PINNED_MUTANT_ARTIFACT_SHA256
            )
            print(
                f"artifact variant=full sha256={full_hash} bytes={full_wasm.stat().st_size}"
            )
            print(
                "artifact variant=semantic_skip "
                f"sha256={mutant_hash} bytes={mutant_wasm.stat().st_size}"
            )
            meters = {"full": Meter(full_wasm), "semantic_skip": Meter(mutant_wasm)}
            require_controls(meters["full"], meters["semantic_skip"], len(controls))
            measure_fuel(meters, len(valid))
            measure_heap(meters, len(valid))
            measure_wall(meters, len(valid))
        print("PASS recursive semantic comparator")
        return 0
    except (ComparatorError, subprocess.CalledProcessError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
