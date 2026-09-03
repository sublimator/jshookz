#!/usr/bin/env bash
# Publish one sealed provider product as a content-addressed GitHub release.
#
# The release is addressed by the complete bundle: its tag is
# `provider-<sha256 of jshookz_provider.receipt>`, and the receipt pins every
# other byte in the bundle. A consumer that commits the receipt derives the
# download URL from that one file. Publishing identical assets again is a
# no-op; a tag whose asset set or bytes differ is a hard failure and is never
# mutated.
#
# Every asset is exported fresh into an empty staging directory by the
# producer exporter, which validates the seal, the native ABI, and the API
# artifacts before writing; nothing is copied from a working bundle.
#
# Publication is an operator act. Without --publish this script only reports
# what it would do. --publish additionally requires
# OPERATOR_EXPLICITLY_SAID_PUBLISH=true in the environment, which agents must
# never set themselves.
set -euo pipefail

usage() {
    cat <<'EOF'
Usage:
  scripts/release-provider.sh [--product provider] [--publish]

Preconditions (all checked, all read-only):
  - tracked tree clean, HEAD reachable from an origin branch
  - the newest CI run for HEAD concluded success
  - scripts/provider-identity.py check is green
  - the product exports cleanly into an empty staging directory

Only the baseline product has a tracked identity snapshot; publication of
provider-consensus-entropy is refused until one exists.

Assets: the twelve consumer-bundle files, the unwizered wasm, and SHA256SUMS.
Release body: jshookz.provider-release.v1 JSON (notes only, not an asset).
EOF
}

product=provider
publish=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --product)
            [[ $# -ge 2 ]] || { printf '%s\n' '--product requires a name' >&2; exit 2; }
            product="$2"
            shift 2
            ;;
        --publish) publish=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'unknown argument: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
done

repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"
command -v gh >/dev/null || { printf '%s\n' 'gh is required' >&2; exit 1; }
jshookz=python/jshookz/.venv/bin/jshookz
[[ -x "$jshookz" ]] || { printf 'missing %s; run uv sync\n' "$jshookz" >&2; exit 1; }

fail() {
    printf 'release-provider: %s\n' "$*" >&2
    exit 1
}

case "$product" in
    provider)
        build_dir=build/xahau-provider
        ;;
    provider-consensus-entropy)
        fail 'provider-consensus-entropy has no tracked identity snapshot; refusing to publish it'
        ;;
    *) fail "unknown product: $product" ;;
esac

# --- preconditions -----------------------------------------------------------

[[ -z "$(git status --porcelain --untracked-files=no)" ]] ||
    fail 'tracked tree is not clean; commit first'
head_sha="$(git rev-parse HEAD)"
git fetch -q origin
[[ -n "$(git branch -r --contains "$head_sha")" ]] ||
    fail "HEAD $head_sha is not on any origin branch; the release target must exist on GitHub"

repo="$(gh repo view --json nameWithOwner -q .nameWithOwner)"
ci_run="$(gh run list --repo "$repo" --commit "$head_sha" --limit 1 \
    --json databaseId,status,conclusion \
    --jq 'if length == 0 then "none" else "\(.[0].databaseId) \(.[0].status) \(.[0].conclusion)" end')"
case "$ci_run" in
    none) fail "no CI run for $head_sha" ;;
    *" completed success") ci_run_id="${ci_run%% *}" ;;
    *) fail "CI for $head_sha is not green: $ci_run" ;;
esac

scripts/provider-identity.py check >/dev/null ||
    fail 'provider identity check failed; run scripts/relock.sh'
[[ -s "$build_dir/jshookz_provider.unwizered.wasm" ]] ||
    fail "missing $build_dir/jshookz_provider.unwizered.wasm; run jshookz build $product"

# --- assets: exported fresh, validated by the exporter -----------------------

# Stage inside the repository (gitignored build/) so cleanup is never blocked
# by a guard on deletions outside the tree.
mkdir -p build
work="$(mktemp -d "$repo_root/build/release-provider.XXXXXX")"
trap 'rm -rf "$work"' EXIT
stage="$work/assets"
mkdir "$stage"
"$jshookz" export-bundle "$product" -o "$stage" >/dev/null ||
    fail "export of $product failed; the seal is stale or incoherent"
cp "$build_dir/jshookz_provider.unwizered.wasm" "$stage"/
(cd "$stage" && shasum -a 256 -- * | sort -k2 > "$work/SHA256SUMS" && mv "$work/SHA256SUMS" SHA256SUMS)

