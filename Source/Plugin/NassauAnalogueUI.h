#pragma once

// Forward declarations only — no IGraphics include here, so this header is
// harmless to pull in from any translation unit. NassauAnaloguePlugin.h only
// includes it under #if IPLUG_EDITOR, and the entire .cpp body is likewise
// guarded, so a headless build compiles this to nothing and needs no
// IGraphics include paths.
namespace iplug { namespace igraphics { class IGraphics; } }
class NassauAnaloguePlugin;

namespace NassauAnalogueUI {
    // G10: the full, signal-chain-grouped editor covering every param
    // (DESIGN.md §1/§11,
    // docs/GATES.md G10.2) — VCO 1 / VCO 2 / Sub+Noise / Mixer+Drive / HPF /
    // LPF / ENV-F / ENV-A / LFO / Poly-Mod / Voice / Stereo / Output, plus
    // the factory preset selector and an IVKeyboardControl for auditioning
    // notes without external MIDI (G10.3). Every control binds to its
    // parameter by enum index, so IPlug2 handles all UI <-> param sync — this
    // function never writes SynthCore or plugin state directly (R9). See
    // NassauAnalogueUI.cpp for the layout metrics and window-size derivation.
    void Layout(iplug::igraphics::IGraphics& ui, NassauAnaloguePlugin& plugin);
}
