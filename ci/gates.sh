#!/usr/bin/env bash
#
# ci/gates.sh — this repository's gate, in one command.
#
#     ./ci/gates.sh                        # DSP core + tests, no SDK needed
#     ./ci/gates.sh --sdk <dir>            # ...plus the plugin bundles
#     ./ci/gates.sh --sdk <dir> --headless # ...with no UI (no Skia needed)
#
# The logic lives here and not in a workflow file on purpose: a move off GitHub
# (Codeberg, Forgejo, Woodpecker) should rewrite the YAML that CALLS this, not
# re-implement the gate. It is also the same command a developer runs locally,
# so "it passed on my machine" and "it passed in CI" mean the same thing.
#
# Without --sdk this configures with NASSAU_SDK_DIR=/nonexistent, which the
# SDK locator treats as "no SDK": NASSAU_PLUGIN_TARGETS_POSSIBLE goes false and
# the DSP core and the full test suite build anyway. That path needs no
# submodules, no credentials and no macOS, so it is green out of the box on any
# runner — which is the point of having it separate.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_TYPE="${BUILD_TYPE:-Release}"
SDK_DIR=""
HEADLESS=""

while [ $# -gt 0 ]; do
    case "$1" in
        --sdk)      SDK_DIR="$2"; shift 2 ;;
        --headless) HEADLESS="ON"; shift ;;
        *) echo "usage: ci/gates.sh [--sdk <dir>] [--headless]" >&2; exit 2 ;;
    esac
done

say() { printf '\n=== %s ===\n' "$*"; }

if [ -n "$SDK_DIR" ]; then
    SDK_DIR="$(cd "$SDK_DIR" && pwd)"
    BUILD_DIR="$REPO_DIR/build-ci-plugin"
    say "Gate: DSP core + tests + plugin bundles (SDK: $SDK_DIR)"
else
    # An explicit non-path, so the locator never falls through to a sibling
    # checkout or a network fetch. A gate must not depend on what happens to be
    # next to it on disk.
    SDK_DIR="/nonexistent"
    BUILD_DIR="$REPO_DIR/build-ci"
    say "Gate: DSP core + tests only (no SDK)"
fi

# No -G: CMake honours the CMAKE_GENERATOR environment variable, so a runner
# can pick Ninja without this script caring, and a bare box still works.
CMAKE_EXTRA=()
if [ -n "$HEADLESS" ]; then
    # Say it rather than let the SDK infer it from a missing Skia asset: an
    # inferred headless build and an intended one look identical in the log,
    # and only one of them is a decision.
    CMAKE_EXTRA+=( -DNASSAU_FORCE_HEADLESS=ON )
    echo "    (forcing the headless / no-UI path)"
fi

say "Configure"
cmake -S "$REPO_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DNASSAU_SDK_DIR="$SDK_DIR" \
    "${CMAKE_EXTRA[@]+${CMAKE_EXTRA[@]}}"

say "Build"
cmake --build "$BUILD_DIR" -j

say "Test"
ctest --test-dir "$BUILD_DIR" --output-on-failure

say "Pass"
