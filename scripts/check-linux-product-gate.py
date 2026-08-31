#!/usr/bin/env python3
"""Check the small shared Linux provider-static poison gate boundary."""

from __future__ import annotations

import argparse
import re
import shutil
import tempfile
from pathlib import Path


FILES = (
    ".dockerignore",
    ".github/docker/linux-product.Dockerfile",
    ".github/workflows/wasm.yml",
    "scripts/linux-product-gate.lock.env",
    "scripts/linux-product-gate.sh",
    "scripts/run-linux-product-gate.sh",
    "scripts/run-tests.sh",
)
LOCK_KEYS = {
    "LINUX_GATE_SCHEMA",
    "LINUX_BASE_IMAGE",
}
WORKFLOW_PRODUCT_TOKENS = (
    "astral-sh/setup-uv@v6",
    "actions/setup-node@v5",
    "python3 -m pip install --user 'conan>=2,<3'",
    "uv sync --project python/jshookz --locked --group dev",
    "uv sync --project python/hostem --locked --group dev",
    "generate_raw_hook_abi.py --check",
    "jshookz build provider",
    "check-generated-definitions.sh",
    "./scripts/run-linux-product-gate.sh poison",
    "conan install cpp --output-folder=build/cpp --build=missing",
    "cmake -S cpp -B build/cpp",
    "CI=1 scripts/run-tests.sh",
    "compile-hook",
    "package-hook",
)


def parse_lock(text: str) -> tuple[dict[str, str], list[str]]:
    values: dict[str, str] = {}
    errors: list[str] = []
    for line in text.splitlines():
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            errors.append(f"invalid lock line: {line!r}")
            continue
        key, value = line.split("=", 1)
        if key in values:
            errors.append(f"duplicate lock key: {key}")
        values[key] = value
    if set(values) != LOCK_KEYS:
        errors.append(f"lock keys differ: {sorted(values)}")
    if not re.fullmatch(
        r"docker\.io/library/ubuntu@sha256:[0-9a-f]{64}",
        values.get("LINUX_BASE_IMAGE", ""),
    ):
        errors.append("Ubuntu image is not digest-pinned")
    return values, errors


def validate(root: Path) -> list[str]:
    errors: list[str] = []
    texts: dict[str, str] = {}
    for relative in FILES:
        path = root / relative
        if not path.is_file():
            errors.append(f"missing gate file: {relative}")
        else:
            texts[relative] = path.read_text(encoding="utf-8")
    if errors:
        return errors

    _, lock_errors = parse_lock(texts["scripts/linux-product-gate.lock.env"])
    errors.extend(lock_errors)

    dockerfile = texts[".github/docker/linux-product.Dockerfile"]
    for token in (
        "# syntax=docker/dockerfile:1.7@sha256:",
        "ARG BASE_IMAGE",
        "FROM ${BASE_IMAGE}",
        "libgtest-dev",
        'ENTRYPOINT ["/opt/jshookz/linux-product-gate.sh"]',
    ):
        if token not in dockerfile:
            errors.append(f"Dockerfile lost token: {token}")

    workflow = texts[".github/workflows/wasm.yml"]
    for token in WORKFLOW_PRODUCT_TOKENS:
        if token not in workflow:
            errors.append(f"workflow lost product step: {token}")
    if workflow.count("./scripts/run-linux-product-gate.sh poison") != 1:
        errors.append("workflow must call the Linux poison gate exactly once")

    tests = texts["scripts/run-tests.sh"]
    for token in (
        "refusing an unscoped local test run",
        "python/jshookz/.venv/bin/pytest",
        "python/hostem/.venv/bin/pytest",
        "ctest --test-dir build/cpp --output-on-failure",
        "--no-tests=error",
        "full_gate=0",
        'for index in "${!pids[@]}"',
        "build/pytest-cache/jshookz",
        "build/pytest-cache/hostem",
        "build/pytest-cache/x-data",
        "scripts/check-wasm-stack.sh",
    ):
        if token not in tests:
            errors.append(f"test driver lost token: {token}")

    gate = texts["scripts/linux-product-gate.sh"]
    for token in (
        "Usage: linux-product-gate.sh poison",
        "cmake -S cpp -B build/cpp -G Ninja",
        "cmake --build build/cpp --target",
        "provider_static_poison_bad_alloc",
        "provider_static_poison_pre_main_malloc",
        "--tests-regex '^provider_static_compiled_poison_'",
        "GATE_RESULT=PASS",
    ):
        if token not in gate:
            errors.append(f"container gate lost token: {token}")

    wrapper = texts["scripts/run-linux-product-gate.sh"]
    for token in (
        "Usage: scripts/run-linux-product-gate.sh MODE",
        'git archive --format=tar "$source_commit"',
        "docker info --format '{{.Architecture}}'",
        "--platform \"$linux_platform\"",
        '--build-arg "BASE_IMAGE=$LINUX_BASE_IMAGE"',
        "git diff --quiet HEAD",
    ):
        if token not in wrapper:
            errors.append(f"host wrapper lost token: {token}")
    if wrapper.count('--platform "$linux_platform"') != 2:
        errors.append("Docker build and run must both use the native platform")
    if "type=bind" in wrapper or "--volume" in wrapper:
        errors.append("host wrapper must not mount the checkout")

    if "install-uv-lock-with-pip" in "\n".join(texts.values()):
        errors.append("custom uv.lock interpretation returned")
    return errors


def self_test(root: Path) -> list[str]:
    failures: list[str] = []
    with tempfile.TemporaryDirectory() as directory:
        fixture = Path(directory)
        for relative in FILES:
            target = fixture / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(root / relative, target)
        if errors := validate(fixture):
            return ["checker baseline is red: " + "; ".join(errors)]

        mutations = (
            (
                ".github/workflows/wasm.yml",
                "./scripts/run-linux-product-gate.sh poison",
                "true # removed Linux poison gate",
                "Linux poison gate exactly once",
            ),
            (
                "scripts/linux-product-gate.sh",
                "provider_static_poison_bad_alloc",
                "removed_poison_target",
                "container gate lost token",
            ),
            (
                "scripts/run-tests.sh",
                "scripts/check-wasm-stack.sh",
                "true # removed stack check",
                "test driver lost token",
            ),
        )
        for relative, old, new, expected in mutations:
            path = fixture / relative
            original = path.read_text(encoding="utf-8")
            path.write_text(original.replace(old, new, 1), encoding="utf-8")
            if not any(expected in error for error in validate(fixture)):
                failures.append(f"mutation stayed green: {relative}: {old}")
            path.write_text(original, encoding="utf-8")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--root", type=Path, default=Path(__file__).resolve().parents[1]
    )
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    errors = (
        self_test(args.root.resolve())
        if args.self_test
        else validate(args.root.resolve())
    )
    if errors:
        print("\n".join(errors))
        return 1
    print(
        "Linux poison gate mutation controls pass"
        if args.self_test
        else "Linux local/CI poison gate parity exact"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
