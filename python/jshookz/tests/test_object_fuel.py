"""Sealed-provider STObject/STArray fuel, scale, and poison gates."""

from __future__ import annotations

import hashlib
import importlib.metadata
import json
from dataclasses import dataclass
from functools import cache
from pathlib import Path

import pytest
from jshookz.host import WasmHost
from jshookz.paths import XAHAU_HOOK_PROVIDER_WASM, XAHAU_RUNTIME_PROFILE_LOCK
from jshookz.runtime_profile import (
    profile_execution_limits,
    verify_runtime_profile_lock,
)

PINNED_WASMTIME = "47.0.1"
STRUCTURE_SIZES = (8, 64)
ITERATIONS = (8, 16)
CLEAN_REPEATS = 2

ROUTES = (
    "mint",
    "primitive_miss",
    "primitive_hit",
    "rich_miss",
    "rich_hit",
    "nested_miss",
    "nested_hit",
    "object_lookup_miss",
    "object_lookup_hit",
    "object_lookup_missing",
    "array_length",
    "array_random",
    "array_traversal",
    "to_bytes",
    "to_json",
    "replacement",
    "no_op",
)

# First clean measurements on the pinned artifact were deterministic across
# both repeats. Ceilings retain roughly five percent headroom. Raising one or
# lowering a delta floor is a review-triggering rebaseline, not test repair.
CEILINGS = {
    ("mint", 8): 25_500.0,
    ("mint", 64): 93_000.0,
    ("primitive_miss", 8): 4_300.0,
    ("primitive_miss", 64): 4_300.0,
    ("primitive_hit", 8): 2_350.0,
    ("primitive_hit", 64): 2_350.0,
    ("rich_miss", 8): 4_500.0,
    ("rich_miss", 64): 4_600.0,
    ("rich_hit", 8): 2_800.0,
    ("rich_hit", 64): 2_800.0,
    ("nested_miss", 8): 10_150.0,
    ("nested_miss", 64): 10_150.0,
    ("nested_hit", 8): 3_500.0,
    ("nested_hit", 64): 3_500.0,
    ("object_lookup_miss", 8): 39_500.0,
    ("object_lookup_miss", 48): 234_000.0,
    ("object_lookup_hit", 8): 31_300.0,
    ("object_lookup_hit", 48): 188_000.0,
    ("object_lookup_missing", 8): 4_100.0,
    ("object_lookup_missing", 48): 4_200.0,
    ("array_length", 8): 1_850.0,
    ("array_length", 64): 1_850.0,
    ("array_random", 8): 4_000.0,
    ("array_random", 64): 4_000.0,
    ("array_traversal", 8): 55_000.0,
    ("array_traversal", 64): 378_000.0,
    ("to_bytes", 8): 30_000.0,
    ("to_bytes", 64): 150_000.0,
    ("to_json", 8): 164_000.0,
    ("to_json", 64): 970_000.0,
    ("replacement", 8): 56_700.0,
    ("replacement", 64): 219_000.0,
    ("no_op", 8): 3_850.0,
    ("no_op", 64): 3_850.0,
}

