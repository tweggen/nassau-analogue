#pragma once

#include <atomic>
#include <algorithm>
#include <cstdint>

// G3 (docs/GATES.md): SynthCore now wires a real (minimal, provisional --
// see the kG3Voices comment below) voice section together from the
// primitives G1/G2 already proved: synth_dsp.h's AdsrEnv/Lfo and
// synth_osc.h's Osc/SubOsc/NoiseSource/MixerBlock. Both are R2-legal,
// dependency-free siblings within Source/DSP/ (no SDK/IPlug2 dependency,
// ever) so this include does not touch R2's framework-free guarantee.
#include "synth_dsp.h"
#include "synth_osc.h"

// G4 (docs/GATES.md): SynthCore's public API gains a debug readback for the
// clamped LPF cutoff (getDebugLpfCutoff(), G4.9) but does NOT include
// synth_filter.h or hold a LadderFilter/SvfFilter per voice -- the two
// structures themselves are exercised directly by Tests/filter_tests.cpp
// (matching nassau-zermatt/Tests/cabinet_tests.cpp's precedent for driving a
// DSP structure straight from its test rather than through the full voice
// path). See synth_core.cpp's AUDIO-RATE LOOP comment for why wiring them
// into this voice's real signal path is deferred to G5 (the slope crossfade
// and modulation gate, which touches this exact code path anyway).

/**
 * NoteEvent — the framework-free MIDI-event wire format SynthCore::process()
 * consumes (DESIGN.md §10.1). This is the ENTIRE surface the IPlug2 wrapper's
 * MIDI handling has to produce: everything downstream of it (voice allocation,
 * glide, sustain-pedal state, stereo chain duplication) lives in
 * Source/DSP/ (R14), so it is all testable with the SDK completely absent
 * (DESIGN.md §0.4).
 *
 * G0: the struct is declared and process() accepts it, but nothing in the
 * core consumes it yet — voice allocation is G7 (DESIGN.md §10.3-§10.7).
 */
struct NoteEvent {
    int sampleOffset;   ///< 0 .. numSamples-1, quantised forward to the
                         ///< control-block grid at consumption time (DESIGN.md §10.2).
    enum Type { NoteOn, NoteOff, PitchBend, Sustain, AllNotesOff, AllSoundOff };
    Type  type;
    int   note;          ///< 0..127. Unused by PitchBend/AllNotesOff/AllSoundOff.
    float value;         ///< velocity 0..1 (NoteOn), bend -1..+1 (PitchBend),
                          ///< sustain 0/1 (Sustain). Unused otherwise.
};

/**
 * SynthCore — NassauAnalogue polyphonic analogue-synthesiser DSP core.
 *
 * Since G0 this header declares the COMPLETE FINAL public API (a setter for
 * every one of the 53 params in DESIGN.md §11), even though only a prefix of
 * those params is WIRED into a shipped plugin param at any given gate (params
 * 0-1 as of G0 — see NassauAnaloguePlugin.h). Every setter stores into its
 * atomic (R3); each gate fills in the corresponding math behind this stable
 * header shape. This is exactly the pattern nassau-zermatt/Source/DSP/
 * amp_core.h uses — read its class comment for the precedent.
 *
 * GATE G0 (DESIGN.md §2, §10.1, docs/GATES.md): process() writes EXACT
 * SILENCE to both channels — there is no oscillator, filter, envelope or
 * voice yet (those land in G2-G8). What IS real at G0 is the scaffolding
 * every later gate builds inside:
 *   - the control-rate sub-blocking of DESIGN.md §2 [PERF-1], including the
 *     PERSISTENT `mControlPhase` remainder across process() calls (this is
 *     load-bearing for G3.3's block-size-invariance AC — see process()'s own
 *     comment);
 *   - the per-HOST-BLOCK `ParamSnapshot` of DESIGN.md §2.2 (every atomic
 *     loaded exactly once per process() call, R3's mechanism);
 *   - `NoteEvent` accepted (not yet consumed — G7 is the voice allocator).
 *
 * Design contract (kept stable across gates, DESIGN.md + docs/GATES.md R1-R14):
 *   - Zero framework dependencies: only <atomic>, <algorithm>, plus later
 *     gates' own DSP siblings (synth_dsp.h etc, themselves R2-legal, no
 *     SDK/IPlug2 ever). Builds and unit-tests standalone, with NO SDK,
 *     IPlug2 or framework dependency, ever (R2). This is the only way most
 *     of docs/GATES.md's plan is executable on the Linux dev box at all
 *     (DESIGN.md §0.4).
 *   - Lock-free parameters: every setter stores into a std::atomic; process()
 *     only loads, and only ONCE per host block via buildSnapshot() (R3). No
 *     mutex, no allocation, no I/O in the audio path.
 *   - Block-based, STEREO, float I/O — unlike NassauZermatt (mono core, mono-
 *     sum wrapper), NassauAnalogue is natively stereo-out (DESIGN.md §11
 *     "Channel configuration": PLUG_CHANNEL_IO "0-2", no audio input at all,
 *     it is an instrument).
 *   - Coefficient math and filter state will use double once they exist;
 *     public I/O stays float.
 *   - No -ffast-math (R7): the ZDF ladder (G4) and every IIR recursion in
 *     DESIGN.md §5 will be unsafe under it.
 *   - Append-only parameter discipline (R4): state is serialized positionally
 *     by whichever gate lands Source/Plugin/nassau_state.h (G9). The 53
 *     setters below are in DESIGN.md §11's FINAL, FROZEN index order.
 *   - Create once, never copy/move.
 */
