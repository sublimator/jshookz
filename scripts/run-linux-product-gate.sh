#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/run-linux-product-gate.sh MODE [--commit COMMIT] [--rebuild-image]

Run an exact committed source archive through the pinned Linux/amd64 product
gate. MODE is one of:

  host-cpp  Fast Conan/CMake/build/CTest gate; catches host compiler failures.
  full      Complete provider product gate used by GitHub Actions.

The source checkout is never mounted into the container. The command streams a
git archive into an ephemeral build root, so it cannot leave root-owned build
outputs in the checkout. Named Docker volumes cache only package downloads and
are versioned by scripts/linux-product-gate.lock.env.
EOF
}

if [[ $# -lt 1 ]]; then
    usage >&2
    exit 2
fi

mode="$1"
shift
case "$mode" in
    host-cpp|full) ;;
    -h|--help)
        usage
        exit 0
        ;;
    *)
        printf 'unsupported gate mode: %s\n' "$mode" >&2
        usage >&2
        exit 2
        ;;
esac

source_ref=HEAD
rebuild_image=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --commit)
            [[ $# -ge 2 ]] || { printf '%s\n' '--commit requires a value' >&2; exit 2; }
            source_ref="$2"
            shift 2
            ;;
        --rebuild-image)
            rebuild_image=1
            shift
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

repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"

gate_paths=(
    .dockerignore
    .github/docker/linux-product.Dockerfile
    .github/workflows/wasm.yml
    cpp/conanfile.py
    scripts/check-f0-provider-identity.py
    scripts/check-linux-product-gate.py
    scripts/f0-provider.identity.json
    scripts/install-uv-lock-with-pip.py
    scripts/linux-product-gate.lock.env
    scripts/linux-product-gate.sh
    scripts/run-linux-product-gate.sh
)

for path in "${gate_paths[@]}"; do
    [[ -f "$path" ]] || { printf 'missing gate authority: %s\n' "$path" >&2; exit 1; }
done
if ! git ls-files --error-unmatch -- "${gate_paths[@]}" >/dev/null; then
    printf '%s\n' 'gate authority contains untracked paths; commit it before execution' >&2
    exit 1
fi
if ! git diff --quiet HEAD -- "${gate_paths[@]}"; then
    printf '%s\n' 'gate authority has uncommitted changes; commit it before execution' >&2
    exit 1
fi

command -v docker >/dev/null || { printf '%s\n' 'docker is required' >&2; exit 1; }
buildx_config="${BUILDX_CONFIG:-/tmp/jshookz-linux-buildx-$(id -u)}"
mkdir -p "$buildx_config"
export BUILDX_CONFIG="$buildx_config"

source scripts/linux-product-gate.lock.env
source_commit="$(git rev-parse --verify "${source_ref}^{commit}")"
source_epoch="$(git show -s --format=%ct "$source_commit")"
authority_commit="$(git rev-parse HEAD)"
image_tag="jshookz-linux-product:${authority_commit:0:16}"

build_args=(
    --platform "$LINUX_PLATFORM"
    --build-arg "BASE_IMAGE=$LINUX_BASE_IMAGE"
    --file .github/docker/linux-product.Dockerfile
    --tag "$image_tag"
    --label "io.jshookz.gate-authority=$authority_commit"
)
if [[ "$rebuild_image" -eq 1 ]]; then
    build_args+=(--no-cache)
fi
docker build "${build_args[@]}" .

cache_prefix="jshookz-linux-amd64-v${CACHE_SCHEMA}"
printf 'GATE_AUTHORITY_COMMIT=%s\n' "$authority_commit"
printf 'SOURCE_COMMIT=%s\n' "$source_commit"
printf 'LINUX_PLATFORM=%s\n' "$LINUX_PLATFORM"
printf 'LINUX_BASE_IMAGE=%s\n' "$LINUX_BASE_IMAGE"
printf 'DOCKER_IMAGE=%s\n' "$image_tag"
printf 'BUILDX_CONFIG=%s\n' "$BUILDX_CONFIG"
printf 'CACHE_VOLUMES=%s-{conan,pip,npm}\n' "$cache_prefix"

git archive --format=tar "$source_commit" | docker run --rm --init -i \
    --platform "$LINUX_PLATFORM" \
    --mount "type=volume,source=${cache_prefix}-conan,target=/cache/conan" \
    --mount "type=volume,source=${cache_prefix}-pip,target=/cache/pip" \
    --mount "type=volume,source=${cache_prefix}-npm,target=/cache/npm" \
    --env "CONAN_HOME=/cache/conan" \
    --env "PIP_CACHE_DIR=/cache/pip" \
    --env "npm_config_cache=/cache/npm" \
    --env "JSHOOKZ_GATE_AUTHORITY_COMMIT=$authority_commit" \
    --env "JSHOOKZ_SOURCE_COMMIT=$source_commit" \
    --env "SOURCE_DATE_EPOCH=$source_epoch" \
    "$image_tag" "$mode"
