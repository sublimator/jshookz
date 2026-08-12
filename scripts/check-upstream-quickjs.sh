#!/usr/bin/env bash
# check-upstream-quickjs.sh — how far has this fork drifted from bellard/quickjs?
#
# QuickJS is kept under engine/quickjs. This script compares that subtree with
# the recorded Bellard upstream rather than treating the product root as an
# upstream QuickJS checkout.
#
# Usage: scripts/check-upstream-quickjs.sh

set -euo pipefail
cd "$(dirname "$0")/.."

QUICKJS_UPSTREAM_URL="${QUICKJS_UPSTREAM_URL:-https://github.com/bellard/quickjs.git}"
QUICKJS_BRANCH="${QUICKJS_BRANCH:-master}"
QUICKJS_REF="refs/remotes/jshookz-upstream/${QUICKJS_BRANCH}"
QUICKJS_FORK_POINT="${QUICKJS_FORK_POINT:-d7ae12ae71dfd6ab2997527d295014a8996fa0f9}"

echo "== fetching $QUICKJS_UPSTREAM_URL ($QUICKJS_BRANCH) =="
git fetch "$QUICKJS_UPSTREAM_URL" \
    "$QUICKJS_BRANCH:$QUICKJS_REF"

echo
echo "fork point: $(git log -1 --format='%h %ad %s' --date=short "$QUICKJS_FORK_POINT")"
echo

COUNT=$(git rev-list "$QUICKJS_FORK_POINT".."$QUICKJS_REF" --count)
echo "== $COUNT upstream commits since fork =="
git log --oneline "$QUICKJS_FORK_POINT".."$QUICKJS_REF"
echo

echo "== local modifications to upstream-tracked files =="
git diff --stat "$QUICKJS_FORK_POINT" HEAD:engine/quickjs
echo
echo "Tip: git diff $QUICKJS_FORK_POINT HEAD:engine/quickjs -- quickjs.c"