# First-clean total fuel at 8/16 iterations, rounded upward with roughly five
# percent headroom. These independently catch fixed-cost regressions that a
# slope-only comparison would subtract away.
ABSOLUTE_CEILINGS = {
    ("mint", 8): (282_000, 484_000),
    ("mint", 64): (814_000, 1_556_000),
    ("primitive_miss", 8): (115_000, 149_000),
    ("primitive_miss", 64): (114_000, 148_000),
    ("primitive_hit", 8): (101_000, 120_000),
    ("primitive_hit", 64): (101_000, 120_000),
    ("rich_miss", 8): (114_000, 150_000),
    ("rich_miss", 64): (114_000, 150_000),
    ("rich_hit", 8): (102_000, 124_000),
    ("rich_hit", 64): (102_000, 124_000),
    ("nested_miss", 8): (162_000, 243_000),
    ("nested_miss", 64): (163_000, 243_000),
    ("nested_hit", 8): (107_000, 135_000),
    ("nested_hit", 64): (107_000, 135_000),
    ("object_lookup_miss", 8): (397_000, 712_000),
    ("object_lookup_miss", 48): (2_465_000, 4_335_000),
    ("object_lookup_hit", 8): (331_000, 581_000),
    ("object_lookup_hit", 48): (1_582_000, 3_084_000),
    ("object_lookup_missing", 8): (114_000, 146_000),
    ("object_lookup_missing", 48): (114_000, 148_000),
    ("array_length", 8): (96_000, 110_000),
    ("array_length", 64): (96_000, 110_000),
    ("array_random", 8): (112_000, 144_000),
    ("array_random", 64): (112_000, 144_000),
    ("array_traversal", 8): (519_000, 957_000),
    ("array_traversal", 64): (3_105_000, 6_128_000),
    ("to_bytes", 8): (318_000, 556_000),
    ("to_bytes", 64): (1_274_000, 2_468_000),
    ("to_json", 8): (1_385_000, 2_692_000),
    ("to_json", 64): (7_833_000, 15_586_000),
    ("replacement", 8): (536_000, 990_000),
    ("replacement", 64): (1_827_000, 3_571_000),
    ("no_op", 8): (112_000, 142_000),
    ("no_op", 64): (112_000, 143_000),
}
SPREAD_CEILING = 8.0
DELTA_FLOORS = {
    "primitive_miss_minus_hit": 1_500.0,
    "rich_miss_minus_hit": 1_300.0,
    "nested_miss_minus_hit": 4_100.0,
    "object_miss_minus_hit_small": 7_000.0,
    "object_miss_minus_hit_large": 39_000.0,
    "object_miss_large_minus_small": 166_000.0,
    "object_hit_large_minus_small": 134_000.0,
    "mint_large_minus_small": 55_000.0,
    "traversal_large_minus_small": 260_000.0,
    "bytes_large_minus_small": 75_000.0,
    "json_large_minus_small": 650_000.0,
    "replacement_large_minus_small": 130_000.0,
}
CONSTANT_STRUCTURE_SPREAD = 20.0
MISSING_LOOKUP_STRUCTURE_SPREAD = 110.0
MAX_SERIALIZED_BYTES = 1_048_576
MAX_FIELDS = 32_768
MAX_SCOPES = 32_769
MAX_ARRAY_ELEMENTS = 32_767
MAX_ELEMENT_NOPS = 30
MAX_ROOT_NOPS = 29
MAX_WIRE_SETUP_CHUNK = 131_072
MAX_ELEMENT_SETUP_CHUNK = 8_192
HEADROOM_BYTES = 1_048_576
REPRESENTATIVE_OOM_HEAP_BYTES = 4 * 1024 * 1024
WASM_MALLOC_OVERHEAD = 16
MAXIMUM_REQUESTED_CORE = 4_194_336
MAXIMUM_ENGINE_REQUESTED = 413
MAXIMUM_DUPLICATE_STACK = 528
MAXIMUM_STATIC_PROTOCOL_BYTES = 31_481


COMMON_SETUP = r"""
function makeWire(elementCount) {
  const number = [
    0x91, 0x00, 0x04, 0x70, 0xDE, 0x4D, 0xF8,
    0x20, 0x00, 0xFF, 0xFF, 0xFF, 0xF1,
  ];
  const out = new Uint8Array(5 + 5 + number.length + 1 + 2 * elementCount + 1);
  let offset = 0;
  out.set([0x22, 0, 0, 0, 9], offset); offset += 5;
  out.set([0x24, 0, 0, 0, 7], offset); offset += 5;
  out.set(number, offset); offset += number.length;
  out[offset++] = 0xF9;
  for (let index = 0; index < elementCount; ++index) {
    out[offset++] = 0xEA;
    out[offset++] = 0xE1;
  }
  out[offset++] = 0xF1;
  if (offset !== out.length)
    throw new Error("fixture length mismatch");
  return out;
}
""".strip()


