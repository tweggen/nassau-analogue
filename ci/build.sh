#!/usr/bin/env bash
#
# ci/build.sh — build this plugin for development, into build/.
#
#     ./ci/build.sh                 # DSP core + tests + bundles if an SDK is found
#     ./ci/build.sh --sdk <dir>     # ...against exactly this SDK checkout
#     ./ci/build.sh --headless      # no UI, so no Skia and no credentials needed
#     ./ci/build.sh --debug         # CMAKE_BUILD_TYPE=Debug (default: Release)
#     ./ci/build.sh --clean         # drop build/ first
#     ./ci/build.sh --test          # ...and run ctest afterwards
#     ./ci/build.sh -- -DFOO=bar    # extra args for the configure step
#
# THIS FILE IS IDENTICAL IN nassau-analogue, nassau-eq, nassau-mangrove2 and
# nassau-zermatt, the way ci/gates.sh already is. Nothing in it names a plugin:
# what this repo produces is read out of Source/Plugin/CMakeLists.txt and out of
# build/out. Fix a bug here and copy the file across; do not fork it.
#
# WHY THIS IS NOT ci/gates.sh. The gate pins everything so that a pass means the
# same thing on every machine: its own build-ci/ tree, an explicit
# NASSAU_SDK_DIR, and a ctest run every time. This is the developer's build —
# build/, whatever SDK the locator finds, tests only when asked. Keeping them in
# separate trees is also why running one does not invalidate the other.
#
# IT DOES NOT INSTALL, AND THAT IS A DECISION. iPlug2 defaults
# IPLUG_DEPLOY_PLUGINS=ON, which adds a POST_BUILD step copying every bundle it
# builds into ~/Library/Audio/Plug-Ins/… — so building this repo standalone used
# to install it, silently, as a side effect. Two reasons that is wrong: a build
# must not modify anything outside its own tree (nassau-suite turns the same flag
# off, for the same reason, with the same note), and once it does, "which copy
# did the host just load?" stops having an answer. OFF here. ci/install.sh
# installs and says where; ci/install.sh --link gives back the fast
# edit-reload loop without giving back the ambiguity.
#
# THE SDK IS NOT RESOLVED HERE. cmake/NassauSDK.cmake owns that order — an
# explicit NASSAU_SDK_DIR, then a sibling ../nassau-plugin-sdk checkout, then a
# pinned FetchContent — and a second implementation is a second answer to the
# question of which SDK you built against. --sdk only forwards.
set -euo pipefail

CI_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$CI_DIR/.." && pwd)"
BUILD_DIR="$REPO_DIR/build"

BUILD_TYPE="${BUILD_TYPE:-Release}"
SDK_DIR=""
HEADLESS=""
CLEAN=""
RUN_TESTS=""
EXTRA_CMAKE_ARGS=()

while [ $# -gt 0 ]; do
    case "$1" in
        --sdk)       SDK_DIR="${2:?--sdk needs a directory}"; shift 2 ;;
        --headless)  HEADLESS="yes"; shift ;;
        --debug)     BUILD_TYPE="Debug"; shift ;;
        --release)   BUILD_TYPE="Release"; shift ;;
        --clean)     CLEAN="yes"; shift ;;
        --test)      RUN_TESTS="yes"; shift ;;
        --)          shift; EXTRA_CMAKE_ARGS=("$@"); break ;;
        -h|--help)   sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "usage: ci/build.sh [--sdk <dir>] [--headless] [--debug|--release]" >&2
           echo "                   [--clean] [--test] [-- <cmake args>]" >&2
           exit 2 ;;
    esac
done

say()  { printf '\n=== %s ===\n' "$*"; }
note() { printf '  %s\n' "$*"; }

CMAKE_EXTRA=()

if [ -n "$SDK_DIR" ]; then
    SDK_DIR="$(cd "$SDK_DIR" && pwd)"
    CMAKE_EXTRA+=( "-DNASSAU_SDK_DIR=$SDK_DIR" )
fi

