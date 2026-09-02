#!/usr/bin/env bash
# Re-derive every committed pin that a runtime or provider change can move,
# in dependency order, so a change and its pins land in one commit instead
# of a change commit followed by a "relock" commit.
set -euo pipefail

usage() {
    cat <<'EOF'
Usage:
  scripts/relock.sh            regenerate every pin below, then list what moved
  scripts/relock.sh --check    run the same gates read-only; non-zero if stale

Pins, in dependency order:
  1. xahau/generated/raw-hook-abi.json and the generated import tables
     (xahau/tools/generate_raw_hook_abi.py)
  2. build/xahau-provider*/ seals and their consumer bundles
     (jshookz build provider, jshookz build provider-consensus-entropy)
  3. scripts/provider.identity.json
     (scripts/provider-identity.py update)
  4. python/jshookz/src/jshookz/types/api-artifacts.json
     (checked only; regenerate declarations with the projection tool)
  5. python/jshookz/tests/runtime-observations.snapshot.json
     (pytest --update-runtime-snapshots on its owning tests)
  6. cpp/x-data/tests/recursive-fuel.snapshot.json
     (cpp/x-data/tests/run_recursive_fuel.py --update-snapshot)

Requires the locked environments (uv sync) and wasi-sdk. Nothing here
publishes, pushes, or touches a consumer checkout.
EOF
}

mode=write
case "${1:-}" in
    "") ;;
    --check) mode=check ;;
    -h|--help) usage; exit 0 ;;
    *) printf 'unknown argument: %s\n' "$1" >&2; usage >&2; exit 2 ;;
esac

repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"

jshookz=python/jshookz/.venv/bin/jshookz
jshookz_pytest=python/jshookz/.venv/bin/pytest
hostem_python=python/hostem/.venv/bin/python
for tool in "$jshookz" "$jshookz_pytest" "$hostem_python"; do
    [[ -x "$tool" ]] || {
        printf 'missing %s; run uv sync for python/jshookz and python/hostem\n' \
            "$tool" >&2
        exit 1
    }
done

snapshot_tests=(
    python/jshookz/tests/test_object_fuel.py
    python/jshookz/tests/test_runtime_types.py
)
# The two tests that own observations in runtime-observations.snapshot.json.
snapshot_select='maximum_topology or nominal_matrix'
pin_paths=(
    xahau/generated
    cpp/provider/generated
    scripts/provider.identity.json
    python/jshookz/src/jshookz/types/api-artifacts.json
    python/jshookz/tests/runtime-observations.snapshot.json
    cpp/x-data/tests/recursive-fuel.snapshot.json
)

step() {
    printf '\n== %s\n' "$1"
}

if [[ "$mode" == check ]]; then
    failed=0
    gate() {
        local label="$1"
        shift
        step "$label"
        if "$@"; then
            printf 'current: %s\n' "$label"
        else
            printf 'STALE: %s\n' "$label" >&2
            failed=1
        fi
    }
    gate "raw Hook ABI" "$hostem_python" xahau/tools/generate_raw_hook_abi.py --check
    gate "provider identity" scripts/provider-identity.py check
    gate "API artifacts" python3 scripts/check-api-artifacts.py
    gate "runtime observation snapshot" "$jshookz_pytest" -q \
        -o cache_dir=build/pytest-cache/relock \
        -k "$snapshot_select" "${snapshot_tests[@]}"
    gate "recursive fuel snapshot" "$hostem_python" \
        cpp/x-data/tests/run_recursive_fuel.py --src cpp/x-data
    exit "$failed"
fi

step "raw Hook ABI"
"$hostem_python" xahau/tools/generate_raw_hook_abi.py

step "seal provider products and export consumer bundles"
"$jshookz" build provider
"$jshookz" build provider-consensus-entropy

step "provider identity"
scripts/provider-identity.py update

step "API artifacts"
python3 scripts/check-api-artifacts.py || {
    printf '%s\n' \
        'API artifacts are stale: regenerate the declarations with the' \
        'projection tool, then rerun scripts/relock.sh' >&2
    exit 1
}

step "runtime observation snapshot"
"$jshookz_pytest" -q -o cache_dir=build/pytest-cache/relock \
    --update-runtime-snapshots -k "$snapshot_select" "${snapshot_tests[@]}"

step "recursive fuel snapshot"
"$hostem_python" cpp/x-data/tests/run_recursive_fuel.py \
    --src cpp/x-data --update-snapshot

step "pins that moved"
moved="$(git status --short -- "${pin_paths[@]}")"
if [[ -n "$moved" ]]; then
    printf '%s\n' "$moved"
    printf '\n%s\n' 'Commit these with the change that moved them.'
else
    printf '%s\n' 'none; every pin already matched'
fi
