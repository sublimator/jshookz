#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/run-native-sanitizers.sh

Configure, build, and test the native C++ product with AddressSanitizer and
UndefinedBehaviorSanitizer. The gate owns build/native-sanitizers and always uses
one Conan/CMake Release profile with explicit -O1 debug-friendly sanitizer
flags. Re-running the command is incremental.
EOF
}

if [[ $# -gt 0 ]]; then
    case "$1" in
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
fi

repo_root="$(git rev-parse --show-toplevel)"
build_dir="$repo_root/build/native-sanitizers"
conan_home="$repo_root/build/conan-home"

command -v cmake >/dev/null || {
    printf '%s\n' 'cmake is required' >&2
    exit 1
}
command -v conan >/dev/null || {
    printf '%s\n' 'conan 2 is required' >&2
    exit 1
}

jobs="${CMAKE_BUILD_PARALLEL_LEVEL:-}"
if [[ -z "$jobs" ]]; then
    if command -v getconf >/dev/null; then
        jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)"
    fi
    if [[ -z "$jobs" ]] && command -v sysctl >/dev/null; then
        jobs="$(sysctl -n hw.logicalcpu 2>/dev/null || true)"
    fi
    jobs="${jobs:-4}"
fi

sanitizer_flags=(
    -O1
    -g
    -fno-omit-frame-pointer
    -fsanitize=address,undefined
    -fno-sanitize-recover=all
)
sanitizer_flags_string="${sanitizer_flags[*]}"

cd "$repo_root"
mkdir -p "$conan_home"
export CONAN_HOME="$conan_home"
conan profile detect --exist-ok
conan_settings=(
    -s compiler.cppstd=23
    -s build_type=Release
)
# Conan's bundled settings currently stop at AppleClang 17 although current
# Xcode identifies as 21. Match the repository's established compatibility
# profile until Conan publishes that compiler bucket; Linux keeps detection.
if [[ "$(uname -s)" == "Darwin" ]]; then
    profile_path="$conan_home/profiles/default"
    if grep -q '^compiler.version=21$' "$profile_path"; then
        perl -pi -e 's/^compiler\.version=21$/compiler.version=17/' "$profile_path"
    fi
fi
conan install cpp \
    --output-folder="$build_dir" \
    --build=missing \
    "${conan_settings[@]}"

cmake -S cpp -B "$build_dir" \
    -DCMAKE_TOOLCHAIN_FILE="$build_dir/conan_toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS_RELEASE="$sanitizer_flags_string -DNDEBUG" \
    -DCMAKE_CXX_FLAGS_RELEASE="$sanitizer_flags_string -DNDEBUG" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"

cmake --build "$build_dir" --parallel "$jobs"

if [[ "$(uname -s)" == "Darwin" ]]; then
    default_asan_options="detect_leaks=0:halt_on_error=1"
else
    default_asan_options="detect_leaks=1:halt_on_error=1"
fi
export ASAN_OPTIONS="${ASAN_OPTIONS:-$default_asan_options}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}"
ctest --test-dir "$build_dir" --output-on-failure --parallel "$jobs"