MAX_TOPOLOGY_SETUP = (
    r"""
function makeMaximumTopologyWire() {
  globalThis.maximumWire = new Uint8Array({MAX_SERIALIZED_BYTES});
  return maximumWire.length;
}

function fillMaximumNops(begin, end) {
  maximumWire.fill(0x99, begin, end);
  return end;
}

function writeMaximumElements(begin, end) {
  let offset = {MAX_ROOT_NOPS} + 2 + begin * ({MAX_ELEMENT_NOPS} + 2);
  for (let index = begin; index < end; ++index) {
    maximumWire[offset++] = 0xEA; // Memo object element.
    offset += {MAX_ELEMENT_NOPS};
    maximumWire[offset++] = 0xE1;
  }
  return offset;
}

function finishMaximumTopologyWire() {
  // NOPs consume wire but produce no field record.
  maximumWire[{MAX_ROOT_NOPS}] = 0xF0; // Amounts: extended field ordinal.
  maximumWire[{MAX_ROOT_NOPS} + 1] = 0x5C;
  const end = {MAX_ROOT_NOPS} + 2 +
    {MAX_ARRAY_ELEMENTS} * ({MAX_ELEMENT_NOPS} + 2);
  maximumWire[end] = 0xF1;
  if (end + 1 !== maximumWire.length)
    throw new Error(`maximum fixture length ${end + 1}`);
  return maximumWire.length;
}

"maximum topology helpers ready"
""".replace("{MAX_ARRAY_ELEMENTS}", str(MAX_ARRAY_ELEMENTS))
    .replace("{MAX_SERIALIZED_BYTES}", str(MAX_SERIALIZED_BYTES))
    .replace("{MAX_ROOT_NOPS}", str(MAX_ROOT_NOPS))
    .replace("{MAX_ELEMENT_NOPS}", str(MAX_ELEMENT_NOPS))
    .strip()
)


OBJECT_LOOKUP_SETUP = r"""
function makeUInt32ObjectWire(fields) {
  let byteCount = 0;
  for (const field of fields)
    byteCount += (field.fieldCode < 16 ? 1 : 2) + 4;
  const out = new Uint8Array(byteCount);
  let offset = 0;
  for (let index = 0; index < fields.length; ++index) {
    const field = fields[index];
    if (field.typeCode !== 2 || field.fieldCode > 255)
      throw new Error("object lookup fixture requires UInt32 fields");
    if (field.fieldCode < 16) {
      out[offset++] = 0x20 | field.fieldCode;
    } else {
      out[offset++] = 0x20;
      out[offset++] = field.fieldCode;
    }
    const value = index + 1;
    out[offset++] = value >>> 24;
    out[offset++] = value >>> 16;
    out[offset++] = value >>> 8;
    out[offset++] = value;
  }
  if (offset !== out.length)
    throw new Error("object lookup fixture length mismatch");
  return out;
}

const allUInt32Fields = Object.values(Field)
  .filter(field => field.typeCode === 2 && field.fieldCode <= 255)
  .sort((left, right) => left.code - right.code);
if (allUInt32Fields.length <= structureSize)
  throw new Error("insufficient UInt32 fields for lookup fixture");
globalThis.objectFields = allUInt32Fields.slice(0, structureSize);
globalThis.objectMissingField = allUInt32Fields[allUInt32Fields.length - 1];
globalThis.objectWire = makeUInt32ObjectWire(objectFields);
globalThis.objectRoot = util.decodeObject(objectWire);
globalThis.objectValues = objectFields.map(field => objectRoot.get(field));
""".strip()


RUN_BODIES = {
    "mint": """
      let value;
      for (let index = 0; index < count; ++index) {
        value = util.decodeObject(wire);
        accumulator += value !== undefined;
      }
    """,
    "primitive_miss": """
      for (let index = 0; index < count; ++index)
        accumulator += missRoots[index].Number.length;
    """,
    "primitive_hit": """
      for (let index = 0; index < count; ++index)
        accumulator += root.Number.length;
    """,
    "rich_miss": """
      for (let index = 0; index < count; ++index)
        accumulator += missRoots[index].Flags.toNumber();
    """,
    "rich_hit": """
      for (let index = 0; index < count; ++index)
        accumulator += root.Flags === richValue;
    """,
    "nested_miss": """
      for (let index = 0; index < count; ++index)
        accumulator += missRoots[index].Memos.at(0) !== undefined;
    """,
    "nested_hit": """
      for (let index = 0; index < count; ++index)
        accumulator += array.at(middle) === nestedValue;
    """,
    "object_lookup_miss": """
      for (let iteration = 0; iteration < count; ++iteration) {
        const selected = objectMissRoots[iteration];
        for (let fieldIndex = 0; fieldIndex < objectFields.length; ++fieldIndex)
          accumulator += selected.get(objectFields[fieldIndex]).toNumber();
      }
    """,
    "object_lookup_hit": """
      for (let iteration = 0; iteration < count; ++iteration)
        for (let fieldIndex = 0; fieldIndex < objectFields.length; ++fieldIndex)
          accumulator +=
            objectRoot.get(objectFields[fieldIndex]) === objectValues[fieldIndex];
    """,
    "object_lookup_missing": """
      for (let iteration = 0; iteration < count; ++iteration)
        accumulator += objectRoot.get(objectMissingField) === undefined;
    """,
    "array_length": """
      for (let index = 0; index < count; ++index)
        accumulator += array.length;
    """,
    "array_random": """
      for (let index = 0; index < count; ++index) {
        const selected = (index * 17) % structureSize;
        accumulator += array.at(selected) === arrayValues[selected];
      }
    """,
    "array_traversal": """
      for (let iteration = 0; iteration < count; ++iteration)
        for (const value of array)
          accumulator += value !== undefined;
    """,
    "to_bytes": """
      for (let index = 0; index < count; ++index)
        accumulator += root.toBytes().length;
    """,
    "to_json": """
      for (let index = 0; index < count; ++index)
        accumulator += JSON.stringify(root).length;
    """,
    "replacement": """
      for (let index = 0; index < count; ++index)
        accumulator += root.withField(Field.Flags, replacement).Flags.toNumber();
    """,
    "no_op": """
      for (let index = 0; index < count; ++index)
        accumulator += root.withoutField(Field.SourceTag) === root;
    """,
}


