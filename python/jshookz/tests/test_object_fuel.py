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
from jshookz.paths import (
    XAHAU_HOOK_PROVIDER_WASM,
    XAHAU_RUNTIME_PROFILE_LOCK,
)


PINNED_PROVIDER_SHA256 = (
    "2e8272916a28abf76075704876755966a4e6dac4376de9e41a13615f389dca8b"
)
PINNED_RUNTIME_PROFILE_ID = (
    "00a4c2fca32b27e3ec4f26047a895228d360e288bc1b1c08724e78e2310f47c8"
)
PINNED_WASMTIME = "47.0.1"
PINNED_WASMTIME_DYLIB_SHA256 = (
    "272ec3fa3b344e3ed5385140581aa880b0564b67d8c96131d7002218b6d3721d"
)
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
    ("mint", 8): 23_500.0,
    ("mint", 64): 93_000.0,
    ("primitive_miss", 8): 4_300.0,
    ("primitive_miss", 64): 4_300.0,
    ("primitive_hit", 8): 2_350.0,
    ("primitive_hit", 64): 2_350.0,
    ("rich_miss", 8): 4_500.0,
    ("rich_miss", 64): 4_600.0,
    ("rich_hit", 8): 2_800.0,
    ("rich_hit", 64): 2_800.0,
    ("nested_miss", 8): 8_650.0,
    ("nested_miss", 64): 8_650.0,
    ("nested_hit", 8): 3_500.0,
    ("nested_hit", 64): 3_500.0,
    ("object_lookup_miss", 8): 39_500.0,
    ("object_lookup_miss", 48): 234_000.0,
    ("object_lookup_hit", 8): 31_300.0,
    ("object_lookup_hit", 48): 188_000.0,
    ("object_lookup_missing", 8): 2_950.0,
    ("object_lookup_missing", 48): 3_050.0,
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
    ("replacement", 8): 52_500.0,
    ("replacement", 64): 219_000.0,
    ("no_op", 8): 3_850.0,
    ("no_op", 64): 3_850.0,
}

# First-clean total fuel at 8/16 iterations, rounded upward with roughly five
# percent headroom. These independently catch fixed-cost regressions that a
# slope-only comparison would subtract away.
ABSOLUTE_CEILINGS = {
    ("mint", 8): (265_000, 453_000),
    ("mint", 64): (814_000, 1_556_000),
    ("primitive_miss", 8): (115_000, 149_000),
    ("primitive_miss", 64): (114_000, 148_000),
    ("primitive_hit", 8): (101_000, 120_000),
    ("primitive_hit", 64): (101_000, 120_000),
    ("rich_miss", 8): (114_000, 150_000),
    ("rich_miss", 64): (114_000, 150_000),
    ("rich_hit", 8): (102_000, 124_000),
    ("rich_hit", 64): (102_000, 124_000),
    ("nested_miss", 8): (150_000, 219_000),
    ("nested_miss", 64): (150_000, 219_000),
    ("nested_hit", 8): (107_000, 135_000),
    ("nested_hit", 64): (107_000, 135_000),
    ("object_lookup_miss", 8): (397_000, 712_000),
    ("object_lookup_miss", 48): (2_465_000, 4_335_000),
    ("object_lookup_hit", 8): (331_000, 581_000),
    ("object_lookup_hit", 48): (1_582_000, 3_084_000),
    ("object_lookup_missing", 8): (105_000, 128_000),
    ("object_lookup_missing", 48): (105_000, 130_000),
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
    ("replacement", 8): (500_000, 918_000),
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
    "bytes_large_minus_small": 95_000.0,
    "json_large_minus_small": 650_000.0,
    "replacement_large_minus_small": 130_000.0,
}
CONSTANT_STRUCTURE_SPREAD = 20.0
MISSING_LOOKUP_STRUCTURE_SPREAD = 100.0
MAX_SERIALIZED_BYTES = 1_048_576
MAX_FIELDS = 32_768
MAX_SCOPES = 32_769
MAX_ARRAY_ELEMENTS_WITH_TWO_BLOBS = 32_765
HEADROOM_BYTES = 1_048_576
REPRESENTATIVE_OOM_HEAP_BYTES = 4 * 1024 * 1024


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