receipt="$stage/jshookz_provider.receipt"
pin() {
    awk -v key="$1" '$1 == key { print $2; found = 1 } END { exit !found }' "$receipt" ||
        fail "receipt has no $1"
}
[[ "$(pin product)" == "$product" ]] || fail 'receipt names a different product'
wasm_sha="$(pin provider_sha256)"
wasm_size="$(pin provider_size)"
runtime_profile_id="$(pin runtime_profile_id)"
bytecode_abi_id="$(pin bytecode_abi_id)"
wasmtime="$(pin wasmtime_version)"
receipt_sha="$(shasum -a 256 "$receipt" | awk '{print $1}')"
tag="provider-$receipt_sha"

assets=()
while IFS= read -r name; do assets+=("$stage/$name"); done < <(ls "$stage" | sort)

python3 - "$stage" "$head_sha" "$ci_run_id" "$product" "$tag" "$receipt_sha" \
    "$wasm_sha" "$wasm_size" "$runtime_profile_id" "$bytecode_abi_id" "$wasmtime" \
    > "$work/RELEASE_BODY.json" <<'PY'
import hashlib, json, sys
from pathlib import Path
stage, head, ci, product, tag, receipt, sha, size, profile, abi, wasmtime = sys.argv[1:]
stage = Path(stage)
def digest(name):
    return hashlib.sha256((stage / name).read_bytes()).hexdigest()
body = {
    "schema": "jshookz.provider-release.v1",
    "tag": tag,
    "product": product,
    "producer_commit": head,
    "ci_run": ci,
    "receipt_sha256": receipt,
    "wasm_sha256": sha,
    "wasm_size": int(size),
    "unwizered_sha256": digest("jshookz_provider.unwizered.wasm"),
    "values_sha256": digest("jshookz_provider.values.cpp"),
    "runtime_profile_id": profile,
    "bytecode_abi_id": abi,
    "wasmtime": wasmtime,
    "assets": sorted(p.name for p in stage.iterdir()),
}
print(json.dumps(body, indent=2))
PY

printf 'release %s\n  repo    %s\n  target  %s (CI run %s green)\n  product %s\n  wasm    %s (%s bytes)\n  assets  %s\n' \
    "$tag" "$repo" "$head_sha" "$ci_run_id" "$product" "$wasm_sha" "$wasm_size" \
    "$(ls "$stage" | tr '\n' ' ')"

# --- idempotence over the complete asset set ---------------------------------

same_assets() {
    # $1: directory holding a downloaded release; true when its file set and
    # every byte equal the staged assets.
    local dir="$1"
    [[ "$(ls "$dir" | sort)" == "$(ls "$stage" | sort)" ]] || return 1
    (cd "$dir" && shasum -a 256 --quiet -c "$stage/SHA256SUMS") >/dev/null 2>&1 || return 1
    cmp -s "$dir/SHA256SUMS" "$stage/SHA256SUMS"
}

if gh release view "$tag" --repo "$repo" >/dev/null 2>&1; then
    existing="$work/existing"
    mkdir "$existing"
    gh release download "$tag" --repo "$repo" --dir "$existing"
    if same_assets "$existing"; then
        printf '%s\n' "already published with identical assets: $tag"
        exit 0
    fi
    fail "tag $tag exists with a different asset set or bytes; a hash tag is never mutated"
fi

# --- publish -----------------------------------------------------------------

if [[ "$publish" -ne 1 ]]; then
    printf '%s\n' 'dry run: not published (pass --publish with OPERATOR_EXPLICITLY_SAID_PUBLISH=true)'
    exit 0
fi
[[ "${OPERATOR_EXPLICITLY_SAID_PUBLISH:-}" == "true" ]] ||
    fail 'refusing to publish: OPERATOR_EXPLICITLY_SAID_PUBLISH=true was not set by the operator'

gh release create "$tag" --repo "$repo" --target "$head_sha" \
    --title "$product $wasm_sha" \
    --notes-file "$work/RELEASE_BODY.json" \
    "${assets[@]}"

verify="$work/verify"
mkdir "$verify"
gh release download "$tag" --repo "$repo" --dir "$verify"
same_assets "$verify" ||
    fail "published assets do not match the staged bundle; do not consume $tag"
printf 'published %s\n  https://github.com/%s/releases/tag/%s\n' "$tag" "$repo" "$tag"
