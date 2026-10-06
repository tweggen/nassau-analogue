#!/usr/bin/env bash
#
# ci/clean.sh — throw away build output without throwing away the checkout.
#
#     ./ci/clean.sh         # build/, the tree ci/build.sh owns
#     ./ci/clean.sh --all   # ...plus the gate's build-ci/ and build-ci-plugin/
#     ./ci/clean.sh -n      # print what would go, remove nothing
#
# THIS FILE IS IDENTICAL IN nassau-analogue, nassau-eq, nassau-mangrove2 and
# nassau-zermatt. Fix a bug here and copy it across; do not fork it.
#
# WHAT IT DELIBERATELY DOES NOT TOUCH: the provisioned SDK. On macOS that is a
# prebuilt Skia unpacked from a release of a PRIVATE repo, so re-fetching it
# needs the gh CLI logged in or GITHUB_TOKEN set. It does not live here anyway —
# it lives in the SDK checkout, and `nassau-plugin-sdk/ci/clean.sh --deps` is the
# one place that can remove it, on purpose.
#
# There is no `git clean -xfd` here either. It would take the provisioned SDK out
# of a sibling checkout's reach the moment someone ran it one directory up, and it
# is never the right tool for "remove what I built".
set -euo pipefail

CI_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$CI_DIR/.." && pwd)"

DRY=""
WANT_ALL=""

while [ $# -gt 0 ]; do
    case "$1" in
        -n|--dry-run) DRY="yes"; shift ;;
        --all)        WANT_ALL="yes"; shift ;;
        -h|--help)    sed -n '2,8p' "$0"; exit 0 ;;
        *) echo "usage: ci/clean.sh [--all] [-n]" >&2; exit 2 ;;
    esac
done

say()  { printf '\n=== %s ===\n' "$*"; }
note() { printf '  %s\n' "$*"; }

# Collected first, acted on second, so -n and the real run cannot disagree about
# what is about to happen.
TARGETS=()
add() { [ -e "$1" ] && TARGETS+=("$1") || true; }

add "$REPO_DIR/build"

if [ -n "$WANT_ALL" ]; then
    # ci/gates.sh writes these two and nothing else cleans them, so they
    # accumulate — and in this repo family they were committed once, for exactly
    # that reason, before .gitignore grew a rule for them.
    add "$REPO_DIR/build-ci"
    add "$REPO_DIR/build-ci-plugin"
fi

if [ "${#TARGETS[@]}" -eq 0 ]; then
    say "Nothing to remove"
    note "Already clean (or the paths do not exist yet)."
    exit 0
fi

say "$([ -n "$DRY" ] && echo "Would remove" || echo "Removing")"
for t in "${TARGETS[@]}"; do
    sz="$(du -sh "$t" 2>/dev/null | cut -f1 || echo '?')"
    printf '  %-6s %s\n' "$sz" "${t#"$REPO_DIR"/}"
done

if [ -n "$DRY" ]; then
    say "Dry run"
    note "Nothing was removed. Re-run without -n."
    exit 0
fi

for t in "${TARGETS[@]}"; do rm -rf -- "$t"; done

say "Kept"
note "the provisioned SDK — it is not ours to remove (nassau-plugin-sdk/ci/clean.sh --deps)"
note "anything already installed — use ./ci/uninstall.sh for that"
[ -z "$WANT_ALL" ] && note "the gate's build-ci/ trees (pass --all to drop them as well)"
say "Done"
note "Next: ./ci/build.sh"