@dataclass(frozen=True)
class FuelSample:
    gas: int
    result: dict[str, int | str]


def _setup_source(route: str, size: int, max_iterations: int, poison: bool) -> str:
    if route not in ROUTES:
        raise ValueError(f"unknown object fuel route: {route}")
    needs_misses = route in {"primitive_miss", "rich_miss", "nested_miss"}
    needs_object_lookup = route.startswith("object_lookup_")
    needs_object_misses = route == "object_lookup_miss"
    if poison and needs_object_lookup:
        poison_statement = """
        for (let poisonIteration = 0; poisonIteration < count; ++poisonIteration)
          for (let poisonField = 0; poisonField < objectFields.length; ++poisonField)
            if (!util.validateObject(objectWire))
              throw new Error("object lookup rescan poison failed");
        """
    elif poison:
        poison_statement = """
        for (let poisonIndex = 0; poisonIndex < count; ++poisonIndex)
          if (!util.validateObject(wire))
            throw new Error("poison validation failed");
        """
    else:
        poison_statement = ""
    body = RUN_BODIES[route]
    misses = (
        "globalThis.missRoots = Array.from({length: maxIterations}, "
        "() => util.decodeObject(wire));"
        if needs_misses
        else "globalThis.missRoots = undefined;"
    )
    object_setup = OBJECT_LOOKUP_SETUP if needs_object_lookup else ""
    object_misses = (
        "globalThis.objectMissRoots = Array.from({length: maxIterations}, "
        "() => util.decodeObject(objectWire));"
        if needs_object_misses
        else "globalThis.objectMissRoots = undefined;"
    )
    return f"""
{COMMON_SETUP}
globalThis.structureSize = {size};
globalThis.maxIterations = {max_iterations};
globalThis.wire = makeWire(structureSize);
globalThis.root = util.decodeObject(wire);
globalThis.array = root.Memos;
globalThis.middle = Math.floor(structureSize / 2);
globalThis.richValue = root.Flags;
globalThis.nestedValue = array.at(middle);
globalThis.arrayValues = Array.from(
  {{length: structureSize}}, (_, index) => array.at(index));
globalThis.replacement = UInt32.from(11).okOr(null);
{misses}
{object_setup}
{object_misses}
globalThis.__runObjectFuel = function(count) {{
  let accumulator = 0;
  let routeCalls = 0;
  for (let outer = 0; outer < 1; ++outer) {{
    routeCalls += count;
    {poison_statement}
    {body}
  }}
  return {{route: {json.dumps(route)}, count, routeCalls, accumulator}};
}};
JSON.stringify({{ready: true, bytes: wire.length, elements: array.length}})
""".strip()


