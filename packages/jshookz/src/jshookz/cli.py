"""CLI entry point for jshookz tooling."""

import argparse
import os
import sys

from . import build, paths


def cmd_build(args: argparse.Namespace) -> int:
    build.build_xahau_hook_provider()
    return 0


def cmd_info(args: argparse.Namespace) -> int:
    """Print project paths and build status."""
    print(f"Repo root:     {paths.SOURCE_CHECKOUT or '<not a source checkout>'}")
    if (
        paths.SOURCE_CHECKOUT is None
        and "JSHOOKZ_PROVIDER_WASM" not in os.environ
    ):
        print("Hook provider: <set JSHOOKZ_PROVIDER_WASM>")
    else:
        print(
            f"Hook provider: {paths.XAHAU_HOOK_PROVIDER_WASM} "
            f"{'✓' if paths.XAHAU_HOOK_PROVIDER_WASM.exists() else '✗'}"
        )
    print(
        f"v1 declarations: {paths.XAHAU_V1_HOOKS_API_DECLARATIONS} "
        f"{'✓' if paths.XAHAU_V1_HOOKS_API_DECLARATIONS.exists() else '✗'}"
    )
    print(
        f"Canonical API:   {paths.CANONICAL_HOOKS_API_DECLARATIONS} "
        f"{'✓' if paths.CANONICAL_HOOKS_API_DECLARATIONS.exists() else '✗'}"
    )
    print(
        f"v1 surface:      {paths.XAHAU_V1_JAVASCRIPT_SURFACE} "
        f"{'✓' if paths.XAHAU_V1_JAVASCRIPT_SURFACE.exists() else '✗'}"
    )
    print(
        f"API artifacts:   {paths.API_ARTIFACT_MANIFEST} "
        f"{'✓' if paths.API_ARTIFACT_MANIFEST.exists() else '✗'}"
    )
    print(f"wasi-sdk:      {paths.WASI_SDK_PATH} {'✓' if paths.WASI_SDK_PATH.exists() else '✗'}")
    return 0


def cmd_compile_hook(args: argparse.Namespace) -> int:
    """Compile TypeScript/JavaScript to provider-compatible Hook bytecode."""
    from .hook_compiler import DEFAULT_DECLARATIONS, compile_hook
    from pathlib import Path

    source = Path(args.source)
    output = Path(args.output) if args.output else source.with_suffix(".qjsc")
    result = compile_hook(
        source,
        wasm_path=args.wasm,
        declarations=args.declarations or DEFAULT_DECLARATIONS,
        tsc=args.tsc,
    )
    output.write_bytes(result.bytecode)
    if args.emit_js:
        Path(args.emit_js).write_text(result.javascript)
    print(output)
    return 0


def cmd_package_hook(args: argparse.Namespace) -> int:
    """Compile source to a self-describing, installable Hook artifact."""
    from pathlib import Path

    from . import paths
    from .hook_artifact import identity_from_hex
    from .hook_compiler import DEFAULT_DECLARATIONS, package_hook
    from .runtime_profile import verify_runtime_profile_lock

    source = Path(args.source)
    output = Path(args.output) if args.output else source.with_suffix(".xqjs")
    provider = Path(args.wasm or paths.XAHAU_HOOK_PROVIDER_WASM)
    if args.profile:
        if args.bytecode_abi_id or args.runtime_profile_id:
            raise ValueError(
                "--profile cannot be combined with explicit deployment identities"
            )
        profile = verify_runtime_profile_lock(args.profile, provider)
        bytecode_abi_id = profile.bytecode_abi_id
        runtime_profile_id = profile.runtime_profile_id
    else:
        if not args.bytecode_abi_id or not args.runtime_profile_id:
            raise ValueError(
                "supply --profile, or both --bytecode-abi-id and "
                "--runtime-profile-id"
            )
        bytecode_abi_id = identity_from_hex(
            args.bytecode_abi_id, "bytecode ABI identity"
        )
        runtime_profile_id = identity_from_hex(
            args.runtime_profile_id, "runtime profile identity"
        )
    result = package_hook(
        source,
        hook_api_version=args.hook_api_version,
        bytecode_abi_id=bytecode_abi_id,
        runtime_profile_id=runtime_profile_id,
        profile_path=args.profile,
        wasm_path=provider,
        declarations=args.declarations or DEFAULT_DECLARATIONS,
        tsc=args.tsc,
    )
    output.write_bytes(result.artifact)
    if args.emit_bytecode:
        Path(args.emit_bytecode).write_bytes(result.bytecode)
    if args.emit_js:
        Path(args.emit_js).write_text(result.javascript)
    print(output)
    return 0


