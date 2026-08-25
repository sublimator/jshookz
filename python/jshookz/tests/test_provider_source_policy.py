import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
CPP = ROOT / "cpp"
PROVIDER = CPP / "provider"
XAHAU_TYPES = CPP / "xahau-types"
SURFACE = (
    ROOT
    / "python"
    / "jshookz"
    / "src"
    / "jshookz"
    / "types"
    / "xahau-quickjs-v1.surface.json"
)
RAW_BYTE_APIS = ("JS_GetArrayBuffer(", "JS_GetTypedArrayBuffer(")
RAW_BYTE_PARSER_SOURCE = "xahau-types/quickjs.cpp"


def _cpp_sources() -> dict[str, str]:
    sources: dict[str, str] = {}
    for root in (PROVIDER, XAHAU_TYPES):
        for path in root.rglob("*.cpp"):
            if "tests" in path.parts or path.name.endswith("_probe.cpp"):
                continue
            sources[str(path.relative_to(CPP))] = path.read_text()
    return sources


def _raw_byte_api_uses(sources: dict[str, str]) -> list[str]:
    return sorted(
        name
        for name, source in sources.items()
        if name != RAW_BYTE_PARSER_SOURCE
        and any(api in source for api in RAW_BYTE_APIS)
    )


def test_raw_byte_api_fence_detects_a_binding_bypass():
    assert _raw_byte_api_uses(
        {
            RAW_BYTE_PARSER_SOURCE: "JS_GetArrayBuffer(ctx, &size, value);",
            "bad_binding.cpp": "JS_GetTypedArrayBuffer(ctx, value, 0, 0, 0);",
            "provider/bindings/quickjs.cpp": ("JS_GetArrayBuffer(ctx, &size, value);"),
        }
    ) == ["bad_binding.cpp", "provider/bindings/quickjs.cpp"]


def test_provider_byte_inputs_funnel_through_byte_view():
    assert _raw_byte_api_uses(_cpp_sources()) == []


def test_provider_build_depends_on_every_generated_xdata_header():
    cmake = (PROVIDER / "CMakeLists.txt").read_text()

    assert "../x-data/definitions.cmake" in cmake
    assert "add_dependencies(jshookz_provider generate_xdata_definitions)" in cmake


def _cpp_byte_policy_names(header: str) -> set[str]:
    body = re.search(
        r"enum class BytePolicy[^\{]*\{(?P<body>.*?)\};", header, re.DOTALL
    )
    assert body is not None
    return {
        match.group(1)
        for match in re.finditer(
            r"^\s*([A-Za-z][A-Za-z0-9]*)\s*,", body["body"], re.MULTILINE
        )
    }


def test_surface_byte_policies_are_cpp_policy_names():
    surface = json.loads(SURFACE.read_text())
    declared = {
        policy
        for parameters in surface["byte_policies"].values()
        for policy in parameters.values()
    }
    implemented = _cpp_byte_policy_names((XAHAU_TYPES / "quickjs.hpp").read_text())

    assert declared <= implemented


_BINDING_POLICY = re.compile(
    r"ByteView::getBinding\(\s*ctx\s*,\s*[^,]+,\s*"
    r'"(?P<binding>[^"]+)"\s*,\s*(?P<parameter>\d+)\s*,\s*'
    r"qjs::BytePolicy::(?P<policy>[A-Za-z][A-Za-z0-9]*)\s*\)",
    re.DOTALL,
)


def _cpp_binding_byte_policies(sources: dict[str, str]) -> dict[tuple[str, str], str]:
    policies: dict[tuple[str, str], str] = {}
    for source in sources.values():
        for match in _BINDING_POLICY.finditer(source):
            coordinate = (match["binding"], match["parameter"])
            assert coordinate not in policies, (
                f"duplicate binding byte policy: {coordinate}"
            )
            policies[coordinate] = match["policy"]
    return policies


def _surface_binding_byte_policies() -> dict[tuple[str, str], str]:
    surface = json.loads(SURFACE.read_text())
    return {
        (binding, parameter): policy
        for binding, parameters in surface["byte_policies"].items()
        for parameter, policy in parameters.items()
    }


def _binding_policy_join(sources: dict[str, str]) -> bool:
    return _cpp_binding_byte_policies(sources) == _surface_binding_byte_policies()


def test_binding_policy_join_detects_a_retargeted_accept():
    control = (PROVIDER / "bindings" / "control.cpp").read_text()
    mutated, replacements = re.subn(
        r'("accept"\s*,\s*0\s*,\s*qjs::BytePolicy::)lifecycleMessage',
        r"\1bytesLike",
        control,
        count=1,
    )
    assert replacements == 1

    live_sources = _cpp_sources()
    live_sources["provider/bindings/control.cpp"] = mutated

    assert not _binding_policy_join(live_sources)


def test_surface_byte_policies_match_cpp_binding_uses_exactly():
    assert _binding_policy_join(_cpp_sources())