def _sample(
    route: str, size: int, iterations: int, *, poison: bool = False
) -> FuelSample:
    host = WasmHost.profiled()
    host.init()
    try:
        setup = host.eval(_setup_source(route, size, ITERATIONS[1], poison))
        assert setup.ok, setup.error
        assert json.loads(setup.result_value or "null") == {
            "ready": True,
            "bytes": 25 + 2 * size,
            "elements": size,
        }
        assert host.execution_limits is not None
        host.store.set_fuel(host.execution_limits.invocation_fuel)
        result = host.eval(f"JSON.stringify(__runObjectFuel({iterations}))")
        assert result.ok, result.error
        payload = json.loads(result.result_value or "null")
        assert payload["route"] == route
        assert payload["count"] == iterations
        assert payload["routeCalls"] == iterations
        if route == "object_lookup_miss":
            assert payload["accumulator"] == iterations * size * (size + 1) // 2
        elif route == "object_lookup_hit":
            assert payload["accumulator"] == iterations * size
        elif route == "object_lookup_missing":
            assert payload["accumulator"] == iterations
        return FuelSample(result.gas_used, payload)
    finally:
        host.destroy()


@cache
def _sample_bank(
    route: str, size: int, *, poison: bool = False
) -> tuple[tuple[FuelSample, FuelSample], ...]:
    low, high = ITERATIONS
    rows = []
    for _ in range(CLEAN_REPEATS):
        low_sample = _sample(route, size, low, poison=poison)
        high_sample = _sample(route, size, high, poison=poison)
        assert high_sample.gas > low_sample.gas
        rows.append((low_sample, high_sample))
    return tuple(rows)


@cache
def _slope(route: str, size: int, *, poison: bool = False) -> tuple[float, float]:
    low, high = ITERATIONS
    slopes = [
        (high_sample.gas - low_sample.gas) / (high - low)
        for low_sample, high_sample in _sample_bank(route, size, poison=poison)
    ]
    return sum(slopes) / len(slopes), max(slopes) - min(slopes)


def _current_pins() -> tuple[str, str]:
    provider_sha = hashlib.sha256(XAHAU_HOOK_PROVIDER_WASM.read_bytes()).hexdigest()
    lock = verify_runtime_profile_lock(
        XAHAU_RUNTIME_PROFILE_LOCK,
        XAHAU_HOOK_PROVIDER_WASM,
    )
    return provider_sha, lock.data["provider"]["sha256"]


def test_object_fuel_meter_uses_verified_profile_and_locked_engine_version():
    provider_sha, profile_provider_sha = _current_pins()
    assert provider_sha == profile_provider_sha
    assert importlib.metadata.version("wasmtime") == PINNED_WASMTIME


@pytest.mark.parametrize(("route", "size"), CEILINGS)
def test_object_route_fuel_ceiling_and_repeat_spread(route: str, size: int):
    absolute_ceilings = ABSOLUTE_CEILINGS[(route, size)]
    for samples in _sample_bank(route, size):
        for sample, ceiling in zip(samples, absolute_ceilings, strict=True):
            assert sample.gas <= ceiling
    slope, spread = _slope(route, size)
    assert slope <= CEILINGS[(route, size)]
    assert spread <= SPREAD_CEILING


@pytest.mark.parametrize("size", STRUCTURE_SIZES)
def test_cache_miss_lanes_remain_more_expensive_than_hits(size: int):
    primitive_miss, _ = _slope("primitive_miss", size)
    primitive_hit, _ = _slope("primitive_hit", size)
    rich_miss, _ = _slope("rich_miss", size)
    rich_hit, _ = _slope("rich_hit", size)
    nested_miss, _ = _slope("nested_miss", size)
    nested_hit, _ = _slope("nested_hit", size)

    assert primitive_miss - primitive_hit >= DELTA_FLOORS["primitive_miss_minus_hit"]
    assert rich_miss - rich_hit >= DELTA_FLOORS["rich_miss_minus_hit"]
    assert nested_miss - nested_hit >= DELTA_FLOORS["nested_miss_minus_hit"]


def test_object_lookup_sweeps_scale_linearly_and_misses_remain_visible():
    miss_small, _ = _slope("object_lookup_miss", 8)
    miss_large, _ = _slope("object_lookup_miss", 48)
    hit_small, _ = _slope("object_lookup_hit", 8)
    hit_large, _ = _slope("object_lookup_hit", 48)
    missing_small, _ = _slope("object_lookup_missing", 8)
    missing_large, _ = _slope("object_lookup_missing", 48)

    assert miss_small - hit_small >= DELTA_FLOORS["object_miss_minus_hit_small"]
    assert miss_large - hit_large >= DELTA_FLOORS["object_miss_minus_hit_large"]
    assert miss_large - miss_small >= DELTA_FLOORS["object_miss_large_minus_small"]
    assert hit_large - hit_small >= DELTA_FLOORS["object_hit_large_minus_small"]
    assert abs(missing_large - missing_small) <= MISSING_LOOKUP_STRUCTURE_SPREAD


