#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/run-linux-product-gate.sh MODE [--commit COMMIT] [--rebuild-image]

Run an exact committed source tree through the pinned Linux poison gate.

  poison   Build and run every provider-static poison probe on Linux.

The checkout is never mounted into Docker. No workstation package cache or
build output is used.
EOF
}

if [[ $# -lt 1 ]]; then
    usage >&2
    exit 2
fi
mode="$1"
shift
case "$mode" in
    poison) ;;
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
            [[ $# -ge 2 ]] || {
                printf '%s\n' '--commit requires a value' >&2
                exit 2
            }
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
    scripts/check-linux-product-gate.py
    scripts/linux-product-gate.lock.env
    scripts/linux-product-gate.sh
    scripts/run-linux-product-gate.sh
)
for path in "${gate_paths[@]}"; do
    [[ -f "$path" ]] || {
        printf 'missing gate authority: %s\n' "$path" >&2
        exit 1
    }
done
git ls-files --error-unmatch -- "${gate_paths[@]}" >/dev/null || {
    printf '%s\n' 'gate authority must be committed before execution' >&2
    exit 1
}
git diff --quiet HEAD -- "${gate_paths[@]}" || {
    printf '%s\n' 'gate authority has uncommitted changes' >&2
    exit 1
}

command -v docker >/dev/null || {
    printf '%s\n' 'docker is required' >&2
    exit 1
}

source scripts/linux-product-gate.lock.env
source_commit="$(git rev-parse --verify "${source_ref}^{commit}")"
authority_commit="$(git rev-parse HEAD)"
image_tag="jshookz-linux-poison:${authority_commit:0:16}"
docker_arch="$(docker info --format '{{.Architecture}}')"
case "$docker_arch" in
    amd64|x86_64) linux_platform=linux/amd64 ;;
    arm64|aarch64) linux_platform=linux/arm64 ;;
    *)
        printf 'unsupported Docker architecture: %s\n' "$docker_arch" >&2
        exit 1
        ;;
esac
buildx_config="${BUILDX_CONFIG:-/tmp/jshookz-linux-buildx-$(id -u)}"
mkdir -p "$buildx_config"
export BUILDX_CONFIG="$buildx_config"

build_args=(
    --platform "$linux_platform"
    --build-arg "BASE_IMAGE=$LINUX_BASE_IMAGE"
    --file .github/docker/linux-product.Dockerfile
    --tag "$image_tag"
    --label "io.jshookz.gate-authority=$authority_commit"
)
if [[ "$rebuild_image" -eq 1 ]]; then
    build_args+=(--no-cache)
fi
docker build "${build_args[@]}" .

printf 'GATE_AUTHORITY_COMMIT=%s\n' "$authority_commit"
printf 'SOURCE_COMMIT=%s\n' "$source_commit"
printf 'LINUX_PLATFORM=%s\n' "$linux_platform"
printf 'LINUX_BASE_IMAGE=%s\n' "$LINUX_BASE_IMAGE"
printf 'DOCKER_IMAGE=%s\n' "$image_tag"

git archive --format=tar "$source_commit" | docker run --rm --init -i \
    --platform "$linux_platform" \
    --env "LINUX_PLATFORM=$linux_platform" \
    --env "JSHOOKZ_SOURCE_COMMIT=$source_commit" \
    "$image_tag" "$mode"
