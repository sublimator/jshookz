#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: linux-product-gate.sh poison|host-cpp

Internal Linux container gate. Source arrives as a git archive on stdin.

  poison   Build and run only the bad-allocation poison probe.
  host-cpp Build every host-C++ target and run complete CTest.
EOF
}

if [[ $# -ne 1 ]]; then
    usage >&2
    exit 2
fi
mode="$1"
case "$mode" in
    poison|host-cpp) ;;
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

work_root="$(mktemp -d /tmp/jshookz-linux-host-cpp.XXXXXX)"
repo_root="$work_root/source"
cleanup() {
    case "$work_root" in
        /tmp/jshookz-linux-host-cpp.*) rm -rf -- "$work_root" ;;
        *) printf 'refusing unsafe cleanup path: %s\n' "$work_root" >&2 ;;
    esac
}
trap cleanup EXIT

mkdir -p "$repo_root" "$work_root/home" "$work_root/conan"
tar -xf - -C "$repo_root"
export HOME="$work_root/home"
export CONAN_HOME="$work_root/conan"
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
python3 --version
conan --version

conan profile detect --force
conan install cpp --output-folder=build/cpp --build=missing \
    -s compiler.cppstd=23 -s build_type=Release
cmake -S cpp -B build/cpp -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$repo_root/build/cpp/conan_toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release

if [[ "$mode" == poison ]]; then
    cmake --build build/cpp --target provider_static_poison_bad_alloc
    ctest --test-dir build/cpp --output-on-failure \
        --tests-regex '^provider_static_compiled_poison_bad_alloc$'
else
    cmake --build build/cpp
    ctest --test-dir build/cpp --output-on-failure
fi

printf 'GATE_RESULT=PASS mode=%s source=%s\n' "$mode" "$JSHOOKZ_SOURCE_COMMIT"