def test_linear_routes_scale_and_direct_array_routes_do_not():
    mint_small, _ = _slope("mint", 8)
    mint_large, _ = _slope("mint", 64)
    traversal_small, _ = _slope("array_traversal", 8)
    traversal_large, _ = _slope("array_traversal", 64)
    bytes_small, _ = _slope("to_bytes", 8)
    bytes_large, _ = _slope("to_bytes", 64)
    json_small, _ = _slope("to_json", 8)
    json_large, _ = _slope("to_json", 64)
    replacement_small, _ = _slope("replacement", 8)
    replacement_large, _ = _slope("replacement", 64)

    assert mint_large - mint_small >= DELTA_FLOORS["mint_large_minus_small"]
    assert (
        traversal_large - traversal_small >= DELTA_FLOORS["traversal_large_minus_small"]
    )
    assert bytes_large - bytes_small >= DELTA_FLOORS["bytes_large_minus_small"]
    assert json_large - json_small >= DELTA_FLOORS["json_large_minus_small"]
    assert (
        replacement_large - replacement_small
        >= DELTA_FLOORS["replacement_large_minus_small"]
    )

    for route in ("array_length", "array_random", "primitive_hit", "rich_hit"):
        small, _ = _slope(route, 8)
        large, _ = _slope(route, 64)
        assert abs(large - small) <= CONSTANT_STRUCTURE_SPREAD


def test_recertification_poison_turns_the_cached_length_ceiling_red():
    poisoned, spread = _slope("array_length", 64, poison=True)
    assert poisoned > CEILINGS[("array_length", 64)]
    assert spread <= SPREAD_CEILING


def test_object_rescan_poison_turns_the_large_hit_ceiling_red():
    poisoned, spread = _slope("object_lookup_hit", 48, poison=True)
    assert poisoned > CEILINGS[("object_lookup_hit", 48)]
    assert spread <= SPREAD_CEILING


def _maximum_setup_step(host: WasmHost, source: str):
    assert host.execution_limits is not None
    host.store.set_fuel(host.execution_limits.invocation_fuel)
    result = host.eval(source)
    assert result.ok, result.error
    return result


def _prepare_maximum_topology_wire(host: WasmHost) -> None:
    _maximum_setup_step(host, MAX_TOPOLOGY_SETUP)
    _maximum_setup_step(host, "makeMaximumTopologyWire()")
    for begin in range(0, MAX_SERIALIZED_BYTES, MAX_WIRE_SETUP_CHUNK):
        end = min(begin + MAX_WIRE_SETUP_CHUNK, MAX_SERIALIZED_BYTES)
        _maximum_setup_step(host, f"fillMaximumNops({begin}, {end})")
    for begin in range(0, MAX_ARRAY_ELEMENTS, MAX_ELEMENT_SETUP_CHUNK):
        end = min(begin + MAX_ELEMENT_SETUP_CHUNK, MAX_ARRAY_ELEMENTS)
        _maximum_setup_step(host, f"writeMaximumElements({begin}, {end})")
    _maximum_setup_step(host, "finishMaximumTopologyWire()")


def _mint_maximum_topology(host: WasmHost) -> None:
    _maximum_setup_step(
        host,
        "globalThis.maximumRoot = util.decodeObject(maximumWire); "
        "globalThis.maximumArray = maximumRoot.Amounts; maximumArray.length",
    )
    setup = _maximum_setup_step(
        host,
        "JSON.stringify({"
        "bytes: maximumWire.length,"
        "valid: util.validateObject(maximumWire),"
        "elements: maximumArray.length,"
        "fields: maximumArray.length + 1,"
        "scopes: maximumArray.length + 2})",
    )
    assert json.loads(setup.result_value or "null") == {
        "bytes": MAX_SERIALIZED_BYTES,
        "valid": True,
        "elements": MAX_ARRAY_ELEMENTS,
        "fields": MAX_FIELDS,
        "scopes": MAX_SCOPES,
    }