class SynthCore {
public:
    // ===== Enum-valued parameters (DESIGN.md §11) =====

    /// kOsc1Wave (param 2) / kOsc2Wave (param 7). Default Saw for both.
    enum class Wave : int { Saw = 0, Pulse = 1, Tri = 2 };

    /// kOsc1Octave (param 3) / kOsc2Octave (param 8). Default 8' for both.
    /// Ft16/Ft8/Ft4/Ft2 spell out the footage naming (DESIGN.md §3.2) since a
    /// C++ identifier cannot start with a digit.
    enum class Octave : int { Ft16 = 0, Ft8 = 1, Ft4 = 2, Ft2 = 3 };

    /// kSubOctave (param 15). Default Minus1 (-1 octave from VCO 1).
    enum class SubOctave : int { Minus1 = 0, Minus2 = 1 };

    /// kNoiseColor (param 17). Default White.
    enum class NoiseColor : int { White = 0, Pink = 1 };

    /// kLfoWave (param 27). Default Tri.
    enum class LfoWave : int { Tri = 0, Saw = 1, Ramp = 2, Square = 3, SampleHold = 4 };

    /// kLpfSlope (param 32). Default Db24 — DESIGN.md §5.1: 24 dB is a 4-pole
    /// ZDF ladder, 12 dB is a *different structure* (2-pole TPT SVF), not a tap.
    enum class LpfSlope : int { Db24 = 0, Db12 = 1 };

    /// kHpfSlope (param 39). Default Db12 (DESIGN.md §11 lists "12 / 24 dB"
    /// for this param, the opposite listed order from kLpfSlope above — kept
    /// as documented, not "fixed" to match).
    enum class HpfSlope : int { Db12 = 0, Db24 = 1 };

    /// kPolyphony (param 44). Default Eight.
    enum class Polyphony : int { Four = 0, Six = 1, Eight = 2, Twelve = 3, Sixteen = 4 };

    /// kVoiceMode (param 45). Default Poly.
    enum class VoiceMode : int { Poly = 0, Unison = 1, Mono = 2 };

    /// Total parameter count (DESIGN.md §11: "53 params, append-only", R4
    /// frozen). Kept here, not just in Source/Plugin/, so a DSP-only test
    /// binary (no SDK, R2) has something to check its setter count against.
    static constexpr int kNumParams = 53;

    // ===== Lifecycle =====
    SynthCore();
    ~SynthCore();

    SynthCore(const SynthCore&) = delete;
    SynthCore& operator=(const SynthCore&) = delete;
    SynthCore(SynthCore&&) = delete;
    SynthCore& operator=(SynthCore&&) = delete;

    /// Initialize DSP state for a sample rate. Accepts 44100 / 48000 / 88200 /
    /// 96000 / 192000 (G0.6) and, more generally, anything in [8000, 192000];
    /// bogus rates are ignored, leaving the sample rate at its last valid
    /// value (mirrors nassau-zermatt's AmpCore::init()). Resets all internal
    /// state (equivalent to reset()) without touching parameters.
    void init(float sampleRate);

    /// Clear all internal DSP state (oscillator phase, filter state, envelope
    /// state, noise-generator seeds, the voice allocator, the control-rate
    /// grid position, SmoothedValues snapped to target, LFO phase) WITHOUT
    /// touching any parameter (DESIGN.md §11 "Reset semantics", G0.7). At G0
    /// there is no voice/filter/envelope state to clear yet, so this only
    /// resets the control-rate grid position — landed now so every later
    /// gate's OnReset()-equivalent call has a stable target.
    void reset();

    // ===== Audio processing =====

    /// Process one host block. `events` is a framework-free MIDI event list
    /// (DESIGN.md §10.1), `outL`/`outR` are the two output channels (this
    /// core is natively stereo-out, DESIGN.md §11 "Channel configuration" —
    /// there is no audio input at all, this is an instrument).
    ///
    /// GATE G0: writes EXACT SILENCE (`== 0.0f`) to both channels for every
    /// sample, regardless of `events`/params — there is no voice, oscillator,
    /// filter or VCA yet (G2-G8 land the real signal path). `events` is
    /// accepted but not yet consumed (voice allocation is G7).
    ///
    /// What IS real here: the control-rate sub-blocking of DESIGN.md §2
    /// [PERF-1]. `mControlPhase` is a PERSISTENT member — NOT reset at the
    /// top of this call — so the control-block grid is anchored to the
    /// sample STREAM, not to host block boundaries. A host block that ends
    /// mid-control-block leaves the remainder to be completed by the next
    /// call. This is what docs/GATES.md G3.3 (block-size invariance) depends
    /// on: if the grid restarted every call, a host delivering 1-sample
    /// blocks would update modulation every sample and a host delivering
    /// 33-sample blocks would produce an uneven grid, so identical MIDI would
    /// render differently under different buffer sizes. The per-control-block
    /// update point (ADSR/LFO advance, pitch/cutoff/VCA-gain recompute) is
    /// marked in the .cpp; it is empty at G0 because there is nothing to
    /// update yet.
    ///
    /// Also real: the per-HOST-BLOCK `ParamSnapshot` of DESIGN.md §2.2 —
    /// every one of the 53 atomics is loaded exactly once via
    /// buildSnapshot(), which is what keeps the (future) audio-rate loop at
    /// zero atomic loads (R3, R12). At G0 the snapshot has nothing to feed
    /// yet and is discarded.
    void process(const NoteEvent* events, int numEvents,
                 float* outL, float* outR, int numSamples);

