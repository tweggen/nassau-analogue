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
# resources/ (NassauAnalogue-{VST3,AU,CLAP}-Info.plist).

nassau_add_plugin(NassauAnalogue
    FORMATS VST3 AU CLAP
    SOURCES NassauAnaloguePlugin.cpp NassauAnalogueUI.cpp
    LINK    nassau_synth_core)
