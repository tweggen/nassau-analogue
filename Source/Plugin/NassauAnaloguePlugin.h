#pragma once
#include "config.h"
#include "IPlug_include_in_plug_hdr.h"
#include "synth_core.h"

#if IPLUG_EDITOR
#include "NassauAnalogueUI.h"   // #if IPLUG_EDITOR guarded; not present headless.
#endif

// FINAL parameter surface (DESIGN.md §11, 53 params, append-only, R4), in its
// FINAL frozen index order. Unlike nassau-zermatt's own G0 (which grew
// PLUG_N_PARAMS gate by gate), this instrument's config.h already fixes
// PLUG_N_PARAMS at 53 from G0 (docs/GATES.md's own G0 "Instrument-specific
// config.h values" snippet) — matching SynthCore's own "declare the complete
// final API early" shape (synth_core.h's class comment). So EParams below
// already names all 53 indices; only kMasterVolume/kOutputClip are actually
// REGISTERED with a GetParam(i)->InitXxx() call in the constructor at G0 (see
// the .cpp) — every other index exists as a frozen position (its enum value
// will never move, R4) but is not yet wired to a live IParam. Each gate below
// adds its own InitXxx() call(s) when it lands, never reordering this enum.
//
// APPEND-ONLY PARAM DISCIPLINE (state compatibility): params are serialized
// positionally by enum index (see nassau_state.h). To keep saved states
// forward/backward compatible, params must NEVER be reordered or inserted in
// the middle — this enum is already complete, so "append-only" from here on
// means "never change a value below", not "add more entries".
enum EParams {
    // ---- G0: indices 0-1 --------------------------------------------------
    kMasterVolume = 0,  //  0  -60 .. +12 dB,        default -6
    kOutputClip,        //  1  bool,                 default on

    // ---- G2 (oscillators, sub, noise, mixer): indices 2-18 -----------------
    kOsc1Wave,          //  2  Saw/Pulse/Tri,         default Saw
    kOsc1Octave,        //  3  16'/8'/4'/2',          default 8'
    kOsc1Fine,          //  4  -50 .. +50 cents,      default 0
    kOsc1PW,            //  5  5 .. 95 %,             default 50
    kOsc1Level,         //  6  0 .. 100 %,            default 100
    kOsc2Wave,          //  7  Saw/Pulse/Tri,         default Saw
    kOsc2Octave,        //  8  16'/8'/4'/2',          default 8'
    kOsc2Semi,          //  9  -12 .. +12,            default 0
    kOsc2Fine,          // 10  -50 .. +50 cents,      default -7
    kOsc2PW,            // 11  5 .. 95 %,             default 50
    kOsc2Level,         // 12  0 .. 100 %,            default 80
    kOsc2Sync,          // 13  bool,                  default off
    kOsc2KeyTrack,      // 14  bool,                  default on
    kSubOctave,         // 15  -1 / -2,               default -1
    kSubLevel,          // 16  0 .. 100 %,            default 0
    kNoiseColor,        // 17  White/Pink,            default White
    kNoiseLevel,        // 18  0 .. 100 %,            default 0

    // ---- G3 (envelopes, LFO, control-rate architecture): indices 19-31 -----
    kEnvFAttack,        // 19  1 .. 10000 ms,         default 2
    kEnvFDecay,         // 20  1 .. 10000 ms,         default 400
    kEnvFSustain,       // 21  0 .. 100 %,            default 30
    kEnvFRelease,       // 22  1 .. 10000 ms,         default 300
    kEnvAAttack,        // 23  1 .. 10000 ms,         default 2
    kEnvADecay,         // 24  1 .. 10000 ms,         default 800
    kEnvASustain,       // 25  0 .. 100 %,            default 80
    kEnvARelease,       // 26  1 .. 10000 ms,         default 250
    kLfoWave,           // 27  Tri/Saw/Ramp/Sqr/S&H,  default Tri
    kLfoRate,           // 28  0.05 .. 30 Hz,         default 5
    kLfoDelay,          // 29  0 .. 3000 ms,          default 0
    kLfoPitchAmount,    // 30  0 .. 100 %,            default 0
    kLfoPwmAmount,      // 31  0 .. 100 %,            default 0

    // ---- G4 (low-pass filter): indices 32-34 -------------------------------
    kLpfSlope,          // 32  24 / 12 dB,            default 24
    kLpfCutoff,         // 33  20 .. 18000 Hz,        default 2000
    kLpfResonance,      // 34  0 .. 100 %,            default 20

    // ---- G5 (LPF modulation): indices 35-37 --------------------------------
    kLpfEnvAmount,      // 35  -100 .. +100 %,        default 40
    kLpfKeyFollow,      // 36  0 .. 100 %,            default 50
    kLpfLfoAmount,      // 37  0 .. 100 %,            default 0