    // ===== Parameters (DESIGN.md §11, indices 0..52 of the FINAL surface) ===
    // Every setter stores into a std::atomic (R3) and is safe to call from a
    // non-audio thread concurrently with process() (G0.9). Only params 0 and
    // 1 are wired to a shipped plugin param at G0 (NassauAnaloguePlugin.h);
    // the rest exist here so the header does not change shape in later gates.
    // Every default below is [voicing] (DESIGN.md §0.3): it is the chosen
    // starting position of a knob, not a derived quantity — see DESIGN.md
    // §11's table, transcribed verbatim.

    /// 0  kMasterVolume   -60 .. +12 dB, default -6. G0.
    void setMasterVolumeDb(float db);
    /// 1  kOutputClip     bool, default on. G0.
    void setOutputClip(bool enabled);
    /// 2  kOsc1Wave       Saw/Pulse/Tri, default Saw. G2.
    void setOsc1Wave(Wave w);
    /// 3  kOsc1Octave     16'/8'/4'/2', default 8'. G2.
    void setOsc1Octave(Octave o);
    /// 4  kOsc1Fine       -50 .. +50 cents, default 0. G2.
    void setOsc1FineCents(float cents);
    /// 5  kOsc1PW         5 .. 95 %, default 50. G2.
    void setOsc1PwPercent(float pct);
    /// 6  kOsc1Level      0 .. 100 %, default 100. G2.
    void setOsc1LevelPercent(float pct);
    /// 7  kOsc2Wave       Saw/Pulse/Tri, default Saw. G2.
    void setOsc2Wave(Wave w);
    /// 8  kOsc2Octave     16'/8'/4'/2', default 8'. G2.
    void setOsc2Octave(Octave o);
    /// 9  kOsc2Semi       -12 .. +12 semitones, default 0. G2.
    void setOsc2Semi(int semitones);
    /// 10 kOsc2Fine       -50 .. +50 cents, default -7. G2.
    void setOsc2FineCents(float cents);
    /// 11 kOsc2PW         5 .. 95 %, default 50. G2.
    void setOsc2PwPercent(float pct);
    /// 12 kOsc2Level      0 .. 100 %, default 80. G2.
    void setOsc2LevelPercent(float pct);
    /// 13 kOsc2Sync       bool, default off. G2.
    void setOsc2Sync(bool enabled);
    /// 14 kOsc2KeyTrack   bool, default on. G2.
    void setOsc2KeyTrack(bool enabled);
    /// 15 kSubOctave      -1 / -2, default -1. G2.
    void setSubOctave(SubOctave o);
    /// 16 kSubLevel       0 .. 100 %, default 0. G2.
    void setSubLevelPercent(float pct);
    /// 17 kNoiseColor     White/Pink, default White. G2.
    void setNoiseColor(NoiseColor c);
    /// 18 kNoiseLevel     0 .. 100 %, default 0. G2.
    void setNoiseLevelPercent(float pct);
    /// 19 kEnvFAttack     1 .. 10000 ms, default 2. G3.
    void setEnvFAttackMs(float ms);
    /// 20 kEnvFDecay      1 .. 10000 ms, default 400. G3.
    void setEnvFDecayMs(float ms);
    /// 21 kEnvFSustain    0 .. 100 %, default 30. G3.
    void setEnvFSustainPercent(float pct);
    /// 22 kEnvFRelease    1 .. 10000 ms, default 300. G3.
    void setEnvFReleaseMs(float ms);
    /// 23 kEnvAAttack     1 .. 10000 ms, default 2. G3.
    void setEnvAAttackMs(float ms);
    /// 24 kEnvADecay      1 .. 10000 ms, default 800. G3.
    void setEnvADecayMs(float ms);
    /// 25 kEnvASustain    0 .. 100 %, default 80. G3.
    void setEnvASustainPercent(float pct);
    /// 26 kEnvARelease    1 .. 10000 ms, default 250. G3.
    void setEnvAReleaseMs(float ms);
    /// 27 kLfoWave        Tri/Saw/Ramp/Sqr/S&H, default Tri. G3.
    void setLfoWave(LfoWave w);
    /// 28 kLfoRate        0.05 .. 30 Hz, default 5. G3.
    void setLfoRateHz(float hz);
    /// 29 kLfoDelay       0 .. 3000 ms, default 0. G3.
    void setLfoDelayMs(float ms);
    /// 30 kLfoPitchAmount 0 .. 100 %, default 0. G3.
    void setLfoPitchAmountPercent(float pct);
    /// 31 kLfoPwmAmount   0 .. 100 %, default 0. G3.
    void setLfoPwmAmountPercent(float pct);
    /// 32 kLpfSlope       24 / 12 dB, default 24. G4.
    void setLpfSlope(LpfSlope s);
    /// 33 kLpfCutoff      20 .. 18000 Hz, default 2000. G4.
    void setLpfCutoffHz(float hz);
    /// 34 kLpfResonance   0 .. 100 %, default 20. G4.
    void setLpfResonancePercent(float pct);
    /// 35 kLpfEnvAmount   -100 .. +100 % (bipolar), default 40. G5.
    void setLpfEnvAmountPercent(float pct);
    /// 36 kLpfKeyFollow   0 .. 100 %, default 50. G5.
    void setLpfKeyFollowPercent(float pct);
    /// 37 kLpfLfoAmount   0 .. 100 %, default 0. G5.
    void setLpfLfoAmountPercent(float pct);
    /// 38 kDrive          0 .. 100 %, default 15. G6.
    void setDrivePercent(float pct);
    /// 39 kHpfSlope       12 / 24 dB, default 12. G6.
    void setHpfSlope(HpfSlope s);
    /// 40 kHpfCutoff      20 .. 2000 Hz, default 20 (= hard bypass, DESIGN.md
    ///    §5.5). G6.
    void setHpfCutoffHz(float hz);
    /// 41 kHpfKeyFollow   0 .. 100 %, default 0. G6.
    void setHpfKeyFollowPercent(float pct);
    /// 42 kPmEnvFToOsc2   -100 .. +100 % (bipolar), default 0. G6.
    void setPmEnvFToOsc2Percent(float pct);
    /// 43 kPmEnvFToPw     -100 .. +100 % (bipolar), default 0. G6.
    void setPmEnvFToPwPercent(float pct);
    /// 44 kPolyphony      4/6/8/12/16, default 8. G7.
    void setPolyphony(Polyphony p);
    /// 45 kVoiceMode      Poly/Unison/Mono, default Poly. G7.
    void setVoiceMode(VoiceMode m);
    /// 46 kGlideTime      0 .. 2000 ms, default 0. G7.
    void setGlideTimeMs(float ms);
    /// 47 kBendRange      0 .. 24 semitones, default 2. G7.
    void setBendRangeSemitones(int semitones);
    /// 48 kVelToVca       0 .. 100 %, default 40. G7.
    void setVelToVcaPercent(float pct);
    /// 49 kVelToFilter    0 .. 100 %, default 20. G7.
    void setVelToFilterPercent(float pct);
    /// 50 kStereoMode     bool, default off. G8.
    void setStereoMode(bool enabled);
    /// 51 kStereoDetune   0 .. 25 cents, default 6. G8.
    void setStereoDetuneCents(float cents);
    /// 52 kStereoSpread   0 .. 100 %, default 70. G8.
    void setStereoSpreadPercent(float pct);

