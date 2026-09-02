#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage:
  scripts/run-tests.sh PYTEST_TARGET [...]
  scripts/run-tests.sh --ctest REGEX [--ctest REGEX ...]
  CI=1 scripts/run-tests.sh [--suite NAME ...]

Local runs require explicit test files, node ids, or CTest regular expressions.
The unscoped all-suite gate is reserved for CI/publication and runs each
product suite serially on the constrained CI runner. --suite limits that gate
to named suites (jshookz, hostem, x-data, ctest, wasm-stack) so parallel CI
jobs can each run one slice of the same serial gate.
EOF
}

repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"

jobs="${CTEST_PARALLEL_LEVEL:-${CMAKE_BUILD_PARALLEL_LEVEL:-4}}"
jshookz_targets=()
hostem_targets=()
xdata_targets=()
ctest_patterns=()
suites=()
full_gate=0

args=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --suite)
            [[ $# -ge 2 ]] || {
                printf '%s\n' '--suite requires a name' >&2
                exit 2
            }
            case "$2" in
                jshookz|hostem|x-data|ctest|wasm-stack) suites+=("$2") ;;
                *)
                    printf 'unknown suite: %s\n' "$2" >&2
                    exit 2
                    ;;
            esac
            shift 2
            ;;
        *)
            args+=("$1")
            shift
            ;;
    esac
done
set -- "${args[@]+"${args[@]}"}"

if [[ $# -eq 0 ]]; then
    case "${CI:-}" in
        1|true|TRUE)
            full_gate=1
            jshookz_targets=(python/jshookz/tests)
            hostem_targets=(python/hostem/tests)
            xdata_targets=(cpp/x-data/tests)
            ctest_patterns=(.)
            ;;
        *)
            printf '%s\n' \
                'refusing an unscoped local test run; pass test targets or set CI=1' >&2
            usage >&2
            exit 2
            ;;
    esac
elif [[ ${#suites[@]} -gt 0 ]]; then
    printf '%s\n' '--suite applies only to the unscoped CI=1 gate' >&2
    exit 2
fi

wanted() {
    [[ ${#suites[@]} -eq 0 ]] && return 0
    local suite
    for suite in "${suites[@]}"; do
        [[ "$suite" == "$1" ]] && return 0
    done
    return 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help)
            usage
            exit 0
            ;;
        --ctest)
            [[ $# -ge 2 ]] || {
                printf '%s\n' '--ctest requires a regular expression' >&2
                exit 2
            }
            ctest_patterns+=("$2")
            shift 2
            ;;
        python/jshookz/tests/*)
            jshookz_targets+=("$1")
            shift
            ;;
        python/hostem/tests/*)
            hostem_targets+=("$1")
            shift
            ;;
        cpp/x-data/tests/*)
            xdata_targets+=("$1")
            shift
            ;;
        *)
            printf 'unsupported test target: %s\n' "$1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

pids=()
labels=()

start() {
    local label="$1"
    shift
    printf 'starting %s\n' "$label"
    "$@" &
    pids+=("$!")
    labels+=("$label")
}

run_serial() {
    local label="$1"
    shift
    printf 'starting %s\n' "$label"
    if "$@"; then
        printf 'passed %s\n' "$label"
    else
        printf 'failed %s\n' "$label" >&2
        failed=1
    fi
}

failed=0

if [[ "$full_gate" -eq 1 ]]; then
    wanted jshookz && run_serial jshookz python/jshookz/.venv/bin/pytest -q \
        -o cache_dir=build/pytest-cache/jshookz "${jshookz_targets[@]}"
    wanted hostem && run_serial hostem python/hostem/.venv/bin/pytest -q \
        -o cache_dir=build/pytest-cache/hostem "${hostem_targets[@]}"
    wanted x-data && run_serial x-data python/jshookz/.venv/bin/pytest -q \
        -o cache_dir=build/pytest-cache/x-data "${xdata_targets[@]}"
    # Leave one runner CPU available to the OS and support processes. Even
    # deterministic fuel lanes have wall-clock timeouts, so the complete CI
    # gate uses one test worker throughout.
    wanted ctest && run_serial ctest ctest --test-dir build/cpp \
        --output-on-failure --no-tests=error --parallel 1 -R '(.)'
    wanted wasm-stack && run_serial wasm-stack scripts/check-wasm-stack.sh
    exit "$failed"
fi

if [[ ${#jshookz_targets[@]} -gt 0 ]]; then
    start jshookz python/jshookz/.venv/bin/pytest -q \
        -o cache_dir=build/pytest-cache/jshookz "${jshookz_targets[@]}"
fi
if [[ ${#hostem_targets[@]} -gt 0 ]]; then
    start hostem python/hostem/.venv/bin/pytest -q \
        -o cache_dir=build/pytest-cache/hostem "${hostem_targets[@]}"
fi
if [[ ${#xdata_targets[@]} -gt 0 ]]; then
    start x-data python/jshookz/.venv/bin/pytest -q \
        -o cache_dir=build/pytest-cache/x-data "${xdata_targets[@]}"
fi
if [[ ${#ctest_patterns[@]} -gt 0 ]]; then
    ctest_regex="$(IFS='|'; printf '%s' "${ctest_patterns[*]}")"
    start ctest ctest --test-dir build/cpp --output-on-failure \
        --no-tests=error --parallel "$jobs" -R "($ctest_regex)"
fi
for index in "${!pids[@]}"; do
    if wait "${pids[$index]}"; then
        printf 'passed %s\n' "${labels[$index]}"
    else
        printf 'failed %s\n' "${labels[$index]}" >&2
        failed=1
    fi
done
exit "$failed"