MAX_TOPOLOGY_SETUP = r"""
function writeVL(out, offset, length) {
  if (length <= 192) {
    out[offset++] = length;
  } else if (length <= 12480) {
    const adjusted = length - 193;
    out[offset++] = 193 + (adjusted >>> 8);
    out[offset++] = adjusted & 255;
  } else if (length <= 918744) {
    const adjusted = length - 12481;
    out[offset++] = 241 + (adjusted >>> 16);
    out[offset++] = (adjusted >>> 8) & 255;
    out[offset++] = adjusted & 255;
  } else {
    throw new RangeError("VL payload is too large");
  }
  return offset;
}

function makeMaximumTopologyWire() {
  const elementCount = 32765;
  const firstBlob = 918744;
  const secondBlob = 64292;
  const out = new Uint8Array(1048576);
  let offset = 0;
  out[offset++] = 0x73; // SigningPubKey
  offset = writeVL(out, offset, firstBlob);
  offset += firstBlob;
  out[offset++] = 0x7D; // MemoData
  offset = writeVL(out, offset, secondBlob);
  offset += secondBlob;
  out[offset++] = 0xF9; // Memos
  for (let index = 0; index < elementCount; ++index) {
    out[offset++] = 0xEA; // Memo
    out[offset++] = 0xE1;
  }
  out[offset++] = 0xF1;
  if (offset !== out.length)
    throw new Error(`maximum fixture length ${offset}`);
  return out;
}

"maximum topology helpers ready"
""".strip()


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


def _wasmtime_engine_hash() -> str:
    import wasmtime

    package = Path(wasmtime.__file__).parent
    libraries = tuple(package.rglob("_libwasmtime.dylib")) + tuple(
        package.rglob("_libwasmtime.so")
    )
    assert len(libraries) == 1
    return hashlib.sha256(libraries[0].read_bytes()).hexdigest()


def _current_pins() -> tuple[str, str, str, str]:
    provider_sha = hashlib.sha256(XAHAU_HOOK_PROVIDER_WASM.read_bytes()).hexdigest()
    profile_id = json.loads(XAHAU_RUNTIME_PROFILE_LOCK.read_text())[
        "runtime_profile_id"
    ]
    return (
        provider_sha,
        profile_id,
        importlib.metadata.version("wasmtime"),
        _wasmtime_engine_hash(),
    )


def test_object_fuel_meter_is_exactly_pinned():
    assert _current_pins() == (
        PINNED_PROVIDER_SHA256,
        PINNED_RUNTIME_PROFILE_ID,
        PINNED_WASMTIME,
        PINNED_WASMTIME_DYLIB_SHA256,
    )


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


def _prepared_maximum_topology_host() -> WasmHost:
    host = WasmHost.profiled()
    host.init()
    try:
        assert host.execution_limits is not None

        def setup_step(source: str):
            host.store.set_fuel(host.execution_limits.invocation_fuel)
            result = host.eval(source)
            assert result.ok, result.error
            return result

        setup_step(MAX_TOPOLOGY_SETUP)
        setup_step(
            "globalThis.maximumWire = makeMaximumTopologyWire(); maximumWire.length"
        )
        setup_step(
            "globalThis.maximumRoot = util.decodeObject(maximumWire); "
            "globalThis.maximumArray = maximumRoot.Memos; maximumArray.length"
        )
        setup = setup_step(
            "JSON.stringify({"
            "bytes: maximumWire.length,"
            "valid: util.validateObject(maximumWire),"
            "elements: maximumArray.length,"
            "fields: maximumArray.length + 3,"
            "scopes: maximumArray.length + 2})"
        )
        assert json.loads(setup.result_value or "null") == {
            "bytes": MAX_SERIALIZED_BYTES,
            "valid": True,
            "elements": MAX_ARRAY_ELEMENTS_WITH_TWO_BLOBS,
            "fields": MAX_FIELDS,
            "scopes": MAX_ARRAY_ELEMENTS_WITH_TWO_BLOBS + 2,
        }
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
    assert samples[0][:3] == (True, None, "32766")
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
                sourceIdentity: maximumRoot.Memos === maximumArray,
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
            "sourceLength": MAX_ARRAY_ELEMENTS_WITH_TWO_BLOBS,
        }
    finally:
        host.set_memory_limit(16 * 1024 * 1024)
        host.destroy()
