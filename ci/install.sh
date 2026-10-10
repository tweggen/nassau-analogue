#!/usr/bin/env bash
#
# ci/install.sh — install this plugin's development build for the current user.
#
#     ./ci/install.sh              # into the per-user plug-in folders, no password
#     ./ci/install.sh --link       # symlink instead of copy: rebuild, reload, done
#     ./ci/install.sh --system     # into the shared folders the release .pkg writes
#     ./ci/install.sh --uninstall  # remove what this would install
#     ./ci/install.sh -n           # print what would happen, change nothing
#
# THIS FILE IS IDENTICAL IN nassau-analogue, nassau-eq, nassau-mangrove2 and
# nassau-zermatt. Nothing in it names a plugin: what to install comes from
# build/out, and what to REMOVE comes from the nassau_add_plugin() call in
# Source/Plugin/CMakeLists.txt — which is what lets uninstall work in a checkout
# with no build tree at all. Fix a bug here and copy it across; do not fork it.
#
# WHERE THINGS GO:
#
#                    --user (default)                      --system
#   macOS  VST3      ~/Library/Audio/Plug-Ins/VST3         /Library/Audio/Plug-Ins/VST3
#          AU        ~/Library/Audio/Plug-Ins/Components   /Library/Audio/Plug-Ins/Components
#          CLAP      ~/Library/Audio/Plug-Ins/CLAP         /Library/Audio/Plug-Ins/CLAP
#          APP       ~/Applications                        /Applications
#   Win    VST3      %LOCALAPPDATA%\Programs\Common\VST3   %COMMONPROGRAMFILES%\VST3
#          CLAP      %LOCALAPPDATA%\Programs\Common\CLAP   %COMMONPROGRAMFILES%\CLAP
#   Linux  VST3      ~/.vst3                               /usr/local/lib/vst3
#          CLAP      ~/.clap                               /usr/local/lib/clap
#
# Those are the standard per-user plug-in locations, not an invention of this
# script: they are the same paths iPlug2's own Deploy.cmake uses, which is what
# every host already scans. The per-user domain needs no administrator password —
# the point for something you reinstall twenty times an afternoon — and it leaves
# whatever a real installer put in the system folders alone.
#
# UNINSTALL SWEEPS FORMATS THIS REPO NO LONGER BUILDS, and says when it does. A
# dropped format leaves its installed copy behind forever otherwise — mangrove2
# removed AU from its FORMATS because the AU editor crashed auval, and a
# MangroveIPlug.app from an older build was still sitting in ~/Applications,
# installed by iPlug2's auto-deploy, with nothing in the world that would remove
# it. A host goes on loading such a copy.
#
# ONE COPY, AND A WARNING WHEN THERE ARE TWO. The product rule is that a plugin
# exists once, in a standard folder, so other hosts can load it. This script can
# only honour half of that by itself, so it does the other half out loud: after
# installing, it looks in the OTHER domain for the same bundle and says so if it
# is there. Two copies is not a build error, it is a silent one — which of them a
# host loads is then down to the host's scan order, not to which you just built.
set -euo pipefail

CI_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$CI_DIR/.." && pwd)"
OUT_DIR="$REPO_DIR/build/out"
DECL="$REPO_DIR/Source/Plugin/CMakeLists.txt"

DOMAIN="user"
METHOD="copy"
MODE="install"
DRY=""

while [ $# -gt 0 ]; do
    case "$1" in
        --user)       DOMAIN="user"; shift ;;
        --system)     DOMAIN="system"; shift ;;
        --link)       METHOD="link"; shift ;;
        --copy)       METHOD="copy"; shift ;;
        --uninstall)  MODE="uninstall"; shift ;;
        -n|--dry-run) DRY="yes"; shift ;;
        -h|--help)    sed -n '2,10p' "$0"; exit 0 ;;
        *) echo "usage: ci/install.sh [--user|--system] [--link] [--uninstall] [-n]" >&2
           exit 2 ;;
    esac
done

say()  { printf '\n=== %s ===\n' "$*"; }
note() { printf '  %s\n' "$*"; }
warn() { printf '  WARNING: %s\n' "$*"; }

SUDO=""
# Every mutation goes through this, so -n cannot drift from the real run: one
# code path, one decision about whether to execute it.
run() {
    if [ -n "$DRY" ]; then
        printf '    would run: %s%s\n' "${SUDO:+sudo }" "$*"
    else
        ${SUDO:+sudo} "$@"
    fi
}