    // ===== Utility =====

    /// Query the sample rate set by the last valid init() call.
    float getSampleRate() const { return mSampleRate.load(std::memory_order_relaxed); }

    // ===== Test-only debug accessors (DESIGN.md §2) =====
    // Only the control-rate grid's own two pieces of state are exposed here
    // at G0. Per-voice/per-filter debug accessors that docs/GATES.md's later
    // gates mention (getDebugLpfCutoff, getDebugVoiceState,
    // getDebugActiveVoiceCount, getDebugEnvF/A, ...) are deliberately NOT
    // pre-declared: their correct shape depends on the voice-allocator/filter
    // design those gates (G4/G7/G8) haven't made yet, and nassau-zermatt's own
    // precedent is to add each debug accessor at the gate that introduces the
    // state it exposes (e.g. getDebugLoopGain arrived at G4, not G0) rather
    // than freezing a shape ahead of the design that owns it. See the G0 gate
    // report for the full reasoning.

    /// mControlBlock is a RUNTIME member (default 32), not a compile-time
    /// constant, purely so Tests/ can set it to 1 and measure the artifact the
    /// choice of 32 introduces (DESIGN.md §2, docs/GATES.md G3.9). Test-only:
    /// not safe to call concurrently with process() (unlike the real
    /// parameter setters above, which ARE process()-concurrency-safe, R3/G0.9)
    /// — call it before processing starts. Clamped to >= 1, and resets the
    /// grid position to 0 so the two stay consistent.
    void setDebugControlBlock(int samples);
    int getDebugControlBlock() const { return mControlBlock; }

    /// Current position within the control-block grid, i.e. how many samples
    /// into the current (incomplete) control block the stream has advanced —
    /// always in [0, mControlBlock). Test-only: this is exactly the
    /// persistent remainder DESIGN.md §2 and this class's process() doc
    /// comment describe; exposed so Tests/ can verify it survives a host
    /// block boundary and is cleared by reset() without having to infer it
    /// from audible output (which, before G3, does not yet depend on it).
    int getDebugControlPhase() const { return mControlPhase; }

    // ===== Test-only debug accessors (G3, docs/GATES.md) =====

    /// G3.1: exactly one ParamSnapshot built per host block, every one of its
    /// 53 atomics loaded exactly once. Incremented once per atomic .load()
    /// INSIDE buildSnapshot() only (not mSampleRate, not mControlPhase --
    /// those are not part of the "53 params" DESIGN.md §11 table this AC
    /// counts, see buildSnapshot()'s own comment). Test-only: reset before
    /// measuring, read after.
    long long getDebugAtomicLoadCount() const { return mDebugAtomicLoadCount; }
    void resetDebugAtomicLoadCount() { mDebugAtomicLoadCount = 0; }

