#pragma once
#include "config.h"
#include "IPlug_include_in_plug_hdr.h"
// IMidiMsg/IMidiQueue (iPlug2's MIDI wire types, used by ProcessMidiMsg/
// ProcessBlock below). UNVERIFIED (DESIGN.md §0.4): the SDK's iplug2
// submodule is unchecked-out on this Linux dev box, so this header's exact
// name could not be confirmed -- "IPlugMidi.h" is the file that defines both
// IMidiMsg and IMidiQueue in the publicly documented iPlug2 tree. If
// IPlug_include_in_plug_hdr.h already pulls this in transitively (likely,
// since iplug::Plugin's own API surface depends on IMidiMsg), this include is
// harmless and redundant; if the filename differs, this is the one line a
// Windows/macOS build needs to correct first -- see this gate's report.
#include "IPlugMidi.h"
#include "synth_core.h"

#if IPLUG_EDITOR
#include "NassauAnalogueUI.h"   // #if IPLUG_EDITOR guarded; not present headless.
#endif

// Parameter surface (DESIGN.md §11, append-only, R4), in its frozen index
// order. G9 (docs/GATES.md) wires every one of these to a live IParam in
// the constructor (see the .cpp) -- G0 only ever wired
// kMasterVolume/kOutputClip, but indices 0-52 have been complete and frozen
// since G0 (see that gate's own note in the .cpp/.h history); G12 appended
// index 53.
//
// APPEND-ONLY PARAM DISCIPLINE (state compatibility): params are serialized
// positionally by enum index (see nassau_state.h). To keep saved states
// forward/backward compatible, params must NEVER be reordered or inserted in
// the middle. A NEW param may only ever be added at the END, immediately
// before kNumParams -- which is exactly how kChorus arrived, and is why a
// patch saved before it existed still recalls correctly.
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

    // ---- G12 (Juno-style output-stage chorus): index 53 --------------------
    // APPENDED, which is the one thing R4's append-only rule permits: every
    // index 0-52 above keeps the meaning it has on disk, so a state chunk or
    // a factory preset saved by a 53-param build loads into this one
    // unchanged, with kChorus simply left at its Off default
    // (nassau_state.h's PlanUnserialize).
    kChorus,            // 53  Off / I / II / I+II,   default Off

    kNumParams   // == 54 (DESIGN.md §11: 53 through G11, + kChorus at G12).
};
static_assert(kNumParams == PLUG_N_PARAMS, "Parameter count mismatch");

// Factory preset count. Kept in lockstep with PLUG_N_PRESETS (config.h) by
// the static_assert below and re-checked at runtime against NPresets() in
// the ctor. The full 12-preset bank is defined once in
// Source/Plugin/nassau_presets.h (single source of truth, also used by
// Tests/preset_tests.cpp, docs/GATES.md G9).
static constexpr int kNumPresets = 12;
static_assert(kNumPresets == PLUG_N_PRESETS, "Preset count mismatch");

class NassauAnaloguePlugin final : public iplug::Plugin {
public:
    explicit NassauAnaloguePlugin(const iplug::InstanceInfo& info);

    void ProcessBlock(iplug::sample** inputs, iplug::sample** outputs, int nFrames) override;
    void OnReset() override;
    void OnParamChange(int paramIdx) override;

    // MIDI in (DESIGN.md §10.1/§10.2, docs/GATES.md G9.7-G9.8): every
    // NoteOn/NoteOff/PolyAftertouch(ignored)/ControlChange/PitchWheel message
    // the host delivers is queued here, offset-preserving, and drained into a
    // NoteEvent array at the top of ProcessBlock. Channel is deliberately
    // IGNORED (omni, G9.8) -- DESIGN.md's NoteEvent has no channel field and
    // this instrument does not do MPE (PLUG_DOES_MPE 0, config.h).
    void ProcessMidiMsg(const iplug::IMidiMsg& msg) override;

    // Versioned, forward-compatible state (nassau_state.h: chunk layout and
    // the tolerant read rules). Design copied verbatim from NassauZermatt's
    // own proven SerializeState/UnserializeState (docs/GATES.md G0's config.h
    // note: "PLUG_DOES_STATE_CHUNKS 0, yet SerializeState IS overridden --
    // copy zermatt's stance verbatim, it is proven").
    bool SerializeState(iplug::IByteChunk& chunk) const override;
    int  UnserializeState(const iplug::IByteChunk& chunk, int startPos) override;

private:
    static iplug::Config MakePluginConfig();

    SynthCore mCore;
    static constexpr int kMaxBlockSize = 8192;

    // SynthCore is natively STEREO (DESIGN.md §11 "Channel configuration",
    // PLUG_CHANNEL_IO "0-2") -- unlike NassauZermatt there is no mono core and
    // no mono-sum/broadcast wrapper here: ProcessBlock writes directly to the
    // two output channels. Fixed-size (R3: no allocation in the audio path).
    float mOutL[kMaxBlockSize];
    float mOutR[kMaxBlockSize];

    // ---- MIDI (DESIGN.md §10.1/§10.2, docs/GATES.md G9.7) -----------------
    // iPlug2's IMidiQueue buffers incoming IMidiMsg's (each carrying its own
    // sample-accurate mOffset within the CURRENT block) between
    // ProcessMidiMsg() calls (which the host/framework invokes for every
    // event, sample-accurately, before ProcessBlock()) and the actual drain
    // in ProcessBlock() below. This preserves G9.7's "sampleOffset ==
    // s" guarantee: the queue is keyed on mOffset, never collapses it to 0.
    iplug::IMidiQueue mMidiQueue;

    // Fixed-size scratch array (R3: no allocation in process()) that
    // ProcessBlock() fills by draining mMidiQueue, then hands to
    // SynthCore::process() as the framework-free NoteEvent list (DESIGN.md
    // §10.1). Sized generously: a MIDI 1.0 stream at typical host block sizes
    // essentially never delivers more events than this in one block (dense
    // controller sweeps aside, which this instrument does not listen to --
    // only note on/off, sustain, all-notes/sound-off and pitch bend cross
    // into NoteEvent at all, see TranslateMidiMsg in the .cpp).
    static constexpr int kMaxEventsPerBlock = 256;
    NoteEvent mEventBuf[kMaxEventsPerBlock];
};