# Writability decides whether sudo is needed, rather than assuming --system does
# and --user does not: ~/Library is awkward on a managed host, and an admin user
# owns /Applications on a stock macOS box without needing a password at all.
need_sudo_for() {
    local probe="$1"
    while [ -n "$probe" ] && [ "$probe" != "/" ] && [ ! -e "$probe" ]; do
        probe="$(dirname "$probe")"
    done
    [ -w "$probe" ] && return 1 || return 0
}

case "$(uname -s)" in
    Darwin)               PLATFORM="macos" ;;
    Linux)                PLATFORM="linux" ;;
    MINGW*|MSYS*|CYGWIN*) PLATFORM="windows" ;;
    *) echo "ci/install.sh: unsupported platform $(uname -s)" >&2; exit 2 ;;
esac

# --- the destination table ----------------------------------------------------
# One function, read by install, by uninstall and by the shadow check, so the
# three cannot disagree about where a format lives. Echoes nothing for a format
# this platform does not host, which is how callers learn to skip it.
dest_for() {   # dest_for <ext> <domain>
    local ext="$1" dom="$2"
    case "$PLATFORM:$dom:$ext" in
        macos:user:vst3)        echo "$HOME/Library/Audio/Plug-Ins/VST3" ;;
        macos:user:component)   echo "$HOME/Library/Audio/Plug-Ins/Components" ;;
        macos:user:clap)        echo "$HOME/Library/Audio/Plug-Ins/CLAP" ;;
        macos:user:app)         echo "$HOME/Applications" ;;
        macos:system:vst3)      echo "/Library/Audio/Plug-Ins/VST3" ;;
        macos:system:component) echo "/Library/Audio/Plug-Ins/Components" ;;
        macos:system:clap)      echo "/Library/Audio/Plug-Ins/CLAP" ;;
        macos:system:app)       echo "/Applications" ;;
        windows:user:vst3)      echo "${LOCALAPPDATA:-$HOME/AppData/Local}/Programs/Common/VST3" ;;
        windows:user:clap)      echo "${LOCALAPPDATA:-$HOME/AppData/Local}/Programs/Common/CLAP" ;;
        windows:system:vst3)    echo "${COMMONPROGRAMFILES:-/c/Program Files/Common Files}/VST3" ;;
        windows:system:clap)    echo "${COMMONPROGRAMFILES:-/c/Program Files/Common Files}/CLAP" ;;
        linux:user:vst3)        echo "$HOME/.vst3" ;;
        linux:user:clap)        echo "$HOME/.clap" ;;
        linux:system:vst3)      echo "/usr/local/lib/vst3" ;;
        linux:system:clap)      echo "/usr/local/lib/clap" ;;
        # AU is macOS-only, and the standalone APP has no agreed install location
        # off macOS — iPlug2's own deploy leaves it in the build directory there,
        # and inventing one here would be a path no other tool knows.
        *) echo "" ;;
    esac
}

OTHER_DOMAIN="system"
[ "$DOMAIN" = "system" ] && OTHER_DOMAIN="user"

# --- what this repo declares --------------------------------------------------
# The nassau_add_plugin(NAME ... FORMATS ...) call is this repo's statement of
# what it produces, and reading it is what lets uninstall work with no build/.
# Best effort by design: if the shape ever changes, install still works from
# build/out and only uninstall-without-a-build-tree degrades.
plugin_names() {
    [ -f "$DECL" ] || return 0
    sed -n 's/^[[:space:]]*nassau_add_plugin([[:space:]]*\([A-Za-z0-9_][A-Za-z0-9_]*\).*/\1/p' "$DECL"
}

plugin_formats() {
    [ -f "$DECL" ] || return 0
    sed -n 's/^[[:space:]]*FORMATS[[:space:]]\{1,\}\([A-Za-z0-9 ]*\).*/\1/p' "$DECL"
}

# The format token a bundle extension belongs to, so the declared FORMATS list
# can be compared against what is actually installed.
format_of_ext() {
    case "$1" in
        vst3)      echo "VST3" ;;
        component) echo "AU" ;;
        clap)      echo "CLAP" ;;
        app)       echo "APP" ;;
        *)         echo "" ;;
    esac
}

