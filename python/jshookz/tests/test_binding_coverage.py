"""Join // @binding provider: tags vs the projected v1 surface.

cpp/xahau-types must not include hook_imports.hpp or call hook_* / host_*.
Host crossings live in cpp/provider/bindings/. Result is a JS class, not a
host call. Tags stay on the provider plane: they are the product JS surface.
"""

from __future__ import annotations

import json
import re
from pathlib import Path

from jshookz.paths import REPO_ROOT, XAHAU_V1_JAVASCRIPT_SURFACE

_TAG = re.compile(
    r"^\s*//\s*@binding\s+(?P<plane>provider):(?P<path>[A-Za-z][\w.]*)\s*$"
)
_HOST_CALL = re.compile(r"\b(?:hook_|host_)[A-Za-z]\w*\s*\(")
_HOOK_IMPORT = re.compile(r'^\s*#\s*include\s*[<"][^>"]*hook_imports\.hpp[>"]')
_UINT_WIDTHS = ("8", "16", "32", "64")
PROVIDER_ONLY = frozenset({"CallbackInfo"})


def _cpp_roots() -> list[Path]:
    return [
        REPO_ROOT / "cpp" / "provider",
        REPO_ROOT / "cpp" / "xahau-types",
    ]


def _iter_tagged() -> list[tuple[str, str, Path]]:
    found: list[tuple[str, str, Path]] = []
    for root in _cpp_roots():
        for path in root.rglob("*"):
            if path.suffix not in {".cpp", ".hpp", ".h"}:
                continue
            for line in path.read_text(encoding="utf-8").splitlines():
                match = _TAG.match(line)
                if match:
                    found.append((match.group("plane"), match.group("path"), path))
    return found


def _surface_paths(surface: dict) -> set[str]:
    paths: set[str] = set()
    for name, kind in surface["globals"].items():
        if kind == "function":
            paths.add(name)
    for namespace, members in surface["namespaces"].items():
        paths.update(f"{namespace}.{name}" for name in members)
    for name, spec in surface["prototypes"].items():
        paths.update(f"{name}.{member}" for member in spec["members"])
    for name, members in surface["statics"].items():
        paths.update(f"{name}.{member}" for member in members)
    return paths


def _expand(path: str) -> set[str]:
    names = {path}
    if path.startswith("UInt.") and not path.startswith("UInt8"):
        rest = path[len("UInt.") :]
        names.update(f"UInt{width}.{rest}" for width in _UINT_WIDTHS)
    if path.startswith("Result."):
        names.add("VoidResult." + path[len("Result.") :])
    return names


def test_binding_tags_match_surface():
    surface = json.loads(XAHAU_V1_JAVASCRIPT_SURFACE.read_text())
    required = _surface_paths(surface)
    tagged = _iter_tagged()
    assert tagged, "no // @binding tags found"

    provider = {path for _plane, path, _ in tagged}
    covered = set()
    for path in provider:
        covered.update(_expand(path) & (required | PROVIDER_ONLY | {path}))

    extra = (provider - PROVIDER_ONLY) - required
    extra -= {path for path in extra if _expand(path) & required}
    assert extra == set(), f"provider tags not on the v1 surface: {sorted(extra)}"

    missing = sorted(required - covered)
    assert missing == [], (
        "v1 surface members without a provider @binding:\n  "
        + "\n  ".join(missing)
    )


def test_xahau_types_do_not_call_host():
    root = REPO_ROOT / "cpp" / "xahau-types"
    for path in root.rglob("*"):
        if path.suffix not in {".cpp", ".hpp", ".h"}:
            continue
        text = path.read_text(encoding="utf-8")
        includes = [
            line for line in text.splitlines() if _HOOK_IMPORT.match(line)
        ]
        assert includes == [], f"{path}: xahau-types must not include hook imports"
        hits = _HOST_CALL.findall(text)
        assert hits == [], f"{path}: xahau-types must not call host ({hits})"
