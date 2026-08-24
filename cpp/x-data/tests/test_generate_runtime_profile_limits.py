import importlib.util
import json
from pathlib import Path

import pytest


SCRIPT = Path(__file__).parents[1] / "scripts" / "generate_runtime_profile_limits.py"
REPOSITORY = Path(__file__).parents[3]
PROFILE = REPOSITORY / "xahau/profiles/xahau-quickjs-v1.source.json"
GENERATED = Path(__file__).parents[1] / "generated/runtime_profile_limits.h"
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


def source(**overrides: object) -> bytes:
    limits = {
        "serialized_object_max_bytes": 1_048_576,
        "serialized_object_max_fields": 32_768,
        "serialized_object_max_scopes": 32_769,
        "serialized_object_max_depth": 10,
        **overrides,
    }
    return json.dumps({"schema": GEN.SCHEMA, "limits": limits}, sort_keys=True).encode()


def test_header_projects_all_four_limits_and_source_identity():
    rendered = GEN.render(source(), "profile.source.json")
    assert "serialized_object_max_bytes = 1048576u;" in rendered
    assert "serialized_object_max_fields = 32768u;" in rendered
    assert "serialized_object_max_scopes = 32769u;" in rendered
    assert "serialized_object_max_depth = 10u;" in rendered
    assert "Source: profile.source.json" in rendered
    assert "SHA-256:" in rendered


def test_checked_in_header_matches_the_exact_runtime_profile_source():
    source_bytes = PROFILE.read_bytes()
    assert GENERATED.read_text() == GEN.render(source_bytes, PROFILE.name)


def test_every_runtime_limit_consumer_uses_the_generated_chain():
    for path, expected_tokens in CONSUMER_EXPECTATIONS.items():
        contents = path.read_text()
        for token in expected_tokens:
            assert token in contents, f"{path.relative_to(REPOSITORY)} lost {token}"


@pytest.mark.parametrize("value", [True, 0, -1, 1 << 32, "1048576"])
def test_header_rejects_non_positive_uint32_limits(value: object):
    with pytest.raises(ValueError, match="positive uint32"):
        GEN.render(source(serialized_object_max_bytes=value), "profile.source.json")


def test_mutating_one_projection_changes_that_generated_constant():
    rendered = GEN.render(
        source(serialized_object_max_fields=32_767), "profile.source.json"
    )
    assert "serialized_object_max_fields = 32767u;" in rendered
    assert "serialized_object_max_scopes = 32769u;" in rendered
