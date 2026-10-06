#!/usr/bin/env bash
#
# ci/uninstall.sh — remove this plugin's locally installed development build.
#
#     ./ci/uninstall.sh            # from the per-user plug-in folders (the default install)
#     ./ci/uninstall.sh --system   # from the shared folders
#     ./ci/uninstall.sh -n         # print what would go, remove nothing
#
# A FORWARDER, NOT A SECOND IMPLEMENTATION. An uninstaller that removes something
# other than exactly what the installer wrote is worse than no uninstaller at
# all, and the destination table is precisely the thing that would drift: two
# files, two chances to disagree about where Components/ stops and CLAP/ starts.
# So the table lives once, in ci/install.sh, and this is the door to it.
#
# It works in a checkout with no build tree: the plugin names come from the
# nassau_add_plugin() call in Source/Plugin/CMakeLists.txt, not from build/out.
#
# Removing nothing is SUCCESS, not failure — "already absent" is the state this
# exists to reach, and a non-zero exit would break the obvious loop
# `for r in nassau-*/; do "$r/ci/uninstall.sh"; done`.
set -euo pipefail

exec "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/install.sh" --uninstall "$@"
