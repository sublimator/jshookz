#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage:
  scripts/run-tests.sh PYTEST_TARGET [...]
  scripts/run-tests.sh --ctest REGEX [--ctest REGEX ...]
  CI=1 scripts/run-tests.sh

Local runs require explicit test files, node ids, or CTest regular expressions.
The unscoped all-suite gate is reserved for CI/publication and runs the four
independent suites concurrently.
EOF
}

repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"

jobs="${CTEST_PARALLEL_LEVEL:-${CMAKE_BUILD_PARALLEL_LEVEL:-4}}"
jshookz_targets=()
hostem_targets=()
xdata_targets=()
ctest_patterns=()
full_gate=0

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
fi

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
if [[ "$full_gate" -eq 1 ]]; then
    start wasm-stack scripts/check-wasm-stack.sh
fi

failed=0
for index in "${!pids[@]}"; do
    if wait "${pids[$index]}"; then
        printf 'passed %s\n' "${labels[$index]}"
    else
        printf 'failed %s\n' "${labels[$index]}" >&2
        failed=1
    fi
done
exit "$failed"