    // ---- G6 (HPF, drive, Poly-Mod, output stage): indices 38-43 -----------
    kDrive,             // 38  0 .. 100 %,            default 15
    kHpfSlope,          // 39  12 / 24 dB,            default 12
    kHpfCutoff,         // 40  20 .. 2000 Hz,         default 20 (= bypass)
    kHpfKeyFollow,      // 41  0 .. 100 %,            default 0
    kPmEnvFToOsc2,      // 42  -100 .. +100 %,        default 0
    kPmEnvFToPw,        // 43  -100 .. +100 %,        default 0

    // ---- G7 (voice allocation, MIDI, polyphony, glide, velocity): 44-49 ---
    kPolyphony,         // 44  4/6/8/12/16,           default 8
    kVoiceMode,         // 45  Poly/Unison/Mono,      default Poly
    kGlideTime,         // 46  0 .. 2000 ms,          default 0
    kBendRange,         // 47  0 .. 24 semi,          default 2
    kVelToVca,          // 48  0 .. 100 %,            default 40
    kVelToFilter,       // 49  0 .. 100 %,            default 20

    // ---- G8 (stereo dual chain): indices 50-52 -----------------------------
    kStereoMode,        // 50  bool,                  default off
    kStereoDetune,      // 51  0 .. 25 cents,         default 6
    kStereoSpread,      // 52  0 .. 100 %,            default 70

    kNumParams   // == 53 (DESIGN.md §11). Frozen: R4 append-only, and this is
                 // the last index in the FINAL surface.
};
static_assert(kNumParams == PLUG_N_PARAMS, "Parameter count mismatch");

// Factory preset count. Kept in lockstep with PLUG_N_PRESETS (config.h) by
// the static_assert below. G0: config.h already fixes PLUG_N_PRESETS at the
// FINAL count (12), matching the params-surface treatment above, but the
// bank itself (Source/Plugin/nassau_presets.h) and the constructor's
// preset-creation loop are a G9 deliverable (docs/GATES.md) — there is
// nothing meaningful to preset-recall-test before the full param surface (and
// therefore every preset's parameter values) exists. G9 adds both
// nassau_presets.h and the runtime seatbelt assert (NPresets() ==
// kNumPresets) that nassau-zermatt's own constructor uses, once the loop
// that actually creates the 12 presets is in place.
static constexpr int kNumPresets = 12;
static_assert(kNumPresets == PLUG_N_PRESETS, "Preset count mismatch");

class NassauAnaloguePlugin final : public iplug::Plugin {
public:
    explicit NassauAnaloguePlugin(const iplug::InstanceInfo& info);

    void ProcessBlock(iplug::sample** inputs, iplug::sample** outputs, int nFrames) override;
    void OnReset() override;
    void OnParamChange(int paramIdx) override;

    // Versioned, forward-compatible state (lands fully at G9, docs/GATES.md).
    // See nassau_state.h for the chunk layout and the tolerant read rules.
    bool SerializeState(iplug::IByteChunk& chunk) const override;
    int  UnserializeState(const iplug::IByteChunk& chunk, int startPos) override;

private:
    static iplug::Config MakePluginConfig();

    // Number of EParams indices actually registered with a live IParam (via
    // GetParam(i)->InitXxx() in the constructor) so far. G0: just the two
    // below. OnReset() re-pushes exactly this many indices through
    // OnParamChange() -- NOT all of kNumParams, since indices past this are
    // frozen POSITIONS (R4) but have no live IParam yet (see the EParams
    // comment above). Bump this alongside each gate's own InitXxx() calls.
    static constexpr int kNumWiredParams = 2; // kMasterVolume, kOutputClip

    SynthCore mCore;
    static constexpr int kMaxBlockSize = 8192;

    // SynthCore is natively STEREO (DESIGN.md §11 "Channel configuration",
    // PLUG_CHANNEL_IO "0-2") — unlike NassauZermatt there is no mono core and
    // no mono-sum/broadcast wrapper here: ProcessBlock writes directly to the
    // two output channels. Fixed-size (R3: no allocation in the audio path).
    float mOutL[kMaxBlockSize];
    float mOutR[kMaxBlockSize];

    // MIDI handling (ProcessMidiMsg -> an IMidiQueue drained into a fixed-size
    // NoteEvent array each block, DESIGN.md §10.1/§10.2) is a G9 deliverable
    // (docs/GATES.md: "the IMidiQueue drain semantics G9.7 depends on" is one
    // of the four iPlug2 assumptions G9 must verify first, DESIGN.md §0.4 —
    // iplug2's submodule is unchecked-out on this box and could not be read
    // while G0 was written). At G0, SynthCore::process() ignores events
    // entirely regardless (synth_core.h's class comment), so ProcessBlock
    // below simply calls it with events == nullptr.
};
