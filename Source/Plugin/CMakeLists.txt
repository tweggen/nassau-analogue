# NassauAnalogue plugin bundles (thin wrapper over the Nassau SDK -> iPlug2).
#
# One call emits VST3 (.vst3), AU v2 (.component) and CLAP (.clap) from the
# same sources, via iPlug2's official iplug_add_plugin(). The SDK picks the UI
# backend: prebuilt Skia + Metal when available, otherwise headless. Pass
# -DNASSAU_FORCE_HEADLESS=ON to force the no-UI build even when Skia is
# present; NassauAnalogueUI.cpp is fully guarded by #if IPLUG_EDITOR, so it
# compiles to nothing in the headless build.
#
# This subdirectory is only added by the root CMakeLists.txt when the Nassau
# SDK + vendored iPlug2 tree are both present AND the platform is macOS or
# Windows (docs/GATES.md G0.2) -- on the Linux dev box this file is never
# processed at all, which is exactly what keeps the DSP core + tests building
# standalone (DESIGN.md §0.4).
#
# Info.plist templates + config.h live in this directory (config.h) and
# resources/ (NassauAnalogue-{VST3,AU,CLAP,macOS}-Info.plist).
#
# APP is the standalone host (iPlug2's IPlugAPP + RTAudio/RTMidi over SWELL).
# It exists so the editor can be rendered and measured without a DAW: it runs
# the real NassauAnalogueUI::Layout against the real platform font metrics,
# which is what the editor's per-platform text fitting has to be checked
# against. Its extra inputs are resources/NassauAnalogue-macOS-Info.plist,
# -macOS-MainMenu.xib, and the SWELL dialog/menu resources main.rc,
# main.rc_mac_dlg, main.rc_mac_menu, resource.h (the last four adapted from
# iPlug2's own IPlugInstrument example, which is where the APP host expects
# to find them: IPlugAPP_main.cpp #includes "resources/main.rc_mac_dlg").
#
# KNOWN ISSUE (macOS 15/26, iPlug2 @ 7dfe7a9): the standalone builds and runs
# -- IGraphics attaches at the right size and the whole editor lays out -- but
# its host WINDOW comes up 0x0 and therefore invisible, so it cannot yet be
# played or screenshotted. The cause is upstream, in IPlugAPP_dialog.cpp's
# ClientResize(): it derives the window/client size difference from gHWND,
# which is the SWELL child view, and that view still has a zero frame at
# WM_INITDIALOG time -- so ptDiff comes out as -PLUG_WIDTH/-PLUG_HEIGHT and
# the SetWindowPos that follows asks for exactly 0x0. Nothing in this repo
# feeds that; the VST3/AU/CLAP builds are unaffected. Headless uses of the
# app (--screenshot, --no-io) still exercise the full editor code path.

nassau_add_plugin(NassauAnalogue
    FORMATS VST3 AU CLAP APP
    SOURCES NassauAnaloguePlugin.cpp NassauAnalogueUI.cpp
    LINK    nassau_synth_core)
