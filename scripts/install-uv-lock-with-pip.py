#!/usr/bin/env python3
"""Install one uv.lock exactly without executing uv.

Docker Desktop's x86 emulator cannot execute uv's x86 Rust binary on ARM Macs.
This narrow bridge consumes the same lock directly: every registry distribution
is exact-version and hash checked, while editable and Git sources are wired by
the caller from their exact source pins.
"""

from __future__ import annotations

import argparse
import hashlib
import subprocess
import sys
import tempfile
import tomllib
from pathlib import Path
from typing import Any


LOCK_VERSION = 1
LOCK_REVISION = 3


def parse_lock(path: Path) -> tuple[list[str], list[str]]:
    data: dict[str, Any] = tomllib.loads(path.read_text(encoding="utf-8"))
    errors: list[str] = []
    if data.get("version") != LOCK_VERSION:
        errors.append(f"unsupported uv lock version: {data.get('version')!r}")
    if data.get("revision") != LOCK_REVISION:
        errors.append(f"unsupported uv lock revision: {data.get('revision')!r}")
    requirements: list[str] = []
    seen: set[str] = set()
    for package in data.get("package", []):
        source = package.get("source", {})
        if "registry" not in source:
            continue
        name = package.get("name")
        version = package.get("version")
        if not isinstance(name, str) or not isinstance(version, str):
            errors.append("registry package lacks a string name/version")
            continue
        normalized = name.lower().replace("_", "-")
        if normalized in seen:
            errors.append(f"duplicate locked registry package: {name}")
            continue
        seen.add(normalized)
        hashes: set[str] = set()
        sdist = package.get("sdist")
        if isinstance(sdist, dict) and isinstance(sdist.get("hash"), str):
            hashes.add(sdist["hash"])
        for wheel in package.get("wheels", []):
            if isinstance(wheel, dict) and isinstance(wheel.get("hash"), str):
                hashes.add(wheel["hash"])
        invalid = [
            value
            for value in hashes
            if not value.startswith("sha256:") or len(value) != 71
        ]
        if not hashes or invalid:
            errors.append(f"locked package has missing/invalid hashes: {name}")
            continue
        parts = [f"{name}=={version}"]
        parts.extend(f"--hash={value}" for value in sorted(hashes))
        requirements.append(" ".join(parts))
    if not requirements:
        errors.append("uv lock contains no registry distributions")
    return sorted(requirements), errors


def install(lock: Path, environment: Path) -> list[str]:
    requirements, errors = parse_lock(lock)
    if errors:
        return errors
    subprocess.run([sys.executable, "-m", "venv", str(environment)], check=True)
    with tempfile.NamedTemporaryFile("w", encoding="utf-8") as stream:
        stream.write("\n".join(requirements) + "\n")
        stream.flush()
        subprocess.run(
            [
                str(environment / "bin/python"),
                "-m",
                "pip",
                "install",
                "--disable-pip-version-check",
                "--no-deps",
                "--only-binary=:all:",
                "--require-hashes",
                "--requirement",
                stream.name,
            ],
            check=True,
        )
    digest = hashlib.sha256(lock.read_bytes()).hexdigest()
    print(f"installed uv lock: {lock} sha256={digest} packages={len(requirements)}")
    return []


def check(lock: Path) -> list[str]:
    requirements, errors = parse_lock(lock)
    if errors:
        return errors
    digest = hashlib.sha256(lock.read_bytes()).hexdigest()
    print(f"checked uv lock: {lock} sha256={digest} packages={len(requirements)}")
    return []


def self_test() -> list[str]:
    with tempfile.TemporaryDirectory() as directory:
        lock = Path(directory) / "uv.lock"
        lock.write_text(
            """version = 1
revision = 3
[[package]]
name = "fixture"
version = "1.0"
source = { registry = "https://example.invalid/simple" }
wheels = [{ url = "https://example.invalid/fixture.whl", hash = "sha256:bad" }]
""",
            encoding="utf-8",
        )
        _, errors = parse_lock(lock)
        if not any("invalid hashes" in error for error in errors):
            return ["uv lock hash mutation control stayed green"]
    return []


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("lock", nargs="?", type=Path)
    parser.add_argument("environment", nargs="?", type=Path)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test and args.check:
        parser.error("--self-test and --check are mutually exclusive")
    if args.self_test:
        errors = self_test()
    elif args.check:
        if args.lock is None or args.environment is not None:
            parser.error("--check requires exactly one lock path")
        errors = check(args.lock)
    elif args.lock is None or args.environment is None:
        parser.error("lock and environment are required outside --self-test")
    else:
        errors = install(args.lock, args.environment)
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print(
        "uv lock installer mutation control passes"
        if args.self_test
        else "uv lock exact"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