def _prepared_maximum_topology_host() -> WasmHost:
    host = WasmHost.profiled()
    host.init()
    try:
        _prepare_maximum_topology_wire(host)
        _mint_maximum_topology(host)
        return host
    except BaseException:
        host.destroy()
        raise


def test_maximum_topology_fits_heap_and_post_success_headroom():
    host = _prepared_maximum_topology_host()
    try:
        assert host.execution_limits is not None
        assert host.execution_limits.quickjs_heap_bytes == 16 * 1024 * 1024
        assert host.execution_limits.invocation_fuel == 50_000_000
        host.store.set_fuel(host.execution_limits.invocation_fuel)
        headroom_result = host.eval(
            f"""
            JSON.stringify((() => {{
              const headroom = new Uint8Array({HEADROOM_BYTES});
              headroom[0] = 0xA5;
              headroom[headroom.length - 1] = 0x5A;
              return {{
                headroom: headroom.length,
                sentinels: headroom[0] + headroom[headroom.length - 1],
              }};
            }})())
            """
        )
        assert headroom_result.ok, headroom_result.error
        assert headroom_result.gas_used < host.execution_limits.invocation_fuel
        assert json.loads(headroom_result.result_value or "null") == {
            "headroom": HEADROOM_BYTES,
            "sentinels": 0xA5 + 0x5A,
        }
    finally:
        host.destroy()


def test_maximum_topology_records_exact_requested_and_charged_heap(
    resource_probe_wasm: Path,
):
    lock = verify_runtime_profile_lock(
        XAHAU_RUNTIME_PROFILE_LOCK,
        XAHAU_HOOK_PROVIDER_WASM,
    )
    limits = profile_execution_limits(lock)
    host = WasmHost(
        wasm_path=resource_probe_wasm,
        execution_limits=limits,
    )
    exports = host.instance.exports(host.store)

    def resource(name: str) -> int:
        return exports[name](host.store)

    try:
        restored_snapshot = (
            resource("qjs_resource_current_size"),
            resource("qjs_resource_current_count"),
        )
        host.init()
        registration = (
            resource("qjs_resource_current_size"),
            resource("qjs_resource_current_count"),
        )
        _prepare_maximum_topology_wire(host)
        pre_call = (
            resource("qjs_resource_current_size"),
            resource("qjs_resource_current_count"),
        )
        exports["qjs_resource_reset_peak"](host.store)
        _mint_maximum_topology(host)
        peak = (
            resource("qjs_resource_peak_size"),
            resource("qjs_resource_peak_count"),
        )
        post_success = (
            resource("qjs_resource_current_size"),
            resource("qjs_resource_current_count"),
        )
        static_bytes = resource("qjs_resource_static_protocol_bytes")

        host.store.set_fuel(limits.invocation_fuel)
        headroom = host.eval(
            f"const measuredHeadroom = new Uint8Array({HEADROOM_BYTES});"
            "measuredHeadroom[0] = 0xA5;"
            "measuredHeadroom[measuredHeadroom.length - 1] = 0x5A;"
            "measuredHeadroom[0] + measuredHeadroom[measuredHeadroom.length - 1]"
        )
        assert headroom.ok, headroom.error
        assert headroom.result_value == str(0xA5 + 0x5A)
    finally:
        host.destroy()

    assert {
        "registration": registration,
        "pre_call": pre_call,
        "peak": peak,
        "post_success": post_success,
        "static_protocol": static_bytes,
    } == {
        # Wizer restores the accepted runtime namespaces plus five frozen
        # serialized-object constructors/prototypes.  The class hierarchy adds
        # 3,179 requested persistent bytes and 58 persistent allocations; the
        # generated complete-format tables add 7,757 static protocol bytes.
        # Publishing accept.require adds 136 charged bytes and 3 allocations.
        # The frozen foreign-state accessor class/prototype adds 1,090 charged
        # persistent bytes and 21 allocations; no accessor instance is live in
        # this topology. The frozen LedgerKeylet class/prototype, classifier
        # noun, and nested util.keylet namespace add 1,364 charged bytes and
        # 25 allocations; no keylet instance is live in this topology.
        # Publishing hook.again adds 150 charged bytes and 3 allocations.
        # The frozen emit.build namespace, its two functions, and the nominal
        # EmittedTransaction class/prototype add 857 charged persistent bytes
        # and 16 allocations; no emitted transaction instance is live here.
        "registration": (173_804, 2_620),
        "pre_call": (1_226_144, 2_670),
        "peak": (5_421_101, 2_683),
        "post_success": (3_455_805, 2_697),
        "static_protocol": MAXIMUM_STATIC_PROTOCOL_BYTES,
    }
    assert restored_snapshot == registration

    registration_requested = registration[0] - registration[1] * WASM_MALLOC_OVERHEAD
    pre_call_requested = pre_call[0] - pre_call[1] * WASM_MALLOC_OVERHEAD
    peak_requested = peak[0] - peak[1] * WASM_MALLOC_OVERHEAD
    peak_requested_delta = peak_requested - pre_call_requested
    peak_count_delta = peak[1] - pre_call[1]

    assert pre_call_requested - registration_requested == 1_051_540
    assert peak_requested_delta == MAXIMUM_REQUESTED_CORE + MAXIMUM_ENGINE_REQUESTED
    assert peak[0] - pre_call[0] == (
        MAXIMUM_REQUESTED_CORE
        + MAXIMUM_ENGINE_REQUESTED
        + peak_count_delta * WASM_MALLOC_OVERHEAD
    )
    assert MAXIMUM_DUPLICATE_STACK == 11 * 6 * 8
    assert peak[0] < limits.quickjs_heap_bytes
    assert limits.quickjs_heap_bytes - post_success[0] == 13_321_411
    assert limits.quickjs_heap_bytes - post_success[0] >= HEADROOM_BYTES


