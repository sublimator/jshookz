#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
codec="$repo_root/codec/xrpl"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/jshookz-definitions.XXXXXX")"
trap 'rm -rf "$scratch"' EXIT

python3 "$codec/scripts/generate_definitions.py" \
    --input "$codec/x-data/definitions/xahau_definitions.json" \
    --output "$scratch/embedded_xahau_definitions.h" \
    --namespace catl::xdata::xahau
python3 "$codec/scripts/generate_definitions.py" \
    --input "$codec/x-data/definitions/xrpl_definitions.json" \
    --output "$scratch/embedded_xrpl_definitions.h" \
    --namespace catl::xdata::xrpl

cmp "$scratch/embedded_xahau_definitions.h" \
    "$codec/x-data/generated/embedded_xahau_definitions.h"
cmp "$scratch/embedded_xrpl_definitions.h" \
    "$codec/x-data/generated/embedded_xrpl_definitions.h"

echo "embedded protocol definitions are fresh"