    /// G3.5: force every voice's VCA gain to a constant 1.0 (bypassing ENV-A
    /// entirely), so a render with this set is "a separately-rendered
    /// unity-VCA render of the same note" -- dividing a normal render by one
    /// of these demodulates the gain envelope back out of the raw (saw-wrap-
    /// dominated) waveform. Test-only, not safe concurrently with process().
    void setDebugForceUnityVca(bool enabled) { mDebugForceUnityVca = enabled; }

    /// G3.5: skip the per-sample linear interpolation of vcaGain and just
    /// hold vcaGainEnd for the whole control block -- "a reference build
    /// with the interpolation disabled", built from the SAME snapshot/
    /// envelope math as the real path so the comparison isolates exactly the
    /// interpolation, nothing else. Test-only, not safe concurrently with
    /// process().
    void setDebugDisableVcaInterpolation(bool enabled) { mDebugDisableVcaInterpolation = enabled; }

    /// G3.10/G3.7: the LFO's raw output (post depth-gate, [-1,1]) as of the
    /// most recent control-rate step -- the SAME single value every active
    /// voice's pitch/PW recompute reads that control block (DESIGN.md §7
    /// [PERF-4]: one global LFO, not one per voice).
    double getDebugLfoValue() const { return mLastLfoValue; }

    /// G3.11: the Lfo's own internal delay/ramp-in depth multiplier (0 at
    /// note-on with a pending delay, ramping linearly to 1 -- DESIGN.md §7).
    /// Exposed directly so G3.11 can assert "does not reset under a held
    /// chord" exactly, rather than trying to infer it from audio alone.
    double getDebugLfoDepthGain() const { return mLfo.depthGain; }

    /// G3.10: per-voice-slot readback of the shared LFO->pitch modulation
    /// term (semitones) applied to that slot's oscillators as of the most
    /// recent control-rate step it was active for -- 0 for a slot that is
    /// not active. Mirrors the getDebugEnvF()/getDebugEnvA() "exactly one
    /// value per voice, not one per chain" pattern DESIGN.md/docs/GATES.md
    /// already uses for the analogous G8.5 shared-modulation AC.
    double getDebugVoiceLfoPitchModSemis(int voiceIndex) const {
        if (voiceIndex < 0 || voiceIndex >= kG3Voices) return 0.0;
        return mVoices[voiceIndex].debugLfoPitchModSemis;
    }

    /// G3.10 (and generally useful for driving deterministic multi-voice
    /// tests): true iff voice slot `voiceIndex` currently has a sounding
    /// note (Attack/Decay/Sustain/Release, not Idle).
    bool getDebugVoiceActive(int voiceIndex) const {
        if (voiceIndex < 0 || voiceIndex >= kG3Voices) return false;
        return mVoices[voiceIndex].active;
    }

    /// Number of voice slots this GATE's minimal, provisional multi-voice
    /// path provides (see kG3Voices's own comment below) -- NOT DESIGN.md
    /// §10.3's kMaxVoices=16 (+2 fade slots), which is G7's job.
    static int getDebugNumVoiceSlotsG3() { return kG3Voices; }

    /// G3.6: ENV-F has no audible destination until G4/G5 (it does not drive
    /// the filter yet), so its own zipper-freedom cannot be measured from
    /// OUTPUT AUDIO the way ENV-A's can (R6) -- there is nothing for it to
    /// modulate yet. These two expose the raw envelope value (`y`, DESIGN.md
    /// §6) directly per voice slot instead, at CONTROL rate, which is the
    /// same underlying quantity that will reach audio once a filter exists
    /// to read it. Also anticipates G8.5's "getDebugEnvF()/getDebugEnvA()
    /// per voice" pattern.
    double getDebugEnvFValue(int voiceIndex) const {
        if (voiceIndex < 0 || voiceIndex >= kG3Voices) return 0.0;
        return mVoices[voiceIndex].envF.y;
    }
    double getDebugEnvAValue(int voiceIndex) const {
        if (voiceIndex < 0 || voiceIndex >= kG3Voices) return 0.0;
        return mVoices[voiceIndex].envA.y;
    }

    // ===== Test-only debug accessors (G4, docs/GATES.md) =====

