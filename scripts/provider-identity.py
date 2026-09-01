#!/usr/bin/env python3
"""Update or verify the exact baseline provider identity snapshot."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import tempfile
from pathlib import Path
from typing import Any


DEFAULT_IDENTITY = Path("scripts/provider.identity.json")
SCHEMA = "jshookz.provider-identity.v1"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def git_head(root: Path) -> str:
    return subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=root,
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()


def snapshot(root: Path, identity_path: Path) -> dict[str, Any]:
    previous = read_json(identity_path)
    files = {
        relative: sha256(root / relative)
        for relative in previous["files"]
    }
    manifest = read_json(root / "build/xahau-provider/jshookz_provider.manifest.json")
    provider = manifest["provider"]
    memories = [entry for entry in provider["exports"] if entry["kind"] == "memory"]
    if len(memories) != 1:
        raise ValueError(f"expected one provider memory export, found {len(memories)}")
    previous_limits = previous["provider"]["limits"]
    limits = manifest["source"]["limits"]
    missing_limits = previous_limits.keys() - limits.keys()
    if missing_limits:
        raise ValueError(f"provider manifest is missing limits: {sorted(missing_limits)}")
    memory = memories[0]
    provider_snapshot = {
        "wasm_size": provider["size"],
        "wasm_sha256": provider["sha256"],
        "bytecode_abi_id": manifest["bytecode_abi_id"],
        "runtime_profile_id": manifest["runtime_profile_id"],
        "import_count": len(provider["imports"]),
        "export_count": len(provider["exports"]),
        "memory": {
            "minimum_pages": memory["minimum_pages"],
            "maximum_pages": memory["maximum_pages"],
            "memory64": memory["memory64"],
            "shared": memory["shared"],
        },
        "limits": {key: limits[key] for key in previous_limits},
    }
    unchanged = (
        previous.get("files") == files
        and previous.get("provider") == provider_snapshot
    )
    return {
        "schema": SCHEMA,
        "control_commit": (
            previous["control_commit"] if unchanged else git_head(root)
        ),
        "files": files,
        "provider": provider_snapshot,
    }


def update(root: Path, identity_path: Path) -> None:
    updated = snapshot(root, identity_path)
    encoded = json.dumps(updated, indent=2) + "\n"
    if identity_path.read_text(encoding="utf-8") == encoded:
        print("provider identity unchanged")
        return
    identity_path.write_text(encoded, encoding="utf-8")
    print(f"updated {identity_path.relative_to(root)}")


def check(root: Path, identity_path: Path) -> list[str]:
    identity = read_json(identity_path)
    errors: list[str] = []
    if identity.get("schema") != SCHEMA:
        errors.append(f"provider identity schema drift: {identity.get('schema')!r}")
    control_commit = identity.get("control_commit", "")
    if not re.fullmatch(r"[0-9a-f]{40}", control_commit):
        errors.append("provider identity control commit is not a full Git object ID")
    for relative, expected in identity["files"].items():
        path = root / relative
        if not path.is_file():
            errors.append(f"missing provider artifact: {relative}")
            continue
        actual = sha256(path)
        if actual != expected:
            errors.append(f"provider hash drift: {relative}: {actual} != {expected}")

    manifest_path = root / "build/xahau-provider/jshookz_provider.manifest.json"
    lock_path = root / "xahau/profiles/xahau-quickjs-v1.lock.json"
    if not manifest_path.is_file() or not lock_path.is_file():
        return errors
    if manifest_path.read_bytes() != lock_path.read_bytes():
        errors.append("generated provider manifest differs from tracked runtime lock")

    manifest = read_json(manifest_path)
    expected_provider = identity["provider"]
    provider = manifest.get("provider", {})
    imports = provider.get("imports", [])
    exports = provider.get("exports", [])
    if len(imports) != expected_provider["import_count"]:
        errors.append(f"provider import count drift: {len(imports)}")
    if {entry.get("module") for entry in imports} != {"env"}:
        errors.append("provider imports are not exactly from env")
    if len(exports) != expected_provider["export_count"]:
        errors.append(f"provider export count drift: {len(exports)}")
    memories = [entry for entry in exports if entry.get("kind") == "memory"]
    if len(memories) != 1:
        errors.append(f"provider memory export count drift: {len(memories)}")
    else:
        actual_memory = {
            key: memories[0].get(key) for key in expected_provider["memory"]
        }
        if actual_memory != expected_provider["memory"]:
            errors.append(f"provider memory shape drift: {actual_memory}")
    if provider.get("sha256") != expected_provider["wasm_sha256"]:
        errors.append("manifest provider SHA-256 drift")
    if provider.get("size") != expected_provider["wasm_size"]:
        errors.append("manifest provider size drift")
    if manifest.get("bytecode_abi_id") != expected_provider["bytecode_abi_id"]:
        errors.append("bytecode ABI identity drift")
    if manifest.get("runtime_profile_id") != expected_provider["runtime_profile_id"]:
        errors.append("runtime profile identity drift")
    limits = manifest.get("source", {}).get("limits", {})
    for key, expected in expected_provider["limits"].items():
        if limits.get(key) != expected:
            errors.append(f"provider limit drift: {key}={limits.get(key)!r}")
    return errors


def self_test() -> list[str]:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        artifact = root / "artifact"
        artifact.write_bytes(b"accepted")
        expected = sha256(artifact)
        artifact.write_bytes(b"mutated")
        if sha256(artifact) == expected:
            return ["identity mutation control stayed green"]
    return []


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "command", choices=("check", "update", "self-test"), nargs="?", default="check"
    )
    parser.add_argument(
        "--root", type=Path, default=Path(__file__).resolve().parents[1]
    )
    parser.add_argument("--identity", type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    identity_path = (args.identity or root / DEFAULT_IDENTITY).resolve()
    if args.command == "update":
        update(root, identity_path)
        return 0
    errors = self_test() if args.command == "self-test" else check(root, identity_path)
    if errors:
        print("\n".join(errors))
        return 1
    print(
        "provider identity mutation control passes"
        if args.command == "self-test"
        else "provider identity exact"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
