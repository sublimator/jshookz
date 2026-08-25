#!/usr/bin/env python3
"""Mechanically keep local Docker and CI on one Linux product authority."""

from __future__ import annotations

import argparse
import re
import shutil
import tempfile
import tomllib
from pathlib import Path


REQUIRED_FILES = (
    ".dockerignore",
    ".github/docker/linux-product.Dockerfile",
    ".github/workflows/wasm.yml",
    "cpp/conanfile.py",
    "python/hostem/uv.lock",
    "python/jshookz/uv.lock",
    "scripts/install-uv-lock-with-pip.py",
    "scripts/linux-product-gate.lock.env",
    "scripts/linux-product-gate.sh",
    "scripts/run-linux-product-gate.sh",
)
REQUIRED_LOCK_KEYS = {
    "LINUX_GATE_SCHEMA",
    "LINUX_PLATFORM",
    "LINUX_BASE_IMAGE",
    "UBUNTU_SNAPSHOT",
    "NODE_VERSION",
    "NODE_SHA256",
    "UV_VERSION",
    "UV_SHA256",
    "UV_LOCK_FORMAT_VERSION",
    "UV_LOCK_REVISION",
    "CONAN_VERSION",
    "WASI_SDK_VERSION",
    "WASI_SDK_SHA256",
    "BINARYEN_VERSION",
    "BINARYEN_SHA256",
    "WIZER_VERSION",
    "WIZER_SHA256",
    "GTEST_VERSION",
    "HOOKZ_URL",
    "HOOKZ_COMMIT",
    "CACHE_SCHEMA",
}
HASH_KEYS = {
    "NODE_SHA256",
    "UV_SHA256",
    "WASI_SDK_SHA256",
    "BINARYEN_SHA256",
    "WIZER_SHA256",
}
REQUIRED_STAGES = {
    "gate-authority": "check_gate_authority",
    "locked-environments": "install_locked_environments",
    "api-artifacts": "verify_api_artifacts",
    "provider-build": "build_provider",
    "f0-identity": "check_f0_identity",
    "generated-definitions": "check_generated_definitions",
    "host-cpp": "build_host_cpp",
    "product-tests": "test_product_surfaces",
    "wasm-stack": "check_wasm_stack",
    "package-smoke": "package_smoke",
}
FUNCTION_TOKENS = {
    "check_gate_authority": (
        "check-linux-product-gate.py",
        "install-uv-lock-with-pip.py --check python/jshookz/uv.lock",
        "install-uv-lock-with-pip.py --check python/hostem/uv.lock",
        "--self-test",
    ),
    "install_locked_environments": (
        "npm ci",
        "install-uv-lock-with-pip.py",
        "python/jshookz/uv.lock",
        "python/hostem/uv.lock",
        'git -C "$hookz_root" fetch',
    ),
    "verify_api_artifacts": (
        "generate_raw_hook_abi.py --check",
        "check-api-artifacts.py",
        "project-readme-examples.py --check",
        "tsconfig.xahau-integration.json",
    ),
    "build_provider": ("jshookz_cli build provider",),
    "check_f0_identity": ("check-f0-provider-identity.py",),
    "check_generated_definitions": ("check-generated-definitions.sh",),
    "build_host_cpp": (
        "conan install cpp",
        "cmake -S cpp",
        "cmake --build build/cpp",
        "ctest --test-dir build/cpp",
    ),
    "test_product_surfaces": (
        "pytest -q python/jshookz/tests",
        "pytest -q python/hostem/tests",
        "pytest -q cpp/x-data/tests",
    ),
    "check_wasm_stack": ("check-wasm-stack.sh",),
    "package_smoke": ("compile-hook", "package-hook", "test -s"),
}