def test_maximum_density_reflection_fits_the_profile_deterministically():
    samples = []
    for _ in range(2):
        host = _prepared_maximum_topology_host()
        try:
            assert host.execution_limits is not None
            host.store.set_fuel(host.execution_limits.invocation_fuel)
            reflected = host.eval("Reflect.ownKeys(maximumArray).length")
            samples.append(
                (
                    reflected.ok,
                    reflected.error,
                    reflected.result_value,
                    reflected.gas_used,
                )
            )
        finally:
            host.destroy()

    assert samples[0] == samples[1]
    assert samples[0][:3] == (True, None, str(MAX_ARRAY_ELEMENTS + 1))
    assert 0 < samples[0][3] <= 44_000_000


def test_representative_heap_oom_has_no_publication_and_same_runtime_retry():
    host = _prepared_maximum_topology_host()
    try:
        assert host.execution_limits is not None
        full_limit = host.execution_limits.quickjs_heap_bytes
        host.store.set_fuel(host.execution_limits.invocation_fuel)
        sentinel = host.eval("globalThis.oomTarget = 'sentinel'; 0")
        assert sentinel.ok, sentinel.error

        host.set_memory_limit(REPRESENTATIVE_OOM_HEAP_BYTES)
        host.store.set_fuel(host.execution_limits.invocation_fuel)
        tiny = host.eval("0")
        assert tiny.ok, tiny.error
        host.store.set_fuel(host.execution_limits.invocation_fuel)
        failed = host.eval(
            f"globalThis.oomTarget = new Uint8Array({HEADROOM_BYTES}); oomTarget.length"
        )
        assert not failed.ok
        assert failed.error == "InternalError: out of memory"
        assert failed.result_value == "InternalError: out of memory"
        assert 0 < failed.gas_used < host.execution_limits.invocation_fuel

        host.set_memory_limit(full_limit)
        host.store.set_fuel(host.execution_limits.invocation_fuel)
        retry = host.eval(
            f"""
            JSON.stringify((() => {{
              const unpublished = oomTarget === "sentinel";
              oomTarget = new Uint8Array({HEADROOM_BYTES});
              oomTarget[0] = 0xA5;
              oomTarget[oomTarget.length - 1] = 0x5A;
              return {{
                unpublished,
                retryLength: oomTarget.length,
                sentinels: oomTarget[0] + oomTarget[oomTarget.length - 1],
                sourceIdentity: maximumRoot.Amounts === maximumArray,
                sourceLength: maximumArray.length,
              }};
            }})())
            """
        )
        assert retry.ok, retry.error
        assert json.loads(retry.result_value or "null") == {
            "unpublished": True,
            "retryLength": HEADROOM_BYTES,
            "sentinels": 0xA5 + 0x5A,
            "sourceIdentity": True,
            "sourceLength": MAX_ARRAY_ELEMENTS,
        }
    finally:
        host.set_memory_limit(16 * 1024 * 1024)
        host.destroy()
