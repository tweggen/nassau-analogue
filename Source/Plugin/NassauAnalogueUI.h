#pragma once

// Forward declarations only — no IGraphics include here, so this header is
// harmless to pull in from any translation unit. NassauAnaloguePlugin.h only
// includes it under #if IPLUG_EDITOR, and the entire .cpp body is likewise
// guarded, so a headless build compiles this to nothing and needs no
// IGraphics include paths.
namespace iplug { namespace igraphics { class IGraphics; } }
class NassauAnaloguePlugin;

namespace NassauAnalogueUI {
    // G0: lays out only the two params that are actually wired at this gate
    // (kMasterVolume, kOutputClip) plus a placeholder title — a full,
    // signal-chain-grouped 53-param editor (Osc1/Osc2/Sub+Noise/Mixer+Drive/
    // HPF/LPF/ENV-F/ENV-A/LFO/Poly-Mod/Voice/Stereo/Output, DESIGN.md §1/§11,
    // docs/GATES.md G10.2) is a G10 deliverable and requires a Windows/macOS
    // host to build at all (DESIGN.md §0.4). Every control binds to its
    // parameter by enum index, so IPlug2 handles all UI <-> param sync — this
    // function never writes SynthCore or plugin state directly (R9).
    void Layout(iplug::igraphics::IGraphics& ui, NassauAnaloguePlugin& plugin);
}