def parse_lock(text: str) -> tuple[dict[str, str], list[str]]:
    values: dict[str, str] = {}
    errors: list[str] = []
    for number, line in enumerate(text.splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        match = re.fullmatch(r"([A-Z][A-Z0-9_]*)=([^\s]+)", line)
        if match is None:
            errors.append(f"lock line {number} is not KEY=value")
            continue
        key, value = match.groups()
        if key in values:
            errors.append(f"duplicate lock key: {key}")
        values[key] = value
    missing = REQUIRED_LOCK_KEYS - values.keys()
    extra = values.keys() - REQUIRED_LOCK_KEYS
    if missing:
        errors.append(f"missing lock keys: {sorted(missing)}")
    if extra:
        errors.append(f"unexpected lock keys: {sorted(extra)}")
    for key in HASH_KEYS:
        if not re.fullmatch(r"[0-9a-f]{64}", values.get(key, "")):
            errors.append(f"{key} is not an exact SHA-256")
    if values.get("LINUX_PLATFORM") != "linux/amd64":
        errors.append("Linux gate platform is not exact linux/amd64")
    if not re.fullmatch(
        r"docker\.io/library/ubuntu@sha256:[0-9a-f]{64}",
        values.get("LINUX_BASE_IMAGE", ""),
    ):
        errors.append("Linux base image is not an exact Ubuntu manifest digest")
    if not re.fullmatch(r"\d{8}T\d{6}Z", values.get("UBUNTU_SNAPSHOT", "")):
        errors.append("Ubuntu package snapshot is not timestamp-pinned")
    return values, errors


def function_bodies(script: str) -> dict[str, str]:
    matches = list(re.finditer(r"(?m)^([a-z_][a-z0-9_]*)\(\) \{\n", script))
    bodies: dict[str, str] = {}
    for index, match in enumerate(matches):
        end = matches[index + 1].start() if index + 1 < len(matches) else len(script)
        bodies[match.group(1)] = script[match.end() : end]
    return bodies


def validate(root: Path) -> list[str]:
    errors: list[str] = []
    texts: dict[str, str] = {}
    for relative in REQUIRED_FILES:
        path = root / relative
        if not path.is_file():
            errors.append(f"missing shared gate file: {relative}")
        else:
            texts[relative] = path.read_text(encoding="utf-8")
    if errors:
        return errors

    lock = texts["scripts/linux-product-gate.lock.env"]
    lock_values, lock_errors = parse_lock(lock)
    errors.extend(lock_errors)

    dockerignore = texts[".dockerignore"]
    for token in (
        "**",
        "!scripts/linux-product-gate.lock.env",
        "!scripts/linux-product-gate.sh",
    ):
        if token not in dockerignore:
            errors.append(f"Docker context exclusion lost token: {token}")

    conanfile = texts["cpp/conanfile.py"]
    gtest = re.search(r'self\.test_requires\("gtest/([^"\n]+)"\)', conanfile)
    if gtest is None or gtest.group(1) != lock_values.get("GTEST_VERSION"):
        errors.append("Conan GTest version drifted from the Linux gate lock")

    dockerfile = texts[".github/docker/linux-product.Dockerfile"]
    for token in (
        "ARG BASE_IMAGE",
        "FROM ${BASE_IMAGE}",
        "linux-product-gate.lock.env",
        "UBUNTU_SNAPSHOT",
        'ENTRYPOINT ["/opt/jshookz/linux-product-gate.sh"]',
    ):
        if token not in dockerfile:
            errors.append(f"Dockerfile lost authority token: {token}")

    wrapper = texts["scripts/run-linux-product-gate.sh"]
    for token in (
        "source scripts/linux-product-gate.lock.env",
        "git ls-files --error-unmatch",
        "git diff --quiet HEAD",
        'buildx_config="${BUILDX_CONFIG:-/tmp/jshookz-linux-buildx-$(id -u)}"',
        '--platform "$LINUX_PLATFORM"',
        '--build-arg "BASE_IMAGE=$LINUX_BASE_IMAGE"',
        'git archive --format=tar "$source_commit"',
        "target=/cache/conan",
        "target=/cache/pip",
        "target=/cache/npm",
    ):
        if token not in wrapper:
            errors.append(f"Docker wrapper lost authority token: {token}")
    if wrapper.count('--platform "$LINUX_PLATFORM"') < 2:
        errors.append("Docker build and run are not both pinned to linux/amd64")

    gate = texts["scripts/linux-product-gate.sh"]
    for token in (
        'mktemp -d /tmp/jshookz-linux-gate.XXXXXX',
        '/tmp/jshookz-linux-gate.*) rm -rf -- "$work_root"',
    ):
        if token not in gate:
            errors.append(f"ephemeral build-root safety drifted: {token}")
    stages = dict(
        re.findall(r"(?m)^\s*run_stage ([a-z0-9-]+) ([a-z_][a-z0-9_]*)\s*$", gate)
    )
    if stages != REQUIRED_STAGES:
        errors.append(f"full Linux stages drifted: {stages!r}")
    bodies = function_bodies(gate)
    for function, tokens in FUNCTION_TOKENS.items():
        body = bodies.get(function, "")
        if not body:
            errors.append(f"missing stage function: {function}")
            continue
        for token in tokens:
            if token not in body:
                errors.append(f"{function} lost substantive command: {token}")

    workflow = texts[".github/workflows/wasm.yml"]
    if "runs-on: ubuntu-24.04" not in workflow:
        errors.append("product workflow runner is not explicit ubuntu-24.04")
    if "./scripts/run-linux-product-gate.sh full" not in workflow:
        errors.append("product workflow does not call the shared full gate")
    for duplicate in (
        "conan install cpp",
        "jshookz build provider",
        "uv sync --project",
    ):
        if duplicate in workflow:
            errors.append(
                f"workflow duplicates shared substantive command: {duplicate}"
            )
    expected_lock_version = lock_values.get("UV_LOCK_FORMAT_VERSION")
    expected_lock_revision = lock_values.get("UV_LOCK_REVISION")
    for relative in ("python/jshookz/uv.lock", "python/hostem/uv.lock"):
        parsed_uv_lock = tomllib.loads(texts[relative])
        if str(parsed_uv_lock.get("version")) != expected_lock_version:
            errors.append(f"{relative} version drifted from the Linux gate lock")
        if str(parsed_uv_lock.get("revision")) != expected_lock_revision:
            errors.append(f"{relative} revision drifted from the Linux gate lock")

    installer = texts["scripts/install-uv-lock-with-pip.py"]
    for name, expected in (
        ("LOCK_VERSION", expected_lock_version),
        ("LOCK_REVISION", expected_lock_revision),
    ):
        if f"{name} = {expected}" not in installer:
            errors.append(f"uv lock installer {name} drifted from the Linux gate lock")

    hostem_lock = texts["python/hostem/uv.lock"]
    if lock_values.get("HOOKZ_COMMIT", "") not in hostem_lock:
        errors.append("Hookz commit drifted from the hostem uv.lock")
    return errors


def self_test(root: Path) -> list[str]:
    failures: list[str] = []
    with tempfile.TemporaryDirectory() as directory:
        fixture = Path(directory)
        for relative in REQUIRED_FILES:
            target = fixture / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(root / relative, target)
        baseline = validate(fixture)
        if baseline:
            return ["checker baseline is red: " + "; ".join(baseline)]

        mutations = (
            (
                ".github/workflows/wasm.yml",
                "./scripts/run-linux-product-gate.sh full",
                "./scripts/run-linux-product-gate.sh host-cpp",
                "shared full gate",
            ),
            (
                "scripts/linux-product-gate.sh",
                "    run_stage product-tests test_product_surfaces\n",
                "",
                "full Linux stages drifted",
            ),
            (
                "scripts/linux-product-gate.sh",
                "      python/hostem/.venv/bin/python -m pytest -q python/hostem/tests\n",
                "",
                "lost substantive command",
            ),
            (
                "scripts/linux-product-gate.lock.env",
                "WIZER_SHA256=1e9dfaa",
                "WIZER_SHA256=ze9dfaa",
                "exact SHA-256",
            ),
            (
                "python/hostem/uv.lock",
                "revision = 3",
                "revision = 4",
                "revision drifted",
            ),
        )
        originals = {
            relative: (fixture / relative).read_text(encoding="utf-8")
            for relative, *_ in mutations
        }
        for relative, old, new, expected in mutations:
            path = fixture / relative
            source = originals[relative]
            if old not in source:
                failures.append(f"self-test mutation token missing: {old}")
                continue
            path.write_text(source.replace(old, new, 1), encoding="utf-8")
            errors = validate(fixture)
            if not any(expected in error for error in errors):
                failures.append(f"mutation stayed green: {relative}: {expected}")
            path.write_text(source, encoding="utf-8")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--root", type=Path, default=Path(__file__).resolve().parents[1]
    )
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()
    errors = self_test(root) if args.self_test else validate(root)
    if errors:
        print("\n".join(errors))
        return 1
    print(
        "Linux gate parity mutation controls pass"
        if args.self_test
        else "Linux local/CI gate parity exact"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
