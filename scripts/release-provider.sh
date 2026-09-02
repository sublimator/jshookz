#!/usr/bin/env bash
# Publish one sealed provider product as a content-addressed GitHub release.
#
# The release id is the sealed wasm's SHA-256 (`provider-<sha256>`), so a
# consumer derives the download URL from its own committed lock. Publishing
# the same bytes again is a no-op; a tag whose bytes differ is a hard failure
# and is never mutated.
#
# Publication is an operator act. Without --publish this script only reports
# what it would do. --publish additionally requires
# OPERATOR_EXPLICITLY_SAID_PUBLISH=true in the environment, which agents must
# never set themselves.
set -euo pipefail

usage() {
    cat <<'EOF'
Usage:
  scripts/release-provider.sh [--product provider|provider-consensus-entropy]
                              [--publish]

Preconditions (all checked, all read-only):
  - tracked tree clean, HEAD reachable from an origin branch
  - the newest CI run for HEAD concluded success
  - scripts/provider-identity.py check is green (baseline product)
  - build/<product>-bundle/ exists and its lock matches the sealed wasm

Assets: every consumer-bundle file, the unwizered wasm, and SHA256SUMS.
Release body: jshookz.provider-release.v1 JSON.
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

case "$product" in
    provider)
        build_dir=build/xahau-provider
        bundle_dir=build/xahau-provider-bundle
        ;;
    provider-consensus-entropy)
        build_dir=build/xahau-provider-consensus-entropy
        bundle_dir=build/xahau-provider-consensus-entropy-bundle
        ;;
    *) printf 'unknown product: %s\n' "$product" >&2; exit 2 ;;
esac

repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"
command -v gh >/dev/null || { printf '%s\n' 'gh is required' >&2; exit 1; }

fail() {
    printf 'release-provider: %s\n' "$*" >&2
    exit 1
}

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

if [[ "$product" == provider ]]; then
    scripts/provider-identity.py check >/dev/null ||
        fail 'provider identity check failed; run scripts/relock.sh'
fi

lock="$bundle_dir/jshookz_provider.lock.json"
[[ -s "$lock" ]] ||
    fail "missing consumer bundle $bundle_dir; run jshookz build $product"
[[ -s "$build_dir/jshookz_provider.unwizered.wasm" ]] ||
    fail "missing $build_dir/jshookz_provider.unwizered.wasm"

read -r wasm_sha wasm_size lock_product runtime_profile_id bytecode_abi_id wasmtime < <(
    python3 - "$lock" <<'PY'
import json, sys
lock = json.load(open(sys.argv[1]))
print(lock["provider"]["sha256"], lock["provider"]["size"], lock["product"],
      lock["runtime_profile_id"], lock["bytecode_abi_id"], lock["wasmtime_version"])
PY
)
[[ "$lock_product" == "$product" ]] ||
    fail "bundle lock names product $lock_product, not $product"
actual_sha="$(shasum -a 256 "$bundle_dir/jshookz_provider.wasm" | awk '{print $1}')"
actual_size="$(stat -f %z "$bundle_dir/jshookz_provider.wasm" 2>/dev/null ||
    stat -c %s "$bundle_dir/jshookz_provider.wasm")"
[[ "$actual_sha" == "$wasm_sha" && "$actual_size" == "$wasm_size" ]] ||
    fail 'sealed wasm does not match the bundle lock; rebuild'

tag="provider-$wasm_sha"

# --- assets ------------------------------------------------------------------

stage="$(mktemp -d "${TMPDIR:-/tmp}/release-$tag.XXXXXX")"
trap 'rm -rf "$stage"' EXIT
cp "$bundle_dir"/* "$stage"/
cp "$build_dir/jshookz_provider.unwizered.wasm" "$stage"/
(cd "$stage" && shasum -a 256 -- * | sort -k2 > SHA256SUMS.tmp && mv SHA256SUMS.tmp SHA256SUMS)

python3 - "$stage" "$head_sha" "$ci_run_id" "$product" "$wasm_sha" "$wasm_size" \
    "$runtime_profile_id" "$bytecode_abi_id" "$wasmtime" > "$stage/RELEASE_BODY.json" <<'PY'
import hashlib, json, sys
from pathlib import Path
stage, head, ci, product, sha, size, profile, abi, wasmtime = sys.argv[1:]
stage = Path(stage)
def digest(name):
    return hashlib.sha256((stage / name).read_bytes()).hexdigest()
body = {
    "schema": "jshookz.provider-release.v1",
    "product": product,
    "producer_commit": head,
    "ci_run": ci,
    "wasm_sha256": sha,
    "wasm_size": int(size),
    "unwizered_sha256": digest("jshookz_provider.unwizered.wasm"),
    "manifest_sha256": digest("jshookz_provider.manifest.json"),
    "native_abi_sha256": digest("jshookz_provider.native-abi.json"),
    "consumer_lock_sha256": digest("jshookz_provider.lock.json"),
    "runtime_profile_id": profile,
    "bytecode_abi_id": abi,
    "wasmtime": wasmtime,
}
print(json.dumps(body, indent=2))
PY

printf 'release %s\n  repo    %s\n  target  %s (CI run %s green)\n  product %s\n  assets  %s\n' \
    "$tag" "$repo" "$head_sha" "$ci_run_id" "$product" \
    "$(ls "$stage" | tr '\n' ' ')"

# --- idempotence -------------------------------------------------------------

if gh release view "$tag" --repo "$repo" >/dev/null 2>&1; then
    existing="$(mktemp -d "${TMPDIR:-/tmp}/existing-$tag.XXXXXX")"
    gh release download "$tag" --repo "$repo" --dir "$existing" --pattern 'jshookz_provider.wasm'
    existing_sha="$(shasum -a 256 "$existing/jshookz_provider.wasm" | awk '{print $1}')"
    rm -rf "$existing"
    if [[ "$existing_sha" == "$wasm_sha" ]]; then
        printf '%s\n' "already published with identical bytes: $tag"
        exit 0
    fi
    fail "tag $tag exists with different bytes ($existing_sha); a hash tag is never mutated"
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
    --notes-file "$stage/RELEASE_BODY.json" \
    "$stage"/*

verify="$(mktemp -d "${TMPDIR:-/tmp}/verify-$tag.XXXXXX")"
gh release download "$tag" --repo "$repo" --dir "$verify" --pattern 'jshookz_provider.wasm'
published_sha="$(shasum -a 256 "$verify/jshookz_provider.wasm" | awk '{print $1}')"
rm -rf "$verify"
[[ "$published_sha" == "$wasm_sha" ]] ||
    fail "published wasm hashes to $published_sha, expected $wasm_sha"
printf 'published %s\n  https://github.com/%s/releases/tag/%s\n' "$tag" "$repo" "$tag"
