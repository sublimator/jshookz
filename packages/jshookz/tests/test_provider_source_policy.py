import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
PROVIDER = ROOT / "runtime" / "provider"
SURFACE = (
    ROOT
    / "packages"
    / "jshookz"
    / "src"
    / "jshookz"
    / "types"
    / "xahau-quickjs-v1.surface.json"
)
RAW_BYTE_APIS = ("JS_GetArrayBuffer(", "JS_GetTypedArrayBuffer(")


def _raw_byte_api_uses(sources: dict[str, str]) -> list[str]:
    return sorted(
        name
        for name, source in sources.items()
        if name != "quickjs.cpp" and any(api in source for api in RAW_BYTE_APIS)
    )


def test_raw_byte_api_fence_detects_a_binding_bypass():
    assert _raw_byte_api_uses(
        {
            "quickjs.cpp": "JS_GetArrayBuffer(ctx, &size, value);",
            "bad_binding.cpp": "JS_GetTypedArrayBuffer(ctx, value, 0, 0, 0);",
        }
    ) == ["bad_binding.cpp"]


def test_provider_byte_inputs_funnel_through_byte_view():
    sources = {
        str(path.relative_to(PROVIDER)): path.read_text()
        for path in PROVIDER.rglob("*.cpp")
    }

    assert _raw_byte_api_uses(sources) == []


def _cpp_byte_policy_names(header: str) -> set[str]:
    body = re.search(
        r"enum class BytePolicy[^\{]*\{(?P<body>.*?)\};", header, re.DOTALL
    )
    assert body is not None
    return {
        match.group(1)
        for match in re.finditer(r"^\s*([A-Za-z][A-Za-z0-9]*)\s*,", body["body"], re.MULTILINE)
    }


def test_surface_byte_policies_are_cpp_policy_names():
    surface = json.loads(SURFACE.read_text())
    declared = {
        policy
        for parameters in surface["byte_policies"].values()
        for policy in parameters.values()
    }
    implemented = _cpp_byte_policy_names((PROVIDER / "quickjs.hpp").read_text())

    assert declared <= implemented