    /// G4.9: the ACTUAL cutoff (Hz) fed to both filter structures'
    /// setControlRate() as of the most recent control-rate update -- i.e.
    /// `kLpfCutoff` after DESIGN.md §5.4's `[10, 0.45*fs]` clamp (G4's slice
    /// of that clamp: no modulation term exists yet, that is G5's). Updated
    /// once per control block regardless of whether any voice is active
    /// (DESIGN.md §2: cutoff is not per-voice at G4 -- see
    /// controlRateUpdate()'s own comment).
    double getDebugLpfCutoff() const { return mDebugLpfCutoffHz; }

private:
    // ===== Per-host-block parameter snapshot (DESIGN.md §2.2) =====
    // Built exactly once per process() call: every atomic is loaded exactly
    // once here (R3's mechanism — the audio-rate loop performs zero atomic
    // loads). At G0 nothing consumes these fields yet (no voice, filter or
    // envelope exists); G2-G8 progressively add the parameter-derived
    // constants (ADSR coefficients, LFO increment, drive knee, mixer gains,
    // resonance k, base cutoffs, ...) DESIGN.md §2.2 describes, but the RAW
    // per-param fields below are already final in shape and correspond
    // 1:1 with the 53 setters above.
    struct ParamSnapshot {
        float masterVolumeDb;
        bool  outputClip;
        int   osc1Wave;
        int   osc1Octave;
        float osc1FineCents;
        float osc1PwPercent;
        float osc1LevelPercent;
        int   osc2Wave;
        int   osc2Octave;
        int   osc2Semi;
        float osc2FineCents;
        float osc2PwPercent;
        float osc2LevelPercent;
        bool  osc2Sync;
        bool  osc2KeyTrack;
        int   subOctave;
        float subLevelPercent;
        int   noiseColor;
        float noiseLevelPercent;
        float envFAttackMs;
        float envFDecayMs;
        float envFSustainPercent;
        float envFReleaseMs;
        float envAAttackMs;
        float envADecayMs;
        float envASustainPercent;
        float envAReleaseMs;
        int   lfoWave;
        float lfoRateHz;
        float lfoDelayMs;
        float lfoPitchAmountPercent;
        float lfoPwmAmountPercent;
        int   lpfSlope;
        float lpfCutoffHz;
        float lpfResonancePercent;
        float lpfEnvAmountPercent;
        float lpfKeyFollowPercent;
        float lpfLfoAmountPercent;
        float drivePercent;
        int   hpfSlope;
        float hpfCutoffHz;
        float hpfKeyFollowPercent;
        float pmEnvFToOsc2Percent;
        float pmEnvFToPwPercent;
        int   polyphony;
        int   voiceMode;
        float glideTimeMs;
        int   bendRangeSemitones;
        float velToVcaPercent;
        float velToFilterPercent;
        bool  stereoMode;
        float stereoDetuneCents;
        float stereoSpreadPercent;

        // ---- G3: derived, BLOCK-RATE constants (DESIGN.md §2.2) ----
        // NOT loaded from atomics -- filled in by finishSnapshot() AFTER
        // buildSnapshot() returns, from the raw fields above plus fs and
        // mControlBlock (each read exactly once elsewhere in process(), see
        // its own comment). Kept OUT of buildSnapshot() specifically so they
        // do not count against G3.1's "exactly 53 atomic loads" instrumented
        // figure -- these three (fsControl, the 6 ADSR coefficients x2, the
        // LFO increment) involve std::log/std::exp (AdsrEnv::coeffForMs),
        // which is fine here (block rate, R12) but must never run per
        // control block or per sample.
        double fsControl = 1.0;
        double envFAttackCoeff = 0.0, envFDecayCoeff = 0.0, envFReleaseCoeff = 0.0, envFSustainLevel = 0.0;
        double envAAttackCoeff = 0.0, envADecayCoeff = 0.0, envAReleaseCoeff = 0.0, envASustainLevel = 0.0;
        double lfoIncrement = 0.0;    ///< cycles/control-step, DESIGN.md §7
        double lfoDelaySeconds = 0.0;
    };

    /// Loads every one of the 53 atomics exactly once (relaxed ordering — a
    /// single scalar with no ordering dependency on other memory, R3/G0.9,
    /// same convention as nassau-zermatt's AmpCore). No clamping is applied
    /// here at G0: clamping arrives with each param's owning gate, alongside
    /// the derived math that actually needs the clamped value. G3: every
    /// `.load()` call in the .cpp implementation goes through a small
    /// counting wrapper feeding mDebugAtomicLoadCount (G3.1) -- see the .cpp.
    ParamSnapshot buildSnapshot() const;

    /// G3 (DESIGN.md §2.2): fills in ParamSnapshot's block-rate derived
    /// fields (ADSR coefficients, LFO increment/delay) from the raw fields
    /// buildSnapshot() already loaded, plus `fsControl` (fs/mControlBlock,
    /// itself computed once per host block in process()). No atomic loads
    /// here (fsControl is passed in, not re-read) — see the field comment
    /// above for why this is a SEPARATE function from buildSnapshot().
    static void finishSnapshot(ParamSnapshot& s, double fsControl);

    // ===== G3: minimal, provisional per-voice state =====
    // Osc/SubOsc/NoiseSource (synth_osc.h, G2) + AdsrEnv x2 (synth_dsp.h, G1)
    // per voice, driven by the control-rate loop in process().
    struct Voice {
        Osc osc1, osc2;
        SubOsc sub;
        NoiseSource noise;
        /// Per-voice DC blocker, applied to the MIXER OUTPUT before anything
        /// downstream (DESIGN.md §4). A pulse of duty d carries a DC offset of
        /// exactly (2d - 1) by construction -- measured -0.50 at PW=25% and
        /// -0.80 at PW=10% -- so PWM, one of this instrument's core sounds,
        /// emits large DC into the drive stage and the resonant filter unless
        /// it is coupled out. Real hardware does this with a capacitor.
        OnePoleHP dcBlock;
        AdsrEnv envF, envA;    ///< ENV-F (filter, not yet routed anywhere -- G5) / ENV-A (VCA).
                               ///< The LPF itself (G4: LadderFilter/SvfFilter,
                               ///< Source/DSP/synth_filter.h) is not yet a per-voice member --
                               ///< see synth_core.h's top-of-file G4 note and the AUDIO-RATE
                               ///< LOOP's own comment in synth_core.cpp.
        bool active = false;   ///< sounding (Attack/Decay/Sustain/Release), not Idle
        int note = -1;
        double vcaGainStart = 0.0;   ///< interpolation endpoints for THIS control block
        double vcaGainEnd = 0.0;     ///< (DESIGN.md §2: vcaGain is one of the 3 interpolated scalars)
        double debugLfoPitchModSemis = 0.0;  ///< test-only readback, G3.10
    };

