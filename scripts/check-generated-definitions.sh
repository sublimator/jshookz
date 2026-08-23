#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
xdata="$repo_root/cpp/x-data"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/jshookz-generated-definitions.XXXXXX")"
cleanup() {
    rm -f "$scratch/embedded_xahau_definitions.h" \
        "$scratch/embedded_xrpl_definitions.h" \
        "$scratch/static_xahau_protocol.h"
    rmdir "$scratch"
}
trap cleanup EXIT

python3 "$xdata/scripts/generate_definitions.py" \
    --input "$xdata/definitions/xahau_definitions.json" \
    --output "$scratch/embedded_xahau_definitions.h" \
    --namespace catl::xdata::xahau
python3 "$xdata/scripts/generate_definitions.py" \
    --input "$xdata/definitions/xrpl_definitions.json" \
    --output "$scratch/embedded_xrpl_definitions.h" \
    --namespace catl::xdata::xrpl
python3 "$xdata/scripts/generate_definitions.py" \
    --input "$xdata/definitions/xahau_definitions.json" \
    --output "$scratch/static_xahau_protocol.h" \
    --namespace catl::xdata::xahau_static_data \
    --provider-static \
    --materializer-policy \
    "$xdata/definitions/provider_static_policy.json"

cmp "$scratch/embedded_xahau_definitions.h" \
    "$xdata/generated/embedded_xahau_definitions.h"
cmp "$scratch/embedded_xrpl_definitions.h" \
    "$xdata/generated/embedded_xrpl_definitions.h"
cmp "$scratch/static_xahau_protocol.h" \
    "$xdata/generated/static_xahau_protocol.h"

echo "dynamic and provider-static protocol definitions are fresh"
