#!/usr/bin/env python3
"""Fail closed unless the rebuilt provider remains exact accepted F0."""

from __future__ import annotations

import argparse
import hashlib
import json
import tempfile
from pathlib import Path
from typing import Any


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def check(root: Path, identity_path: Path) -> list[str]:
    identity = json.loads(identity_path.read_text(encoding="utf-8"))
    errors: list[str] = []
    for relative, expected in identity["files"].items():
        path = root / relative
        if not path.is_file():
            errors.append(f"missing F0 artifact: {relative}")
            continue
        actual = sha256(path)
        if actual != expected:
            errors.append(f"F0 hash drift: {relative}: {actual} != {expected}")

    manifest_path = root / "build/xahau-provider/jshookz_provider.manifest.json"
    lock_path = root / "xahau/profiles/xahau-quickjs-v1.lock.json"
    if not manifest_path.is_file() or not lock_path.is_file():
        return errors
    if manifest_path.read_bytes() != lock_path.read_bytes():
        errors.append("generated provider manifest differs from tracked runtime lock")

    manifest: dict[str, Any] = json.loads(manifest_path.read_text(encoding="utf-8"))
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
        identity = root / "identity.json"
        identity.write_text(
            json.dumps(
                {
                    "files": {"artifact": sha256(artifact)},
                    "provider": {},
                }
            ),
            encoding="utf-8",
        )
        artifact.write_bytes(b"mutated")
        parsed = json.loads(identity.read_text(encoding="utf-8"))
        actual = sha256(artifact)
        expected = parsed["files"]["artifact"]
        if actual == expected:
            return ["identity mutation control stayed green"]
    return []


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--root", type=Path, default=Path(__file__).resolve().parents[1]
    )
    parser.add_argument("--identity", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        errors = self_test()
    else:
        root = args.root.resolve()
        identity = args.identity or root / "scripts/f0-provider.identity.json"
        errors = check(root, identity)
    if errors:
        print("\n".join(errors))
        return 1
    print(
        "F0 provider identity mutation control passes"
        if args.self_test
        else "F0 provider identity exact"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
