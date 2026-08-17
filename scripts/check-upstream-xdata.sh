#!/usr/bin/env bash
# check-upstream-xdata.sh — compare the vendored catl::xdata tree against
# its catalogue-tools upstream checkout.
#
# cpp/x-data/ was vendored from catalogue-tools at
# commit 298b81f (2026-03-29), copied in on 2026-04-03 (commit 5aeb1bd here).
# Several files were then locally modified for WASM (heap lookup table,
# precomputed header_size, sha256 injection, and related WASM adaptations).
#
# Classification per vendored file:
#   IN-SYNC         identical to upstream HEAD (skipped unless -v)
#   UPSTREAM-MOVED  vendored blob matches SOME historical upstream version;
#                   upstream has since changed → candidate to pull
#   LOCAL-MODIFIED  vendored blob never existed upstream → local WASM patch;
#                   must be preserved (or upstreamed) on re-sync
#   NOT-UPSTREAM    file doesn't exist upstream at all
#
# Usage:
#   scripts/check-upstream-xdata.sh [-v]
#   CATALOGUE_TOOLS=/path/to/catalogue-tools scripts/check-upstream-xdata.sh
#
# After a re-sync, update VENDOR_POINT below (and in the provenance doc).

set -euo pipefail

UPSTREAM="${CATALOGUE_TOOLS:-}"
VENDOR_POINT="${VENDOR_POINT:-298b81f}"   # catalogue-tools commit of the vendor snapshot
VERBOSE="${1:-}"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
V="$REPO_ROOT/cpp/x-data"

if [ -z "$UPSTREAM" ] || [ ! -d "$UPSTREAM/.git" ]; then
    echo "error: set CATALOGUE_TOOLS to a catalogue-tools Git checkout" >&2
    exit 1
fi

# vendored relative path -> upstream repo path
map_path() {
    case "$1" in
        base58/*) echo "src/base58/${1#base58/}" ;;
        core/*)   echo "src/core/${1#core/}" ;;
        includes/*|src/*) echo "src/x-data/$1" ;;
        *) echo "" ;;
    esac
}

cd "$UPSTREAM"

in_sync=0
echo "== vendored x-data vs catalogue-tools HEAD ($(git log -1 --format='%h %ad' --date=short HEAD)) =="
while read -r rel; do
    up="$(map_path "$rel")"
    [ -z "$up" ] && continue
    blob=$(git hash-object "$V/$rel")
    head_blob=$(git rev-parse "HEAD:$up" 2>/dev/null || echo missing)

    if [ "$blob" = "$head_blob" ]; then
        in_sync=$((in_sync + 1))
        [ "$VERBOSE" = "-v" ] && echo "IN-SYNC         : $rel"
        continue
    fi
    if [ "$head_blob" = "missing" ]; then
        echo "NOT-UPSTREAM    : $rel"
        continue
    fi

    # Did the vendored blob ever exist in upstream history for this path?
    found=""
    for h in $(git log --format='%H' -- "$up"); do
        if [ "$(git rev-parse "$h:$up" 2>/dev/null)" = "$blob" ]; then
            found=$h
            break
        fi
    done
    if [ -n "$found" ]; then
        echo "UPSTREAM-MOVED  : $rel  (matches upstream @ $(git log -1 --format='%h %ad' --date=short "$found"))"
    else
        echo "LOCAL-MODIFIED  : $rel"
    fi
done < <(cd "$V" && find includes src base58 core -type f \( -name '*.h' -o -name '*.cpp' \) | sort)

echo
echo "($in_sync files in sync with upstream HEAD)"
echo
echo "== upstream commits touching vendored paths since vendor point $VENDOR_POINT =="
git log --format='%h %ad %s' --date=short "$VENDOR_POINT"..HEAD -- src/x-data src/base58 src/core
