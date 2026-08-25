#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: linux-product-gate.sh host-cpp|full

Internal container authority. Read a git archive from stdin, extract it into an
ephemeral build root, and execute either the fast host-C++ gate or the complete
provider product gate. Use scripts/run-linux-product-gate.sh from a checkout.
EOF
}

if [[ $# -ne 1 ]]; then
    usage >&2
    exit 2
fi
mode="$1"
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

source /opt/jshookz/linux-product-gate.lock.env
: "${JSHOOKZ_GATE_AUTHORITY_COMMIT:?missing gate authority commit}"
: "${JSHOOKZ_SOURCE_COMMIT:?missing source commit}"
[[ "$JSHOOKZ_GATE_AUTHORITY_COMMIT" =~ ^[0-9a-f]{40}$ ]] || {
    printf '%s\n' 'invalid gate authority commit' >&2
    exit 1
}
[[ "$JSHOOKZ_SOURCE_COMMIT" =~ ^[0-9a-f]{40}$ ]] || {
    printf '%s\n' 'invalid source commit' >&2
    exit 1
}

work_root="$(mktemp -d /tmp/jshookz-linux-gate.XXXXXX)"
repo_root="$work_root/source"
smoke_root="$work_root/smoke"
hookz_root="$work_root/hookz"
cleanup() {
    case "$work_root" in
        /tmp/jshookz-linux-gate.*) rm -rf -- "$work_root" ;;
        *) printf 'refusing unsafe cleanup path: %s\n' "$work_root" >&2 ;;
    esac
}
trap cleanup EXIT
mkdir -p "$repo_root" "$smoke_root" "$work_root/home"
tar -xf - -C "$repo_root"

export HOME="$work_root/home"
export JSHOOKZ_REPO_ROOT="$repo_root"
cd "$repo_root"

print_identity() {
    source /etc/os-release
    printf 'GATE_MODE=%s\n' "$mode"
    printf 'GATE_AUTHORITY_COMMIT=%s\n' "$JSHOOKZ_GATE_AUTHORITY_COMMIT"
    printf 'SOURCE_COMMIT=%s\n' "$JSHOOKZ_SOURCE_COMMIT"
    printf 'LINUX_PLATFORM=%s\n' "$LINUX_PLATFORM"
    printf 'UBUNTU=%s\n' "$PRETTY_NAME"
    printf 'UBUNTU_SNAPSHOT=%s\n' "$UBUNTU_SNAPSHOT"
    printf 'LOCK_SHA256=%s\n' "$(sha256sum /opt/jshookz/linux-product-gate.lock.env | cut -d' ' -f1)"
    printf 'CACHE_CONAN=%s\n' "${CONAN_HOME:-<unset>}"
    printf 'CACHE_PIP=%s\n' "${PIP_CACHE_DIR:-<unset>}"
    printf 'CACHE_NPM=%s\n' "${npm_config_cache:-<unset>}"
    printf 'UV_VERSION_REPLACED=%s\n' "$UV_VERSION"
    printf 'UV_LOCK_FORMAT=%s revision=%s\n' "$UV_LOCK_FORMAT_VERSION" "$UV_LOCK_REVISION"
    uname -a
    gcc --version | head -1
    g++ --version | head -1
    cmake --version | head -1
    ninja --version
    python3 --version
    python3 -m pip --version
    conan --version
    node --version
    npm --version
    /opt/tools/wasi-sdk/bin/clang --version | head -1
    wasm-opt --version
    wizer --version
    dpkg-query -W -f='${binary:Package}=${Version}\n' \
      build-essential cmake curl gcc g++ libc6 libstdc++6 libboost-dev \
      ninja-build python3 python3-venv xz-utils | sort
}

run_stage() {
    local name="$1"
    local function_name="$2"
    local started finished
    started="$(date +%s)"
    printf '\n=== STAGE %s START %s ===\n' "$name" "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    "$function_name"
    finished="$(date +%s)"
    printf '=== STAGE %s PASS duration_seconds=%s ===\n' "$name" "$((finished - started))"
}

check_gate_authority() {
    cmp scripts/linux-product-gate.lock.env /opt/jshookz/linux-product-gate.lock.env
    python3 scripts/check-linux-product-gate.py
    python3 scripts/check-linux-product-gate.py --self-test
    python3 scripts/install-uv-lock-with-pip.py --self-test
    python3 scripts/install-uv-lock-with-pip.py --check python/jshookz/uv.lock
    python3 scripts/install-uv-lock-with-pip.py --check python/hostem/uv.lock
    python3 scripts/check-f0-provider-identity.py --self-test
}