def cmd_lock_profile(args: argparse.Namespace) -> int:
    """Resolve a reviewed profile source against an exact provider binary."""
    import json
    from pathlib import Path

    from .runtime_profile import build_runtime_profile_lock

    lock = build_runtime_profile_lock(args.source, args.wasm)
    output = Path(args.output)
    output.write_text(json.dumps(lock, indent=2, sort_keys=True) + "\n")
    print(output)
    return 0


def cmd_verify_profile(args: argparse.Namespace) -> int:
    """Verify a runtime-profile lock and provider byte for byte."""
    from .runtime_profile import verify_runtime_profile_lock

    profile = verify_runtime_profile_lock(args.profile, args.wasm)
    print(f"bytecode_abi_id={profile.bytecode_abi_id.hex()}")
    print(f"runtime_profile_id={profile.runtime_profile_id.hex()}")
    return 0


def main() -> None:
    parser = argparse.ArgumentParser(
        prog="jshookz",
        description="Compile and package JavaScript/TypeScript Xahau Hooks",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    # build
    p_build = sub.add_parser("build", help="Build the sealed Xahau Hook provider")
    p_build.add_argument(
        "target",
        choices=["provider"],
        default="provider",
        nargs="?",
    )
    p_build.set_defaults(func=cmd_build)

    # info
    p_info = sub.add_parser("info", help="Show project paths and status")
    p_info.set_defaults(func=cmd_info)

    # compile-hook
    p_compile_hook = sub.add_parser(
        "compile-hook",
        help="Compile a .ts/.js Hook with the exact QuickJS WASM provider",
    )
    p_compile_hook.add_argument("source", help="Hook source (.ts or .js)")
    p_compile_hook.add_argument("-o", "--output", help="Output .qjsc path")
    p_compile_hook.add_argument(
        "--wasm", help="Xahau QuickJS provider WASM (defaults to the local build)"
    )
    p_compile_hook.add_argument(
        "--declarations",
        help="Hook API declarations (defaults to exact xahau-quickjs-v1 surface)",
    )
    p_compile_hook.add_argument("--tsc", help="TypeScript compiler executable")
    p_compile_hook.add_argument(
        "--emit-js", help="Also write the intermediate JavaScript to this path"
    )
    p_compile_hook.set_defaults(func=cmd_compile_hook)

    # package-hook
    p_package_hook = sub.add_parser(
        "package-hook",
        help="Compile and package a .ts/.js Hook for on-ledger installation",
    )
    p_package_hook.add_argument("source", help="Hook source (.ts or .js)")
    p_package_hook.add_argument("-o", "--output", help="Output .xqjs path")
    p_package_hook.add_argument(
        "--hook-api-version",
        type=int,
        default=1,
        help="Hook API version bound into the artifact (default: 1)",
    )
    p_package_hook.add_argument(
        "--profile",
        help="Verified runtime-profile lock supplying both deployment identities",
    )
    p_package_hook.add_argument(
        "--bytecode-abi-id",
        help="64 hexadecimal digits identifying the exact QuickJS decoder ABI",
    )
    p_package_hook.add_argument(
        "--runtime-profile-id",
        help="64 hexadecimal digits identifying the execution profile",
    )
    p_package_hook.add_argument(
        "--wasm", help="Xahau QuickJS provider WASM (defaults to the local build)"
    )
    p_package_hook.add_argument(
        "--declarations",
        help="Hook API declarations (defaults to exact xahau-quickjs-v1 surface)",
    )
    p_package_hook.add_argument("--tsc", help="TypeScript compiler executable")
    p_package_hook.add_argument(
        "--emit-bytecode", help="Also write the internal raw .qjsc payload"
    )
    p_package_hook.add_argument(
        "--emit-js", help="Also write the intermediate JavaScript"
    )
    p_package_hook.set_defaults(func=cmd_package_hook)

    # lock-profile
    p_lock_profile = sub.add_parser(
        "lock-profile",
        help="Resolve a runtime-profile source against an exact provider WASM",
    )
    p_lock_profile.add_argument("source", help="Reviewed profile source JSON")
    p_lock_profile.add_argument("--wasm", required=True, help="Exact provider WASM")
    p_lock_profile.add_argument("-o", "--output", required=True, help="Output lock JSON")
    p_lock_profile.set_defaults(func=cmd_lock_profile)

    # verify-profile
    p_verify_profile = sub.add_parser(
        "verify-profile",
        help="Verify a runtime-profile lock against a provider WASM",
    )
    p_verify_profile.add_argument("profile", help="Runtime-profile lock JSON")
    p_verify_profile.add_argument("--wasm", required=True, help="Exact provider WASM")
    p_verify_profile.set_defaults(func=cmd_verify_profile)

    args = parser.parse_args()
    sys.exit(args.func(args))