# The loadable binary inside a built plugin, or nothing when there is none.
# A plain file (a CLAP or a legacy VST3 off macOS) is its own binary; a bundle
# keeps it under Contents/<arch>/ -- MacOS on macOS, x86_64-win / arm64ec-win
# on Windows, x86_64-linux on Linux. The same rule the hosts apply, so what this
# accepts is what a host can load. ci/build.sh carries the identical function.
bundle_binary() {
    local p="$1" f
    if [ -f "$p" ]; then
        [ -s "$p" ] && echo "$p"
        return 0
    fi
    for f in "$p"/Contents/MacOS/* "$p"/Contents/*-win/* "$p"/Contents/*-linux/*; do
        if [ -f "$f" ] && [ -s "$f" ]; then echo "$f"; return 0; fi
    done
    return 0
}

# =============================================================================
# uninstall
# =============================================================================
if [ "$MODE" = "uninstall" ]; then
    say "Uninstall ($DOMAIN domain)"

    NAMES="$(plugin_names || true)"
    if [ -z "$NAMES" ]; then
        # Fall back to whatever was built, so a repo whose declaration we cannot
        # read is not simply ununinstallable.
        if [ -d "$OUT_DIR" ]; then
            for b in "$OUT_DIR"/*; do
                case "$b" in *.vst3|*.component|*.clap|*.app)
                    n="$(basename "$b")"; NAMES="$NAMES ${n%.*}" ;;
                esac
            done
        fi
    fi
    if [ -z "$NAMES" ]; then
        warn "cannot tell what this repo installs: neither $DECL nor $OUT_DIR"
        warn "says. Nothing removed."
        exit 1
    fi

    # EVERY format is swept, not only the ones this repo still declares, and that
    # is deliberate. A format that gets DROPPED leaves its installed copy behind
    # forever otherwise, and nothing else would ever remove it: mangrove2 took AU
    # out of its FORMATS list because the AU editor crashed auval, and this
    # machine still had a MangroveIPlug.app from 2026-05-24 that iPlug2's old
    # auto-deploy had installed. A stale installed copy of a format a repo
    # stopped producing is the same hazard as a stale staged one, and a host goes
    # on loading it.
    #
    # Swept, but never silently: a removal outside the declared list says so.
    DECLARED="$(plugin_formats || true)"
    REMOVED=0
    for name in $NAMES; do
        for ext in vst3 component clap app; do
            d="$(dest_for "$ext" "$DOMAIN")"
            [ -n "$d" ] || continue
            target="$d/$name.$ext"
            [ -e "$target" ] || [ -L "$target" ] || continue

            verb="remove"; [ -L "$target" ] && verb="unlink"
            fmt="$(format_of_ext "$ext")"
            case "$DECLARED" in
                '')        note "$verb $target" ;;        # no declaration to compare against
                *"$fmt"*)  note "$verb $target" ;;
                *)         note "$verb $target"
                           note "       ^ $fmt is not in this repo's FORMATS any more —"
                           note "         a stale copy from a build that still produced it" ;;
            esac

            need_sudo_for "$target" && SUDO="sudo" || SUDO=""
            run rm -rf -- "$target"
            SUDO=""
            REMOVED=$((REMOVED+1))
        done
    done

    if [ "$REMOVED" -eq 0 ]; then
        note "nothing installed in the $DOMAIN domain for:$NAMES"
        # Not an error: "already absent" is the state this exists to reach, and a
        # non-zero exit would break the obvious loop over several repos.
        exit 0
    fi

    if [ -n "$DRY" ]; then
        say "Dry run"; note "Nothing was removed."
    else
        say "Done"; note "$REMOVED item(s) removed"
    fi
    exit 0
fi

# =============================================================================
# install
# =============================================================================
say "Install ($DOMAIN domain, $METHOD)"

if [ ! -d "$OUT_DIR" ]; then
    echo "  FAIL: no build output at $OUT_DIR" >&2
    echo "  Run ./ci/build.sh first." >&2
    exit 1
fi

if [ "$METHOD" = "link" ] && [ "$PLATFORM" = "windows" ]; then
    # Said rather than attempted: a symlink on Windows needs Developer Mode or
    # elevation, and the failure is a permission error that names neither.
    warn "--link needs Developer Mode or elevation on Windows; copying instead."
    METHOD="copy"
fi

# --- refuse skeletons ---------------------------------------------------------
# iPlug2 creates a bundle's directory tree BEFORE it compiles the binary that
# goes in it, so a failed compile still leaves e.g. NassauAnalogue.vst3/Contents/
# x86_64-win/ in build/out -- empty. Copied, that is a plugin every host skips
# or rejects, while the copy you are looking for is "obviously installed"
# (QBX-140: every Windows install for a week). So every candidate is checked
# BEFORE anything is touched, and one skeleton refuses the whole install: half
# an install is the same ambiguity this script exists to remove.
EMPTY=()
for src in "$OUT_DIR"/*; do
    case "$src" in *.vst3|*.component|*.clap|*.app) ;; *) continue ;; esac
    [ -e "$src" ] || continue
    [ -n "$(bundle_binary "$src")" ] || EMPTY+=("$(basename "$src")")
done
if [ "${#EMPTY[@]}" -gt 0 ]; then
    echo "" >&2
    for e in "${EMPTY[@]}"; do
        echo "  FAIL: $e has no binary in it -- a bundle skeleton, not a plugin." >&2
    done
    echo "  The last build did not finish. Rerun ./ci/build.sh and read its errors;" >&2
    echo "  nothing was installed." >&2
    exit 1
fi

INSTALLED=0
SKIPPED=""
INSTALLED_AU=""
INSTALLED_PATHS=()

for src in "$OUT_DIR"/*; do
    [ -e "$src" ] || continue
    base="$(basename "$src")"
    ext="${base##*.}"
    case "$ext" in
        vst3|component|clap|app) ;;
        *) continue ;;
    esac

    d="$(dest_for "$ext" "$DOMAIN")"
    if [ -z "$d" ]; then
        SKIPPED="$SKIPPED $base"
        continue
    fi

    target="$d/$base"
    note "$base -> $d"
    need_sudo_for "$d" && SUDO="sudo" || SUDO=""
    run mkdir -p "$d"
    run rm -rf -- "$target"
    if [ "$METHOD" = "link" ]; then
        run ln -s "$src" "$target"
    else
        # -R unconditionally: a VST3 is a bundle directory on every platform, a
        # CLAP is a bundle on macOS and a bare file elsewhere, and cp -R copies
        # both shapes correctly. rsync is not on a stock macOS box.
        run cp -R "$src" "$target"
    fi
    SUDO=""
    INSTALLED=$((INSTALLED+1))
    INSTALLED_PATHS+=("$target")
    [ "$ext" = "component" ] && INSTALLED_AU="yes"
done

if [ "$INSTALLED" -eq 0 ]; then
    echo "" >&2
    echo "  FAIL: nothing in $OUT_DIR could be installed." >&2
    [ -n "$SKIPPED" ] && echo "  Found, but not installable on this platform:$SKIPPED" >&2
    exit 1
fi

if [ -n "$SKIPPED" ]; then
    say "Not installed"
    for s in $SKIPPED; do
        note "$s — no agreed install location for this format on $PLATFORM"
    done
    note "iPlug2 leaves the standalone in the build tree on $PLATFORM too; run it"
    note "from $OUT_DIR."
fi

# --- shadowing ----------------------------------------------------------------
SHADOWS=()
for p in "${INSTALLED_PATHS[@]}"; do
    base="$(basename "$p")"
    ext="${base##*.}"
    o="$(dest_for "$ext" "$OTHER_DOMAIN")"
    [ -n "$o" ] || continue
    [ -e "$o/$base" ] && SHADOWS+=("$o/$base") || true
done
if [ "${#SHADOWS[@]}" -gt 0 ]; then
    say "Another copy is installed"
    for s in "${SHADOWS[@]}"; do warn "$s"; done
    warn "The same plugin is now installed in both domains. Which one a host"
    warn "loads is decided by its scan order, not by which one you just built."
    warn "Remove the other with: ./ci/uninstall.sh --$OTHER_DOMAIN"
fi

if [ -n "$INSTALLED_AU" ] && [ -z "$DRY" ]; then
    say "Audio Unit"
    note "macOS caches the AU registry, so a host already running will not see a"
    note "newly installed .component. Restart the host; auval -a lists what the"
    note "system can currently see."
fi

if [ -n "$DRY" ]; then
    say "Dry run"
    note "Nothing was installed."
else
    say "Installed"
    note "$INSTALLED bundle(s)"
    note "Remove them with: ./ci/uninstall.sh$([ "$DOMAIN" = system ] && echo ' --system' || true)"
fi