install_locked_environments() {
    npm ci --prefix python/jshookz/src/jshookz
    python3 scripts/install-uv-lock-with-pip.py \
      python/jshookz/uv.lock python/jshookz/.venv
    python3 scripts/install-uv-lock-with-pip.py \
      python/hostem/uv.lock python/hostem/.venv
    git init -q "$hookz_root"
    git -C "$hookz_root" fetch -q --depth=1 "$HOOKZ_URL" "$HOOKZ_COMMIT"
    git -C "$hookz_root" checkout -q --detach FETCH_HEAD
    test "$(git -C "$hookz_root" rev-parse HEAD)" = "$HOOKZ_COMMIT"
    export PYTHONPATH="$repo_root/python/jshookz/src:$repo_root/python/hostem/src:$hookz_root/src"
    python/jshookz/src/jshookz/node_modules/.bin/tsc --version
    python/jshookz/.venv/bin/python -c 'import pytest, wasmtime, jshookz'
    python/hostem/.venv/bin/python -c 'import hookz, hostem, pytest, wasmtime'
}

jshookz_cli() {
    PYTHONPATH="$repo_root/python/jshookz/src${PYTHONPATH:+:$PYTHONPATH}" \
      python/jshookz/.venv/bin/python -c \
      'from jshookz.cli import main; main()' "$@"
}

verify_api_artifacts() {
    python/hostem/.venv/bin/python xahau/tools/generate_raw_hook_abi.py --check
    python3 scripts/check-api-artifacts.py
    ./scripts/project-readme-examples.py --check
    python/jshookz/src/jshookz/node_modules/.bin/tsc \
      -p python/hostem/tsconfig.xahau-integration.json
}

build_provider() {
    jshookz_cli build provider
}

check_f0_identity() {
    python3 scripts/check-f0-provider-identity.py
}

check_generated_definitions() {
    scripts/check-generated-definitions.sh
}

build_host_cpp() {
    conan profile detect --force
    conan install cpp --output-folder=build/cpp --build=missing \
      -s compiler.cppstd=23 -s build_type=Release
    cmake -S cpp -B build/cpp -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="$repo_root/build/cpp/conan_toolchain.cmake" \
      -DCMAKE_BUILD_TYPE=Release
    cmake --build build/cpp
    ctest --test-dir build/cpp --output-on-failure
}

test_product_surfaces() {
    PYTHONPATH="$repo_root/python/jshookz/src" \
      python/jshookz/.venv/bin/python -m pytest -q python/jshookz/tests
    PYTHONPATH="$repo_root/python/jshookz/src:$repo_root/python/hostem/src:$hookz_root/src" \
      python/hostem/.venv/bin/python -m pytest -q python/hostem/tests
    PYTHONPATH="$repo_root/python/jshookz/src" \
      python/jshookz/.venv/bin/python -m pytest -q cpp/x-data/tests
}

check_wasm_stack() {
    scripts/check-wasm-stack.sh
}

package_smoke() {
    JSHOOKZ_PROVIDER_WASM="$repo_root/build/xahau-provider/jshookz_provider.wasm" \
      jshookz_cli info
    JSHOOKZ_PROVIDER_WASM="$repo_root/build/xahau-provider/jshookz_provider.wasm" \
      jshookz_cli compile-hook \
      "$repo_root/python/hostem/examples/xahau-accept.hook.ts" \
      -o "$smoke_root/smoke.qjsc"
    JSHOOKZ_PROVIDER_WASM="$repo_root/build/xahau-provider/jshookz_provider.wasm" \
      jshookz_cli package-hook \
      "$repo_root/python/hostem/examples/xahau-accept.hook.ts" \
      --profile "$repo_root/build/xahau-provider/jshookz_provider.manifest.json" \
      -o "$smoke_root/smoke.xqjs"
    test -s "$smoke_root/smoke.qjsc"
    test -s "$smoke_root/smoke.xqjs"
}

print_identity
if [[ "$mode" == host-cpp ]]; then
    run_stage host-cpp build_host_cpp
else
    run_stage gate-authority check_gate_authority
    run_stage locked-environments install_locked_environments
    run_stage api-artifacts verify_api_artifacts
    run_stage provider-build build_provider
    run_stage f0-identity check_f0_identity
    run_stage generated-definitions check_generated_definitions
    run_stage host-cpp build_host_cpp
    run_stage product-tests test_product_surfaces
    run_stage wasm-stack check_wasm_stack
    run_stage package-smoke package_smoke
fi
printf '\nGATE_RESULT=PASS mode=%s source=%s authority=%s\n' \
  "$mode" "$JSHOOKZ_SOURCE_COMMIT" "$JSHOOKZ_GATE_AUTHORITY_COMMIT"
