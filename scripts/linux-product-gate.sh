#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: linux-product-gate.sh poison

Internal Linux container gate. Source arrives as a git archive on stdin. The
gate builds and runs every provider-static poison probe, including the
bad-allocation probe that exposed issue 0096.
EOF
}

if [[ $# -ne 1 ]]; then
    usage >&2
    exit 2
fi
mode="$1"
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

source /opt/jshookz/linux-product-gate.lock.env
: "${JSHOOKZ_SOURCE_COMMIT:?missing source commit}"
: "${LINUX_PLATFORM:?missing Linux platform}"
[[ "$JSHOOKZ_SOURCE_COMMIT" =~ ^[0-9a-f]{40}$ ]] || {
    printf '%s\n' 'invalid source commit' >&2
    exit 1
}

work_root="$(mktemp -d /tmp/jshookz-linux-poison.XXXXXX)"
repo_root="$work_root/source"
cleanup() {
    case "$work_root" in
        /tmp/jshookz-linux-poison.*) rm -rf -- "$work_root" ;;
        *) printf 'refusing unsafe cleanup path: %s\n' "$work_root" >&2 ;;
    esac
}
trap cleanup EXIT

mkdir -p "$repo_root" "$work_root/home"
tar -xf - -C "$repo_root"
export HOME="$work_root/home"
cd "$repo_root"

source /etc/os-release
printf 'GATE_MODE=%s\n' "$mode"
printf 'SOURCE_COMMIT=%s\n' "$JSHOOKZ_SOURCE_COMMIT"
printf 'LINUX_PLATFORM=%s\n' "$LINUX_PLATFORM"
printf 'LINUX_BASE_IMAGE=%s\n' "$LINUX_BASE_IMAGE"
printf 'UBUNTU=%s\n' "$PRETTY_NAME"
gcc --version | head -1
g++ --version | head -1
cmake --version | head -1
ninja --version

cmake -S cpp -B build/cpp -G Ninja \
    -DCMAKE_BUILD_TYPE=Release

cmake --build build/cpp --target \
    provider_static_poison_dynamic_protocol \
    provider_static_poison_field_types \
    provider_static_poison_ordinary_new \
    provider_static_poison_aligned_new \
    provider_static_poison_vector \
    provider_static_poison_string \
    provider_static_poison_bad_alloc \
    provider_static_poison_pre_main_malloc
ctest --test-dir build/cpp --output-on-failure \
    --tests-regex '^provider_static_compiled_poison_'

printf 'GATE_RESULT=PASS mode=%s source=%s\n' "$mode" "$JSHOOKZ_SOURCE_COMMIT"
