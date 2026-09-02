#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/run-xahaud-otxn-object-gate.sh --xahaud PATH [--jobs N]

Build the current provider, package the retained Payment Hook with the exact
selected-v1 declarations, and run its external native xahaud proof suite.

The xahaud build directory must already be configured. The test consumes live
paths from this checkout; exact artifact identities belong in the receipt.
EOF
}

xahaud_root="${XAHAUD_REPO:-}"
jobs="${CMAKE_BUILD_PARALLEL_LEVEL:-}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --xahaud)
            [[ $# -ge 2 ]] || {
                printf '%s\n' '--xahaud requires a path' >&2
                exit 2
            }
            xahaud_root="$2"
            shift 2
            ;;
        --jobs)
            [[ $# -ge 2 ]] || {
                printf '%s\n' '--jobs requires a number' >&2
                exit 2
            }
            jobs="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            printf 'unknown argument: %s\n' "$1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

[[ -n "$xahaud_root" ]] || {
    printf '%s\n' 'provide --xahaud PATH or XAHAUD_REPO' >&2
    exit 2
}

repo_root="$(git rev-parse --show-toplevel)"
xahaud_root="$(cd "$xahaud_root" && pwd)"
jshookz="$repo_root/python/jshookz/.venv/bin/jshookz"
provider_dir="$repo_root/build/xahau-provider"
# The complete consumer bundle `jshookz build provider` exports: wasm, receipt,
# preprojected values, provenance JSON, and API artifacts.
live_bundle="$repo_root/build/xahau-provider-bundle"
hook_source="$repo_root/python/hostem/examples/accept-incoming-xah.hook.ts"
hook_bytecode="$repo_root/build/accept-incoming-xah.qjsc"
hook_javascript="$repo_root/build/accept-incoming-xah.mjs"
hook_artifact="$repo_root/build/accept-incoming-xah.xqjs"
declarations="$repo_root/python/jshookz/src/jshookz/types/xahau-quickjs-v1.d.ts"

[[ -x "$jshookz" ]] || {
    printf 'missing jshookz environment: %s\n' "$jshookz" >&2
    exit 1
}
[[ -f "$xahaud_root/build/CMakeCache.txt" ]] || {
    printf 'xahaud build is not configured: %s/build\n' "$xahaud_root" >&2
    exit 1
}
command -v hookz >/dev/null || {
    printf '%s\n' 'hookz is required to compile inline Env test Hooks' >&2
    exit 1
}
command -v x-run-tests >/dev/null || {
    printf '%s\n' 'x-run-tests is required' >&2
    exit 1
}

if [[ -z "$jobs" ]]; then
    jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)"
    jobs="${jobs:-4}"
fi

cd "$repo_root"
"$jshookz" build provider
"$jshookz" package-hook "$hook_source" \
    --profile "$provider_dir/jshookz_provider.manifest.json" \
    --wasm "$provider_dir/jshookz_provider.wasm" \
    --declarations "$declarations" \
    --emit-bytecode "$hook_bytecode" \
    --emit-js "$hook_javascript" \
    -o "$hook_artifact"

# xahaud compiles the runtime profile beside the provider identity. Supplying
# only live WASM bytes to a binary built against its branch-pinned bundle is an
# incoherent test set. Point xahaud at the bundle the build above exported and
# reconfigure only that cache path; the xahaud source tree remains untouched.
[[ -s "$live_bundle/jshookz_provider.receipt" ]] || {
    printf 'provider build did not export the consumer bundle: %s\n' \
        "$live_bundle" >&2
    exit 1
}

# HOOKS_TEST_DIR participates in xahaud's configure-time test discovery, so the
# complete external-test environment must precede both CMake and x-run-tests.
export PATH="$repo_root/python/jshookz/.venv/bin:$PATH"
export HOOKS_TEST_DIR="$repo_root/xahau/env-tests"
export XAHAU_QJS_PROVIDER_WASM="$provider_dir/jshookz_provider.wasm"
export XAHAU_QJS_OTXN_OBJECT_XQJS="$hook_artifact"
export XAHAU_QJS_OTXN_OBJECT_QJSC="$hook_bytecode"
export XAHAU_REQUIRE_QJS_PROVIDER_TESTS=1
export CCACHE_DISABLE=1

cmake -S "$xahaud_root" -B "$xahaud_root/build" \
    -DXAHAU_QUICKJS_PROVIDER_BUNDLE_DIR="$live_bundle"

cd "$xahaud_root"
x-run-tests \
    --no-conan \
    --no-ccache \
    --build-type Release \
    --build-dir build \
    -j "$jobs" \
    -- ripple.app.JSHookzOtxnObjectBoundary