    /// Minimal, PROVISIONAL voice count for this gate only. DESIGN.md §10.3's
    /// real allocator (kMaxVoices=16 + 2 fade-out slots, Idle/oldest-Released/
    /// oldest-Playing allocation order, click-free stealing, sustain pedal,
    /// unison/mono voice modes, kPolyphony honoured) is G7's job (docs/
    /// GATES.md), not G3's -- G3's job is ENV-F/ENV-A/LFO/control-rate wiring.
    /// 8 fixed slots with a trivial "reuse same note, else first idle, else
    /// slot 0" policy is enough to prove ENV-A genuinely drives the VCA and
    /// that the LFO is genuinely global/shared (G3.10) without building G7's
    /// machinery early (scope discipline, docs/GATES.md's own "G3 only" note
    /// for this gate). [voicing] chosen only as "comfortably more than the
    /// 1-2 voices any G3 AC exercises simultaneously".
    static constexpr int kG3Voices = 8;
    Voice mVoices[kG3Voices];
    int mActiveVoiceCount = 0;   ///< DESIGN.md §7: LFO delay retriggers only on 0->1 of this
    Lfo mLfo;                    ///< DESIGN.md §7 [PERF-4]: ONE global LFO, stepped once per
                                  ///< control block, read by every voice -- never per-voice.
    double mLastLfoValue = 0.0;  ///< test-only readback of the most recent mLfo.step() result (G3.10)
    double mDebugLpfCutoffHz = 0.0;  ///< G4.9: last clamped LPF cutoff fed to setControlRate()

    static constexpr uint32_t kLfoShSeed = 0x5EED1234u;  // [voicing] fixed S&H seed, R8/R13

    /// Applies a single note-scoped event to the voice array / mLfo (NoteOn/
    /// NoteOff/AllNotesOff/AllSoundOff). PitchBend/Sustain are no-ops at G3
    /// (G7's job, DESIGN.md §10.3/§10.6/§7.6) -- accepted (not touched) so
    /// they do not crash, matching G0.8's existing "every event type, no
    /// crash" coverage.
    void applyEvent(const NoteEvent& ev);

    /// The DESIGN.md §2 "control-rate update point": steps both envelopes
    /// and the LFO one control step, and recomputes each active voice's
    /// pitch (dt) and pulse width from `snapshot` + the freshly-stepped LFO
    /// value. Called once per control block boundary crossed by process().
    void controlRateUpdate(const ParamSnapshot& snapshot, double fs);

    /// [ref] DESIGN.md §3.2: 16'/8'/4'/2' -> 220/440/880/1760 Hz at note 69,
    /// i.e. -12/0/+12/+24 semitones relative to 8'.
    static double octaveOffsetSemis(Octave o);

    // ===== G3: pending-event queue (DESIGN.md §10.2) =====
    // An event whose target control-block boundary is not reached within the
    // process() call it arrived in (a small host block size can make this
    // common, e.g. a 1-sample-block host against a 32-sample control block)
    // must still take effect at that boundary once a LATER process() call
    // finally reaches it -- but that event's OWN `events` array is only
    // valid for the call it arrived in (DESIGN.md §10.1). So any such
    // trailing, not-yet-applied events are copied into this small FIXED
    // (R3: no allocation) queue at the end of process(), and drained (in
    // order, before that call's own events) at the very first control-block
    // boundary a subsequent call reaches. This is what makes G3.3's block-
    // size invariance hold for EVENT timing, not just for modulation.
    static constexpr int kMaxPendingEvents = 256;  // [voicing] generous fixed bound, see .cpp
    NoteEvent mPendingEvents[kMaxPendingEvents];
    int mPendingCount = 0;

    // ===== G3: test-only instrumentation / debug hooks =====
    mutable long long mDebugAtomicLoadCount = 0;  ///< G3.1, see buildSnapshot()'s .cpp body
    bool mDebugForceUnityVca = false;             ///< G3.5, see the public setter's comment
    bool mDebugDisableVcaInterpolation = false;   ///< G3.5, see the public setter's comment

    // ===== Configuration =====
    std::atomic<float> mSampleRate{44100.0f};

    // ===== Control-rate grid (DESIGN.md §2 [PERF-1]) =====
    // Deliberately plain int members, not atomics: both are touched only from
    // the audio thread (process()) and from the test-only
    // setDebugControlBlock() helper, which is documented as not safe to call
    // concurrently with process() (unlike every real parameter setter below).
    int mControlBlock = 32;  ///< [dsp] DESIGN.md §2: 32-sample control rate. Runtime, not
                              ///< constexpr, so Tests/ can override it (G3.9).
    int mControlPhase = 0;   ///< Persists across process() calls (DESIGN.md §2) —
                              ///< load-bearing for G3.3's block-size invariance.