if [ -n "$HEADLESS" ]; then
    # Said rather than left to be inferred from a missing Skia asset: an inferred
    # headless build and an intended one look identical in the log, and only one
    # of them is a decision.
    CMAKE_EXTRA+=( -DNASSAU_FORCE_HEADLESS=ON )
fi

say "Dev build ($BUILD_TYPE)"
note "repo:  $REPO_DIR"
note "tree:  $BUILD_DIR"
if [ -n "$SDK_DIR" ]; then
    note "sdk:   $SDK_DIR (explicit)"
else
    note "sdk:   whatever cmake/NassauSDK.cmake resolves (see the configure log)"
fi
[ -n "$HEADLESS" ] && note "ui:    forced headless"

if [ -n "$CLEAN" ] && [ -d "$BUILD_DIR" ]; then
    say "Clean"
    note "removing $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

# No -G: CMake honours the CMAKE_GENERATOR environment variable, so a box with
# ninja can pick it without this script caring, and a bare one still works.
say "Configure"
cmake -S "$REPO_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DIPLUG_DEPLOY_PLUGINS=OFF \
    "${CMAKE_EXTRA[@]+${CMAKE_EXTRA[@]}}" \
    "${EXTRA_CMAKE_ARGS[@]+${EXTRA_CMAKE_ARGS[@]}}"

say "Build"
# --config and -C as well as CMAKE_BUILD_TYPE, because the two kinds of
# generator read different ones and this script deliberately passes no -G so
# CMAKE_GENERATOR can choose:
#
#   single-config (Ninja, Makefiles)  CMAKE_BUILD_TYPE decides; --config/-C ignored
#   multi-config  (Visual Studio)     CMAKE_BUILD_TYPE IGNORED; --config/-C decide
#
# Without them a Visual Studio generator silently built DEBUG while the banner
# said Release, and ctest found no tests at all. CMake does say so, in a warning
# that reads like noise: "Manually-specified variables were not used by the
# project: CMAKE_BUILD_TYPE".
cmake --build "$BUILD_DIR" --config "$BUILD_TYPE" -j

if [ -n "$RUN_TESTS" ]; then
    say "Test"
    ctest --test-dir "$BUILD_DIR" -C "$BUILD_TYPE" --output-on-failure
fi

# --- report the OUTCOME, not the options --------------------------------------
# "Plugin targets were possible" and "bundles exist" are different claims, and
# the interesting case — an SDK was found, the DSP core and tests built, and no
# bundle came out because this platform hosts none — has to be distinguishable
# from both. So: read back the SDK the configure actually settled on, and list
# what is really in out/.
say "Built"
RESOLVED_SDK=""
if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
    RESOLVED_SDK="$(sed -n 's/^NASSAU_SDK_DIR:[A-Z]*=//p' "$BUILD_DIR/CMakeCache.txt" | head -1)"
fi
case "$RESOLVED_SDK" in
    ''|/nonexistent) note "SDK:     none — DSP core and tests only" ;;
    *)               note "SDK:     $RESOLVED_SDK" ;;
esac

FOUND=0
if [ -d "$BUILD_DIR/out" ]; then
    for b in "$BUILD_DIR"/out/*; do
        case "$b" in
            *.vst3|*.component|*.clap|*.app|*.exe)
                [ -e "$b" ] || continue
                note "bundle:  $(basename "$b")"
                FOUND=$((FOUND+1)) ;;
        esac
    done
fi

if [ "$FOUND" -eq 0 ]; then
    case "$RESOLVED_SDK" in
        ''|/nonexistent)
            note "bundles: none, because no SDK was resolved. Pass --sdk <dir>, or"
            note "         put a nassau-plugin-sdk checkout beside this repo." ;;
        *)
            note "bundles: NONE, though an SDK was found. On Linux that is expected"
            note "         (no plugin format is built there); anywhere else it is a"
            note "         problem — read the configure log above for the format list." ;;
    esac
else
    note "install: ./ci/install.sh"
fi

say "Done"
