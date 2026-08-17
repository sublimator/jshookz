#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
xdata="$repo_root/cpp/x-data"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/jshookz-definitions.XXXXXX")"
trap 'rm -rf "$scratch"' EXIT

python3 "$xdata/scripts/generate_definitions.py" \
    --input "$xdata/definitions/xahau_definitions.json" \
    --output "$scratch/embedded_xahau_definitions.h" \
    --namespace catl::xdata::xahau
python3 "$xdata/scripts/generate_definitions.py" \
    --input "$xdata/definitions/xrpl_definitions.json" \
    --output "$scratch/embedded_xrpl_definitions.h" \
    --namespace catl::xdata::xrpl

cmp "$scratch/embedded_xahau_definitions.h" \
    "$xdata/generated/embedded_xahau_definitions.h"
cmp "$scratch/embedded_xrpl_definitions.h" \
    "$xdata/generated/embedded_xrpl_definitions.h"

echo "embedded protocol definitions are fresh"