    // ===== Parameters (atomic for lock-free access, R3) =====
    // Numeric knobs: atomic<float>. Discrete integer counts (semitones):
    // atomic<int>. Enums: atomic<int> (holding the underlying enum's int
    // value). Booleans: atomic<float>, stored as 0.0f/1.0f — matches the
    // nassau-zermatt/nassau-eq house convention (see amp_core.h) so
    // buildSnapshot() never has to special-case a bool atomic's memory model.
    std::atomic<float> mMasterVolumeDb{-6.0f};                                  // [voicing]
    std::atomic<float> mOutputClip{1.0f};                                       // [voicing] on
    std::atomic<int>   mOsc1Wave{static_cast<int>(Wave::Saw)};                  // [voicing]
    std::atomic<int>   mOsc1Octave{static_cast<int>(Octave::Ft8)};              // [voicing]
    std::atomic<float> mOsc1FineCents{0.0f};                                    // [voicing]
    std::atomic<float> mOsc1PwPercent{50.0f};                                   // [voicing]
    std::atomic<float> mOsc1LevelPercent{100.0f};                               // [voicing]
    std::atomic<int>   mOsc2Wave{static_cast<int>(Wave::Saw)};                  // [voicing]
    std::atomic<int>   mOsc2Octave{static_cast<int>(Octave::Ft8)};              // [voicing]
    std::atomic<int>   mOsc2Semi{0};                                            // [voicing]
    std::atomic<float> mOsc2FineCents{-7.0f};                                   // [voicing]
    std::atomic<float> mOsc2PwPercent{50.0f};                                   // [voicing]
    std::atomic<float> mOsc2LevelPercent{80.0f};                                // [voicing]
    std::atomic<float> mOsc2Sync{0.0f};                                         // [voicing] off
    std::atomic<float> mOsc2KeyTrack{1.0f};                                     // [voicing] on
    std::atomic<int>   mSubOctave{static_cast<int>(SubOctave::Minus1)};         // [voicing]
    std::atomic<float> mSubLevelPercent{0.0f};                                  // [voicing]
    std::atomic<int>   mNoiseColor{static_cast<int>(NoiseColor::White)};        // [voicing]
    std::atomic<float> mNoiseLevelPercent{0.0f};                                // [voicing]
    std::atomic<float> mEnvFAttackMs{2.0f};                                     // [voicing]
    std::atomic<float> mEnvFDecayMs{400.0f};                                    // [voicing]
    std::atomic<float> mEnvFSustainPercent{30.0f};                              // [voicing]
    std::atomic<float> mEnvFReleaseMs{300.0f};                                  // [voicing]
    std::atomic<float> mEnvAAttackMs{2.0f};                                     // [voicing]
    std::atomic<float> mEnvADecayMs{800.0f};                                    // [voicing]
    std::atomic<float> mEnvASustainPercent{80.0f};                              // [voicing]
    std::atomic<float> mEnvAReleaseMs{250.0f};                                  // [voicing]
    std::atomic<int>   mLfoWave{static_cast<int>(LfoWave::Tri)};                // [voicing]
    std::atomic<float> mLfoRateHz{5.0f};                                        // [voicing]
    std::atomic<float> mLfoDelayMs{0.0f};                                       // [voicing]
    std::atomic<float> mLfoPitchAmountPercent{0.0f};                            // [voicing]
    std::atomic<float> mLfoPwmAmountPercent{0.0f};                              // [voicing]
    std::atomic<int>   mLpfSlope{static_cast<int>(LpfSlope::Db24)};             // [voicing]
    std::atomic<float> mLpfCutoffHz{2000.0f};                                   // [voicing]
    std::atomic<float> mLpfResonancePercent{20.0f};                             // [voicing]
    std::atomic<float> mLpfEnvAmountPercent{40.0f};                             // [voicing]
    std::atomic<float> mLpfKeyFollowPercent{50.0f};                             // [voicing]
    std::atomic<float> mLpfLfoAmountPercent{0.0f};                              // [voicing]
    std::atomic<float> mDrivePercent{15.0f};                                    // [voicing]
    std::atomic<int>   mHpfSlope{static_cast<int>(HpfSlope::Db12)};             // [voicing]
    std::atomic<float> mHpfCutoffHz{20.0f};                                     // [voicing] = bypass, DESIGN.md §5.5
    std::atomic<float> mHpfKeyFollowPercent{0.0f};                              // [voicing]
    std::atomic<float> mPmEnvFToOsc2Percent{0.0f};                              // [voicing]
    std::atomic<float> mPmEnvFToPwPercent{0.0f};                                // [voicing]
    std::atomic<int>   mPolyphony{static_cast<int>(Polyphony::Eight)};          // [voicing]
    std::atomic<int>   mVoiceMode{static_cast<int>(VoiceMode::Poly)};           // [voicing]
    std::atomic<float> mGlideTimeMs{0.0f};                                      // [voicing]
    std::atomic<int>   mBendRangeSemitones{2};                                  // [voicing]
    std::atomic<float> mVelToVcaPercent{40.0f};                                 // [voicing]
    std::atomic<float> mVelToFilterPercent{20.0f};                              // [voicing]
    std::atomic<float> mStereoMode{0.0f};                                       // [voicing] off
    std::atomic<float> mStereoDetuneCents{6.0f};                                // [voicing]
    std::atomic<float> mStereoSpreadPercent{70.0f};                             // [voicing]
};
