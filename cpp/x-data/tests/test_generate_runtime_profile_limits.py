import importlib.util
import json
from pathlib import Path

import pytest


SCRIPT = Path(__file__).parents[1] / "scripts" / "generate_runtime_profile_limits.py"
REPOSITORY = Path(__file__).parents[3]
PROFILE = REPOSITORY / "xahau/profiles/xahau-quickjs-v1.source.json"
GENERATED = Path(__file__).parents[1] / "generated/runtime_profile_limits.h"
GENERATED_PYTHON = (
    REPOSITORY / "python/jshookz/src/jshookz/_runtime_profile_constants.py"
)
CONSUMER_EXPECTATIONS = {
    REPOSITORY / "cpp/x-data/includes/catl/xdata/recursive_index.h": (
        "serialized_object_max_bytes",
        "serialized_object_max_fields",
        "serialized_object_max_scopes",
        "serialized_object_max_depth",
    ),
    REPOSITORY / "cpp/xahau-types/leaf/leaf_js.cpp": ("serialized_object_max_fields",),
    REPOSITORY / "cpp/xahau-types/object/canonical_json.hpp": (
        "serialized_object_max_bytes",
    ),
    REPOSITORY / "cpp/xahau-types/object/object_js.cpp": (
        "RecursiveScanLimits{}.max_bytes",
        "RecursiveScanLimits{}.max_fields",
        "RecursiveScanLimits{}.max_scopes",
        "RecursiveScanLimits{}.max_depth",
    ),
    REPOSITORY / "cpp/xahau-types/pathset/pathset_js.cpp": (
        "serialized_object_max_bytes",
    ),
}
SPEC = importlib.util.spec_from_file_location("generate_runtime_profile_limits", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
GEN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GEN)


def source(
    *,
    limits_overrides: dict[str, object] | None = None,
    profile_overrides: dict[str, object] | None = None,
    validation_overrides: dict[str, object] | None = None,
) -> bytes:
    limits = {
        "serialized_object_max_bytes": 1_048_576,
        "serialized_object_max_fields": 32_768,
        "serialized_object_max_scopes": 32_769,
        "serialized_object_max_depth": 10,
        **(limits_overrides or {}),
    }
    profiles = {
        "none": 0,
        "xahauFloatV1": 1,
        "nearestEvenV1": 2,
        **(profile_overrides or {}),
    }
    validation = {
        "layout_version": 1,
        "failure_sentinel": -1,
        "main_bit": 1,
        "callback_bit": 2,
        "entry_mask": 3,
        "reserved_mask": 0x800000FC,
        "profile_mask": 0x00FFFF00,
        "profile_shift": 8,
        "version_mask": 0x7F000000,
        "version_shift": 24,
        **(validation_overrides or {}),
    }
    return json.dumps(
        {
            "schema": GEN.SCHEMA,
            "artifact": {
                "envelope_version": 2,
                "xfl_arithmetic_profile_codes": profiles,
            },
            "provider": {"module_validation_result": validation},
            "limits": limits,
        },
        sort_keys=True,
    ).encode()


def test_header_projects_all_four_limits_and_source_identity():
    rendered = GEN.render(source(), "profile.source.json")
    assert "serialized_object_max_bytes = 1048576u;" in rendered
    assert "serialized_object_max_fields = 32768u;" in rendered
    assert "serialized_object_max_scopes = 32769u;" in rendered
    assert "serialized_object_max_depth = 10u;" in rendered
    assert "Source: profile.source.json" in rendered
    assert "SHA-256:" in rendered


def test_header_projects_profile_codes_and_validation_layout():
    rendered = GEN.render(source(), "profile.source.json")
    assert "xqjs_envelope_version = 2u;" in rendered
    assert "xfl_arithmetic_profile_none = 0u;" in rendered
    assert "xfl_arithmetic_profile_xahau_float_v1 = 1u;" in rendered
    assert "xfl_arithmetic_profile_nearest_even_v1 = 2u;" in rendered
    assert "module_validation_failure_sentinel = -1;" in rendered
    assert "module_validation_entry_mask = 3u;" in rendered
    assert "module_validation_reserved_mask = 2147483900u;" in rendered
    assert "module_validation_profile_mask = 16776960u;" in rendered
    assert "module_validation_version_mask = 2130706432u;" in rendered


def test_checked_in_header_matches_the_exact_runtime_profile_source():
    source_bytes = PROFILE.read_bytes()
    assert GENERATED.read_text() == GEN.render(source_bytes, PROFILE.name)
    assert GENERATED_PYTHON.read_text() == GEN.render_python(
        source_bytes, PROFILE.name
    )


def test_python_projection_carries_the_same_codes_and_layout():
    rendered = GEN.render_python(source(), "profile.source.json")
    assert "XQJS_ENVELOPE_VERSION = 2" in rendered
    assert "XFL_ARITHMETIC_PROFILE_NONE = 0" in rendered
    assert "XFL_ARITHMETIC_PROFILE_XAHAU_FLOAT_V1 = 1" in rendered
    assert "XFL_ARITHMETIC_PROFILE_NEAREST_EVEN_V1 = 2" in rendered
    assert "MODULE_VALIDATION_FAILURE_SENTINEL = -1" in rendered
    assert "MODULE_VALIDATION_LAYOUT_VERSION = 1" in rendered
    assert "MODULE_VALIDATION_RESERVED_MASK = 2147483900" in rendered


def test_every_runtime_limit_consumer_uses_the_generated_chain():
    for path, expected_tokens in CONSUMER_EXPECTATIONS.items():
        contents = path.read_text()
        for token in expected_tokens:
            assert token in contents, f"{path.relative_to(REPOSITORY)} lost {token}"


@pytest.mark.parametrize("value", [True, 0, -1, 1 << 32, "1048576"])
def test_header_rejects_non_positive_uint32_limits(value: object):
    with pytest.raises(ValueError, match="positive uint32"):
        GEN.render(
            source(limits_overrides={"serialized_object_max_bytes": value}),
            "profile.source.json",
        )


@pytest.mark.parametrize(
    ("field", "value"),
    [
        ("layout_version", 2),
        ("failure_sentinel", -2),
        ("main_bit", 2),
        ("callback_bit", 4),
        ("entry_mask", 1),
        ("reserved_mask", 0),
        ("profile_mask", 0),
        ("profile_shift", 9),
        ("version_mask", 0),
        ("version_shift", 23),
    ],
)
def test_header_rejects_independent_validation_layout_drift(field: str, value: int):
    with pytest.raises(ValueError, match="module-validation"):
        GEN.render(
            source(validation_overrides={field: value}), "profile.source.json"
        )


@pytest.mark.parametrize(
    ("field", "value"),
    [("none", 1), ("xahauFloatV1", 0), ("nearestEvenV1", 3)],
)
def test_header_rejects_independent_profile_code_drift(field: str, value: int):
    with pytest.raises(ValueError, match="must be 0/1/2"):
        GEN.render(source(profile_overrides={field: value}), "profile.source.json")


def test_header_rejects_current_envelope_version_drift():
    document = json.loads(source())
    document["artifact"]["envelope_version"] = 1
    with pytest.raises(ValueError, match="envelope version must be exactly 2"):
        GEN.render(json.dumps(document).encode(), "profile.source.json")


def test_mutating_one_projection_changes_that_generated_constant():
    rendered = GEN.render(
        source(limits_overrides={"serialized_object_max_fields": 32_767}),
        "profile.source.json",
    )
    assert "serialized_object_max_fields = 32767u;" in rendered
    assert "serialized_object_max_scopes = 32769u;" in rendered
