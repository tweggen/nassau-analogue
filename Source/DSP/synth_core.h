#pragma once

#include <atomic>
#include <algorithm>
#include <cstdint>

// G3 (docs/GATES.md): SynthCore now wires a real (minimal, provisional --
// see the kMaxVoices comment below) voice section together from the
// primitives G1/G2 already proved: synth_dsp.h's AdsrEnv/Lfo and
// synth_osc.h's Osc/SubOsc/NoiseSource/MixerBlock. Both are R2-legal,
// dependency-free siblings within Source/DSP/ (no SDK/IPlug2 dependency,
// ever) so this include does not touch R2's framework-free guarantee.
#include "synth_dsp.h"
#include "synth_osc.h"
#include "synth_filter.h"
#include "synth_alloc.h"
#include "synth_simd.h"

// G5 (docs/GATES.md): the LPF is now GENUINELY WIRED into the per-voice
// audio path (mixer -> DC block -> LPF -> VCA, DESIGN.md §1) -- G4 built and
// fully proved LadderFilter/SvfFilter (Source/DSP/synth_filter.h) but
// deliberately left them driven only from Tests/filter_tests.cpp, because an
// early attempt at wiring them in broke the then-green G3.12 ("no DC")
// AC. That root cause (a pulse's DC of 2d-1, DESIGN.md §4.1) has since been
// fixed by the per-voice mixer DC blocker landing BEFORE the filter in the
// chain, so the interaction G4's gate note flagged no longer applies -- see
// synth_core.cpp's AUDIO-RATE LOOP comment for the live measurement. Each
// active voice holds BOTH a LadderFilter and an SvfFilter (Voice struct
// below): normally only the structure DESIGN.md §11's kLpfSlope currently
// selects is driven (per DESIGN.md §5.1 [PERF-8], "only one structure runs
// at a time"), but for the 20ms duration of a slope-switch crossfade BOTH
// run simultaneously and are cross-faded (DESIGN.md §5.1, docs/GATES.md
// G5.4) -- which is why every voice owns both structures unconditionally
// rather than only the currently-selected one.

// G7 (docs/GATES.md): the provisional "kG3Voices=8, reuse-same-note-else-
// first-idle-else-slot-0" array G3 built as a placeholder (its own comment
// named G7 as the gate that would replace it) is now the REAL allocator of
// DESIGN.md §10.3-§10.7: kMaxVoices=16 fixed physical voices plus 2 dedicated
// fade-out slots (kNumFadeSlots), Idle/oldest-Released/oldest-Playing
// allocation order (Source/DSP/synth_alloc.h's nassau_alloc::
// chooseVoiceForSteal, kept deliberately framework-free and decoupled from
// this file's heavy per-voice DSP state so it is unit-testable with no
// SynthCore at all), click-free 2ms-fade stealing, sustain pedal (CC64)
// Held state, CC120/123, pitch bend, glide, velocity->VCA/filter, and the
// three voice modes (Poly/Unison/Mono legato). `mVoices` is now sized
// kMaxVoices+kNumFadeSlots: indices [0,kMaxVoices) are the physical voice
// pool every getDebugVoice*(int) accessor addresses (unchanged shape from
// G3-G6's own use of indices 0-7, which remain valid physical slots),
// indices [kMaxVoices, kMaxVoices+kNumFadeSlots) are the two fade slots,
// addressed only internally (never by index from outside) and observed
// externally only via getDebugFadeSlotActive(). Both pools share the SAME
// Voice struct and the SAME per-voice control-rate/audio-rate processing
// code (a fade slot is literally a moved COPY of a stolen voice, still
// running its full DSP chain, DESIGN.md §10.4) -- the two differ only in
// how they enter/leave "active": a physical voice's state machine
// (nassau_alloc::SlotState) is driven by NoteEvents via applyEvent(); a
// fade slot's `fadeActive`/`fadeGainCur` are driven purely by its own
// 2ms sample counter, decoupled from any NoteEvent once started.

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
        if (voiceIndex < 0 || voiceIndex >= kMaxVoices) return 0.0;
        return mVoices[voiceIndex].debugLfoPitchModSemis;
    }

    /// G3.10 (and generally useful for driving deterministic multi-voice
    /// tests): true iff physical voice slot `voiceIndex` (0..kMaxVoices-1)
    /// currently has a sounding note -- Playing, Held or Released (any
    /// nassau_alloc::SlotState other than Idle), matching this accessor's
    /// original G3 meaning exactly (that gate's single `active` bool has
    /// become G7's 4-state nassau_alloc::SlotState, but "not Idle" is the
    /// same predicate either way).
    bool getDebugVoiceActive(int voiceIndex) const {
        if (voiceIndex < 0 || voiceIndex >= kMaxVoices) return false;
        return mVoices[voiceIndex].state != nassau_alloc::SlotState::Idle;
    }

    /// G3.6: ENV-F has no audible destination until G4/G5 (it does not drive
    /// the filter yet), so its own zipper-freedom cannot be measured from
    /// OUTPUT AUDIO the way ENV-A's can (R6) -- there is nothing for it to
    /// modulate yet. These two expose the raw envelope value (`y`, DESIGN.md
    /// §6) directly per voice slot instead, at CONTROL rate, which is the
    /// same underlying quantity that will reach audio once a filter exists
    /// to read it. Also anticipates G8.5's "getDebugEnvF()/getDebugEnvA()
    /// per voice" pattern.
    double getDebugEnvFValue(int voiceIndex) const {
        if (voiceIndex < 0 || voiceIndex >= kMaxVoices) return 0.0;
        return mVoices[voiceIndex].envF.y;
    }
    double getDebugEnvAValue(int voiceIndex) const {
        if (voiceIndex < 0 || voiceIndex >= kMaxVoices) return 0.0;
        return mVoices[voiceIndex].envA.y;
    }

    // ===== Test-only debug accessors (G4, docs/GATES.md) =====

    /// G4.9 (kept stable, unchanged behaviour, into G5): the RAW `kLpfCutoff`
    /// param after DESIGN.md §5.4's `[10, 0.45*fs]` clamp only -- deliberately
    /// WITHOUT any of G5's per-voice modulation terms (key follow/ENV-F/LFO),
    /// so this stays the same "is the robustness clamp itself real" readback
    /// G4.9 asserted, computed once per control block regardless of whether
    /// any voice is active (DESIGN.md §2: unmodulated, so it is not
    /// per-voice). See getDebugVoiceLpfCutoff() below for the ACTUAL
    /// modulated-and-clamped per-voice fc G5 feeds to each voice's filter.
    double getDebugLpfCutoff() const { return mDebugLpfCutoffHz; }

    // ===== Test-only debug accessors (G5, docs/GATES.md) =====

    /// G5.6: voice slot `voiceIndex`'s ACTUAL cutoff (Hz) -- kLpfCutoff after
    /// key follow, ENV-F and LFO modulation (DESIGN.md §5.4) AND the
    /// `[10, 0.45*fs]` clamp -- exactly the value most recently fed to that
    /// voice's filter structure(s)' setControlRate() (Source/DSP/
    /// synth_filter.h). 0.0 for an inactive slot (matches
    /// getDebugVoiceLfoPitchModSemis's own "0 for a slot that is not active"
    /// convention).
    double getDebugVoiceLpfCutoff(int voiceIndex) const {
        if (voiceIndex < 0 || voiceIndex >= kMaxVoices) return 0.0;
        return mVoices[voiceIndex].debugLpfCutoffHz;
    }

    // ===== Test-only debug accessors (G6, docs/GATES.md) =====

    /// G6.7: voice slot `voiceIndex`'s MIXER-stage output (post mixer DC
    /// block, pre-drive) for the most recently processed sample -- see
    /// Voice::Chain::debugMixOut's own comment. G8: reads CHAIN 0
    /// specifically -- chain 0 is the entire signal path in mono mode
    /// (every existing G6/G7 test's own scenario), so this accessor's
    /// meaning is unchanged from G6/G7.
    double getDebugVoiceMixOut(int voiceIndex) const {
        if (voiceIndex < 0 || voiceIndex >= kMaxVoices) return 0.0;
        return mVoices[voiceIndex].chain[0].debugMixOut;
    }
    /// G6.4: voice slot `voiceIndex`'s DRIVE-stage output for the most
    /// recently processed sample -- see Voice::Chain::debugDriveOut's own
    /// comment. 0.0 for an inactive slot (matches this class's established
    /// "0 for a slot that is not active" convention). G8: chain 0, see
    /// getDebugVoiceMixOut's own comment.
    double getDebugVoiceDriveOut(int voiceIndex) const {
        if (voiceIndex < 0 || voiceIndex >= kMaxVoices) return 0.0;
        return mVoices[voiceIndex].chain[0].debugDriveOut;
    }
    /// G6.4: voice slot `voiceIndex`'s HPF-stage output for the most
    /// recently processed sample -- see Voice::Chain::debugHpfOut's own
    /// comment. G8: chain 0, see getDebugVoiceMixOut's own comment.
    double getDebugVoiceHpfOut(int voiceIndex) const {
        if (voiceIndex < 0 || voiceIndex >= kMaxVoices) return 0.0;
        return mVoices[voiceIndex].chain[0].debugHpfOut;
    }
    /// G6.5: voice slot `voiceIndex`'s actual modulated+clamped HPF cutoff
    /// (Hz) -- see Voice::debugHpfCutoffHz's own comment. 0.0 while
    /// bypassed or inactive.
    double getDebugVoiceHpfCutoff(int voiceIndex) const {
        if (voiceIndex < 0 || voiceIndex >= kMaxVoices) return 0.0;
        return mVoices[voiceIndex].debugHpfCutoffHz;
    }

    // ===== Test-only debug accessors (G7, docs/GATES.md) =====

    /// DESIGN.md §10.3's real allocator's fixed physical-voice-pool size
    /// (16), constructed once in init(), never resized (R3). Public so
    /// Tests/ can size its own scratch arrays against the real number
    /// instead of a magic literal.
    static constexpr int kMaxVoices = 16;
    /// DESIGN.md §10.4: exactly two dedicated fade-out slots.
    static constexpr int kNumFadeSlots = 2;

    /// G7.2: physical voice slot `voiceIndex`'s (0..kMaxVoices-1) current
    /// nassau_alloc::SlotState, as a plain int (0=Idle, 1=Playing, 2=Held,
    /// 3=Released) so a G7.2 allocation-order test can assert exactly which
    /// index chooseVoiceForSteal() picked for each of the three
    /// Idle/Released/Playing scenarios the AC asks be constructed
    /// explicitly, by reading back the resulting state at specific indices.
    int getDebugVoiceState(int voiceIndex) const {
        if (voiceIndex < 0 || voiceIndex >= kMaxVoices) return static_cast<int>(nassau_alloc::SlotState::Idle);
        return static_cast<int>(mVoices[voiceIndex].state);
    }

    /// G7.3/G7.4: number of the two fade-out slots (DESIGN.md §10.4)
    /// currently mid-fade (0, 1 or 2) -- proves the STEALING MECHANISM
    /// itself, not merely the absence of a click, per this gate's own
    /// "assert the mechanism" instruction.
    int getDebugFadeSlotActive() const {
        int n = 0;
        for (int i = kMaxVoices; i < kMaxVoices + kNumFadeSlots; ++i) {
            if (mVoices[i].fadeActive) ++n;
        }
        return n;
    }

    /// G7.14/DESIGN.md §10.7 [PERF-7]: count of physical voices NOT yet
    /// eligible for the silent-voice skip -- i.e. NOT (ENV-A Idle AND this
    /// voice's stored peak over the previous control block was below
    /// -100dBFS), recomputed once per control block in controlRateUpdate().
    /// This is exactly the DESIGN.md §10.7 predicate, not merely the old G3
    /// "active" bool: because the VCA sits AFTER the filter in this design's
    /// signal chain (DESIGN.md §1), ENV-A reaching Idle already pins every
    /// subsequent sample's contribution at exactly 0.0 regardless of any
    /// filter ring-out, so in practice the two conditions coincide within
    /// one control block of each other here -- but the peak term is still
    /// computed and gated for real (not a no-op stand-in), matching DESIGN's
    /// literal two-part predicate and giving G11 a genuine per-voice peak
    /// signal to build real CPU savings on top of.
    int getDebugActiveVoiceCount() const { return mDebugActiveVoiceCount; }

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
        // NOTE: this comment marks the raw-field/atomic boundary for the G6
        // params -- the block-rate DERIVED fields for drive/output stage
        // (drivePre, driveKnee, masterVolumeLinear) live further down with
        // the rest of ParamSnapshot's "derived, BLOCK-RATE constants" (see
        // that section's own comment) rather than here, to keep this run of
        // fields a 1:1 mirror of the 53 setters, matching G0's original
        // layout note.
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

        // ---- G6: derived, BLOCK-RATE constants (DESIGN.md §2.2/§4/§11) ----
        // `kDrive` and `kMasterVolume` are each a SINGLE instrument-wide
        // param (no per-voice modulation, unlike the LPF/HPF cutoffs), so
        // their derived quantities belong here alongside the ADSR
        // coefficients above -- computed ONCE per host block by
        // finishSnapshot(), not per control block or per sample (R12: the
        // std::pow() calls involved are block-rate, legal; they must never
        // run at audio rate).
        double drivePre = 1.0;            ///< [dsp] DESIGN.md §4: 1 + 2*(Drive/100); ==1.0 EXACTLY at Drive=0
        double driveKnee = 0.0;           ///< [dsp] DESIGN.md §4: 3*(Drive/100)^1.5; ==0.0 EXACTLY at Drive=0
        double masterVolumeLinear = 1.0;  ///< [dsp] DESIGN.md §11: 10^(masterVolumeDb/20)

        // ---- G7: derived, BLOCK-RATE constant (DESIGN.md §2.2/§10.6) ----
        // kGlideTime is a single instrument-wide param (not per-voice), so
        // its one-pole step coefficient belongs here alongside the ADSR
        // coefficients above -- DESIGN.md §10.6 directs reusing "the same
        // convention as the envelope decay (§6)", so this IS
        // AdsrEnv::coeffForMs(glideTimeMs, AdsrEnv::decayDivisor(), ...),
        // not a separately re-derived formula (finishSnapshot(), block-rate
        // only, R12 -- involves std::exp via coeffForMs).
        double glideStepCoeff = 0.0;

        // ---- G8: derived, BLOCK-RATE constants (DESIGN.md §2.2/§9) ----
        // kStereoSpread is a single instrument-wide param, not per-voice or
        // per-chain, and its pan law (DESIGN.md §9) is PURE ARITHMETIC (add/
        // sub/mul only, no transcendental) -- so, like drivePre/driveKnee
        // above, it is computed ONCE per host block here rather than in the
        // audio-rate loop or even per control block (nothing modulates pan).
        // DESIGN.md §9's linear law verbatim: s = Spread/100; chain 0
        // gL=0.5+0.5s, gR=0.5-0.5s; chain 1 gL=0.5-0.5s, gR=0.5+0.5s. In MONO
        // mode (stereoMode==false) these collapse to gL0=gR0=1.0 (chain 0
        // only, DESIGN.md §9's own "mono mode: chain 0 only, gL=gR=1.0") and
        // gL1=gR1=0.0 (chain 1 is never even stepped in mono mode, so these
        // two values are never read, but are set to a defined, harmless
        // no-op multiplier rather than left at whatever finishSnapshot()'s
        // zero-initialisation happens to leave them at). Deliberately kept
        // as PLAIN 0.5/1.0 multiplies (never `1.0 - gR` or similar) so
        // `0.5*x + 0.5*x == x` bit-exactly (IEEE-754: halving and doubling
        // are both exact) -- this is the arithmetic G8.2's mono/stereo
        // bit-identity property is built on, and G8.6's `gL+gR==1.0` at
        // every chain/spread (verified: 0.5+term and 0.5-term always sum to
        // exactly 1.0 in IEEE-754 double, checked over a fine grid of
        // spread values as part of this gate's own verification).
        double panGainL0 = 1.0, panGainR0 = 1.0;
        double panGainL1 = 0.0, panGainR1 = 0.0;

        /// G8.9: control-rate one-pole coefficient for `mStereoBlend`'s
        /// mono<->stereo pan-law transition (kStereoBlendMs tau, see
        /// mStereoBlend's own field comment) -- block-rate derived (R12:
        /// involves std::exp via AdsrEnv::coeffForMs), same category as
        /// glideStepCoeff above, not a per-instrument-wide-param
        /// coincidence: it depends only on fsControl, which is fixed for a
        /// given sample rate/mControlBlock, so this is technically constant
        /// across the whole render -- computed once per host block anyway,
        /// matching every other derived constant's own convention here.
        double stereoBlendCoeff = 0.0;
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
        // ===== G8: per-CHAIN state (DESIGN.md §9) =====
        // DESIGN.md §1/§9: "per voice, per chain... everything below runs
        // once per chain, except the boxes marked shared". `chain` is `0` in
        // mono mode, `0` and `1` in stereo mode -- so everything that is
        // genuinely part of the duplicated signal path (both oscillators,
        // the sub, the mixer DC blocker, the drive's STATE -- there is none,
        // shapeTriodeK is memoryless -- the HPF, both LPF structures and
        // their g-interpolation, the post-LPF DC blocker, and the G6.4/G6.7
        // debug readbacks of THIS chain's own stages) lives in one nested
        // `Chain` struct, duplicated exactly via `Voice::chain[kNumChains]`.
        // What is explicitly NOT here (DESIGN.md §9's own "everything else
        // is shared" list, [PERF-5]): `noise` (DESIGN.md §3.5: ONE generator
        // per voice, shared by both chains -- an independently-seeded second
        // one breaks G8.2's bit-identity), `envF`/`envA` (ENV-F/ENV-A),
        // `vcaGainStart/End` (VCA gain), and the modulated
        // `debugLpfCutoffHz`/`debugHpfCutoffHz` cutoffs themselves (computed
        // ONCE per voice from shared glide/key-follow/ENV-F/LFO terms, then
        // fed identically into both chains' filter setControlRate() calls --
        // G8.5's "both chains' filter cutoffs are identical at every control
        // block") -- all of those stay single, un-indexed Voice fields
        // below, unchanged in shape from G7.
        struct Chain {
            Osc osc1, osc2;
            SubOsc sub;
            /// Per-chain DC blocker, applied to the MIXER OUTPUT before
            /// anything downstream (DESIGN.md §4). A pulse of duty d carries
            /// a DC offset of exactly (2d - 1) by construction -- measured
            /// -0.50 at PW=25% and -0.80 at PW=10% -- so PWM, one of this
            /// instrument's core sounds, emits large DC into the drive stage
            /// and the resonant filter unless it is coupled out. Real
            /// hardware does this with a capacitor. Each chain gets its OWN
            /// instance (not shared, DESIGN.md §9/G8: "confirm the second
            /// chain has its own blocker state and is not sharing or
            /// skipping one" -- a shared blocker fed two different signals
            /// interleaved would be a distinct, wrong filter).
            OnePoleHP dcBlock;
            /// SECOND DC blocker, on the LPF OUTPUT, before the VCA
            /// (DESIGN.md §5.6). The mixer blocker above cannot cover this:
            /// the filters' feedback saturators are ODD functions, and an
            /// odd function fed a zero-mean but NOT half-wave-symmetric
            /// signal (any pulse at duty != 50%) re-introduces a nonzero
            /// time-average. Measured without it: 2.3e-2 DC and a 3.69 peak
            /// at {SVF, res 99%, PW 30%, fc 500 Hz}. Same mechanism, and
            /// same remedy, as nassau-zermatt's mCfDcBlock and
            /// mPowerDcBlock. Own instance per chain, same reasoning as
            /// dcBlock above.
            OnePoleHP postLpfDcBlock;

            // ===== G5: per-chain LPF state (DESIGN.md §5.1-§5.4) =====
            // BOTH structures always exist per chain (see synth_core.h's G5
            // top-of-file note): normally only the one `kLpfSlope` currently
            // selects is driven; during a slope-switch crossfade both run
            // and are mixed (SynthCore::mLpfCrossfade* below drives the
            // shared timing, per-chain state lives here). Each chain's own
            // instances: the LADDER's coefficients (g/G/G2/G3/G4/k/invDenom)
            // are pure functions of the SHARED (fc, resonance) inputs, so
            // both chains' ladders end up bit-identical whenever their
            // inputs are; the SVF's `Reff` additionally depends on
            // `peakBpPrev`, this chain's OWN recent |bp| history -- which is
            // genuinely allowed to differ between chains once they carry
            // different (detuned) signals, matching DESIGN.md §9's "the two
            // filter/VCA state sets differ".
            LadderFilter lpfLadder;
            SvfFilter lpfSvf;
            double lastLpfOutput = 0.0;  ///< this chain's most recent LPF-stage output (mixed or
                                          ///< single-structure) -- used to seed the INCOMING
                                          ///< structure's state when a new crossfade starts (G5.4).

            // DESIGN.md §2: `gLpf` is one of exactly three quantities LINEARLY
            // INTERPOLATED PER SAMPLE across a control block (with vcaGain and,
            // from G6, gHpf) -- "Cur" is the per-sample-advancing value fed to
            // that structure's advanceCoeff() (Source/DSP/synth_filter.h) every
            // sample; "Step" is the constant per-sample increment, both set once
            // per control block in controlRateUpdate() from the structure's own
            // `g` before/after calling setControlRate(). Both structures carry
            // their own independent interpolation state because either can be
            // live in a given control block (the currently-selected slope, or
            // both during a crossfade). `nassau_real` (DESIGN.md §12.2): this
            // IS the per-sample-interpolated filter coefficient the
            // weak-machine path targets, so it carries the same type as the
            // structure's own `g` it feeds, not `double` narrowed at the
            // advanceCoeff() call boundary (which would waste the interpolation
            // add itself at full width for nothing).
            nassau_real gLpfLadderCur = 0.0, gLpfLadderStep = 0.0;
            nassau_real gLpfSvfCur = 0.0, gLpfSvfStep = 0.0;

            // ===== G6: per-chain HPF state (DESIGN.md §5.5) =====
            // ONE structure per chain (unlike the LPF's two: the HPF has no
            // slope-switch crossfade, DESIGN.md §5.1's [PERF-8] machinery is
            // LPF-only -- switching kHpfSlope mid-note is not covered by any G6
            // AC). `gHpfCur`/`gHpfStep` are this chain's own per-sample `gHpf`
            // interpolation state (DESIGN.md §2's third interpolated quantity,
            // alongside gLpf/vcaGain), the same shape as the LPF's own
            // gLpf*Cur/Step pair above. HpfCascade's own coefficient (`g`) is a
            // pure function of the SHARED, already-modulated cutoff Hz value
            // (DESIGN.md §9: HPF cutoff modulation uses only shared key-follow/
            // note terms), so both chains' `g` end up bit-identical whenever fed
            // the same Hz value -- which SynthCore::controlRateUpdate() does.
            HpfCascade hpf;
            nassau_real gHpfCur = 0.0, gHpfStep = 0.0;  // [dsp] nassau_real, same reasoning as gLpf*Cur/Step above

            /// G6.4 test-only readback: this CHAIN's DRIVE-stage output (post
            /// mixer DC block, post shapeTriodeK, pre-HPF) and HPF-stage output
            /// (post the hard-bypass check), for the MOST RECENTLY PROCESSED
            /// audio sample. Overwritten every sample in the audio-rate loop
            /// (cheap plain assignments, same pattern as lastLpfOutput above --
            /// R12-legal, no transcendental/atomic). Exists so G6.4 can assert
            /// the two are BIT-IDENTICAL when the HPF is bypassed by reading
            /// PRODUCTION's own real per-sample values, not a synthetic
            /// re-derivation of the drive formula that would not actually
            /// exercise the real bypass branch (R11). G8: the public
            /// getDebugVoiceMixOut/DriveOut/HpfOut(voiceIndex) accessors below
            /// read chain[0]'s copy specifically, unchanged in meaning from
            /// G6/G7 (chain 0 IS the whole signal path in mono mode, which is
            /// every existing G6/G7 test's own scenario).
            /// G6.7 test-only readback: this chain's MIXER-stage output (post
            /// mixer DC block, DESIGN.md §4.1, pre-drive) for the most recently
            /// processed sample -- same pattern/rationale as debugDriveOut/
            /// debugHpfOut just below.
            double debugMixOut = 0.0;
            double debugDriveOut = 0.0;
            double debugHpfOut = 0.0;
        };
        /// DESIGN.md §9: "chain is 0 in mono mode; 0 and 1 in stereo mode".
        /// Both slots are ALWAYS constructed (R3: fixed-size, no allocation
        /// in process()) -- chain[1] simply never gets stepped in the
        /// audio-rate loop while kStereoMode is off (docs/GATES.md G8
        /// deliverable: "The second per-voice chain, run only when
        /// kStereoMode is on").
        static constexpr int kNumChains = 2;
        Chain chain[kNumChains];

        NoiseSource noise;  ///< DESIGN.md §3.5: ONE generator per voice, SHARED by both
                             ///< stereo chains (not duplicated) -- see Chain's own top comment.
        AdsrEnv envF, envA;    ///< ENV-F (filter, DESIGN.md §5.4, G5) / ENV-A (VCA). DESIGN.md
                               ///< §9 [PERF-5]: shared by both chains, computed once per voice.

        double debugLpfCutoffHz = 0.0;  ///< G5.6/G8.5: this voice's actual modulated+clamped fc, as
                                         ///< last fed to EACH chain's setControlRate() (0 while
                                         ///< inactive) -- ONE value, computed once per voice and fed
                                         ///< identically to every chain (DESIGN.md §9 [PERF-5]), not
                                         ///< one per chain (docs/GATES.md G8.5).
        /// G6.5/G8.5: this voice's actual modulated+clamped HPF cutoff (Hz), as
        /// last fed to EACH chain's hpf.setControlRate() -- mirrors
        /// debugLpfCutoffHz's own convention exactly, including the G8.5 "one
        /// value per voice, not one per chain" property. Left at 0.0 while the
        /// HPF is bypassed (DESIGN.md §5.5): there is no "cutoff" for a stage
        /// that is not running, matching this class's "0 for a slot/state that
        /// does not apply" convention throughout.
        double debugHpfCutoffHz = 0.0;

        int note = -1;
        double vcaGainStart = 0.0;   ///< interpolation endpoints for THIS control block
        double vcaGainEnd = 0.0;     ///< (DESIGN.md §2: vcaGain is one of the 3 interpolated scalars)
        double debugLfoPitchModSemis = 0.0;  ///< test-only readback, G3.10

        // ===== G7: allocation state (DESIGN.md §10.3-§10.6) =====
        // `state` replaces G3's plain `active` bool: nassau_alloc::SlotState
        // (synth_alloc.h) is the 4-way Idle/Playing/Held/Released lifecycle
        // DESIGN.md §10.3 actually specifies. Idle here means EXACTLY what
        // the old `!active` meant (ENV-A has reached AdsrEnv::State::Idle --
        // see controlRateUpdate()), so every G3-G6 accessor/AC that read
        // `active` continues to read the equivalent `state != Idle`
        // predicate unchanged (getDebugVoiceActive()'s own comment).
        nassau_alloc::SlotState state = nassau_alloc::SlotState::Idle;
        uint32_t startedAt = 0;   ///< set at note-on; oldest-Playing tie-break (synth_alloc.h)
        uint32_t releasedAt = 0;  ///< set at note-off; oldest-Released tie-break (synth_alloc.h)

        /// DESIGN.md §10.6: exponential one-pole toward the target pitch, in
        /// semitones, at control rate -- `glideCurrentSemis` IS this voice's
        /// smoothed "note" term (DESIGN.md §3.2's pitch formula uses this in
        /// place of the bare integer note number; the two are bit-identical
        /// whenever glide has fully converged or kGlideTime==0, which is
        /// every existing G0-G6 AC's own scenario -- see the G7 gate report
        /// for the bit-exactness argument this depends on). Snapped equal to
        /// `glideTargetSemis` on any FRESH voice take-over (a new physical
        /// strike -- DESIGN.md §3.2's phase-reset note-on, Poly/Unison), left
        /// alone (so it keeps gliding) on a Mono-legato retarget.
        double glideCurrentSemis = 0.0;
        double glideTargetSemis = 0.0;

        /// DESIGN.md §11 kVelToVca/kVelToFilter, captured once per note-on
        /// (or per Mono-legato retarget) from that NoteEvent's own velocity
        /// -- see SynthCore::hardRetrigger()/legatoRetargetVoice()'s own
        /// comments for the two anchor points (100%: linear vel->gain /
        /// -1 octave-per-half-velocity; 0%: velocity has no effect either
        /// way) these formulas are built to hit exactly.
        double velVcaGain = 1.0;
        double velFilterOct = 0.0;

        /// DESIGN.md §10.5 Unison: this voice's fixed fine-detune offset
        /// (cents), added into BOTH oscillators' pitch (not just one) so the
        /// whole voice card is detuned, not one VCO within it. 0 outside
        /// Unison mode (nassau_alloc::unisonDetuneCentsFor() with groupSize
        /// <= 1, or simply never written).
        double unisonDetuneCents = 0.0;

        // ===== G7: fade-out slot state (DESIGN.md §10.4) =====
        // Meaningful ONLY for the two physical slots at index
        // [kMaxVoices, kMaxVoices+kNumFadeSlots) -- a main-pool voice's
        // `fadeActive` is always false and `fadeGainCur` always exactly
        // 1.0, which is why the audio-rate loop can multiply EVERY voice's
        // output by `fadeGainCur` unconditionally (mixSum += ... *
        // fadeGainCur) with zero cost to main-pool voices: x*1.0 is a
        // bit-exact IEEE-754 identity, so this extra multiply does not
        // perturb GoldenParityG5 (G7 gate report).
        bool fadeActive = false;
        int fadeSamplesTotal = 0;
        int fadeSamplesElapsed = 0;
        double fadeGainCur = 1.0;
        double fadeGainStep = 0.0;

        // ===== G7: silent-voice skip bookkeeping (DESIGN.md §10.7 [PERF-7]) ===
        // `curBlockPeakAccum` is the running max |contribution| over the
        // control block IN PROGRESS, updated every audio-rate sample (plain
        // std::max/fabs, no transcendental, R12-legal); at each control-rate
        // boundary controlRateUpdate() copies it into `peakPrevBlock` (the
        // quantity DESIGN.md §10.7 actually names, "peak over the previous
        // control block") and resets the accumulator for the next block.
        double curBlockPeakAccum = 0.0;
        double peakPrevBlock = 0.0;
    };

    /// DESIGN.md §10.3: the real allocator's pool is kMaxVoices=16 fixed
    /// physical voices (public constexpr, see this class's public section)
    /// plus kNumFadeSlots=2 dedicated fade-out slots (DESIGN.md §10.4) --
    /// ALL constructed here, in this fixed-size member array, never resized
    /// or allocated in process() (R3). Indices [0,kMaxVoices) are the
    /// physical pool every getDebugVoice*(int)/applyEvent() addresses by
    /// index; indices [kMaxVoices, kMaxVoices+kNumFadeSlots) are the two
    /// fade slots, entered only via stealToFadeSlot() (a plain struct copy,
    /// not an allocation) and left only by their own 2ms sample counter.
    Voice mVoices[kMaxVoices + kNumFadeSlots];

    // ===== DESIGN.md §12.3: SIMD-over-voices compacted active-chain list =====
    // [PERF-7]/DESIGN.md §12.3: silent voices are skipped entirely, and are
    // NOT contiguous in `mVoices` (a held chord over time scatters active
    // slots across the fixed 16-voice pool) -- so the audio-rate loop cannot
    // just pair (vi, vi+1) directly. `mActiveVoiceList` is a FIXED-SIZE
    // (R3: no allocation in process()) compaction of the currently-active
    // MAIN-POOL voice indices (fade slots are excluded -- see
    // Source/DSP/synth_core.cpp's process() for why), in ascending `vi`
    // order, rebuilt once per CONTROL BLOCK (controlRateUpdate(), the same
    // cadence [PERF-7]'s own activeCount already uses) from the SAME
    // predicate the audio-rate loop's skip check uses (`state != Idle`) --
    // so this list names EXACTLY the voices that render this block, in the
    // EXACT order they render in today's plain vi-ascending scalar loop.
    // Pairing adjacent LIST entries (not adjacent `vi`s) two at a time,
    // scalar remainder on an odd count, is what SynthCore::process()'s
    // audio-rate loop actually does with this (DESIGN.md §12.3).
    int mActiveVoiceList[kMaxVoices] = {};
    int mActiveVoiceCount = 0;

    int mHeldVoiceCount = 0;   ///< DESIGN.md §7: LFO delay retriggers only on 0->1 of this
    Lfo mLfo;                    ///< DESIGN.md §7 [PERF-4]: ONE global LFO, stepped once per
                                  ///< control block, read by every voice -- never per-voice.
    double mLastLfoValue = 0.0;  ///< test-only readback of the most recent mLfo.step() result (G3.10)

    // ===== G7: allocation bookkeeping (DESIGN.md §10.3-§10.6) =====
    uint32_t mVoiceClock = 0;   ///< monotonic counter, ++'d on every note-on/note-off; feeds
                                 ///< Voice::startedAt/releasedAt for chooseVoiceForSteal()'s
                                 ///< oldest-first tie-breaks (synth_alloc.h).
    int mNextFadeSlot = 0;      ///< 0/1 round-robin (DESIGN.md §10.4: "the oldest fade slot is
                                 ///< simply overwritten" -- alternating guarantees exactly that
                                 ///< with only 2 slots).
    bool mSustainHeld = false;  ///< CC64 (DESIGN.md §10.3).
    double mBendSemis = 0.0;    ///< DESIGN.md §3.2/§10: current pitch-bend offset, applied to
                                 ///< EVERY sounding voice every control block (G7.9), not just
                                 ///< new ones -- set by a PitchBend NoteEvent, DESIGN.md §10.1.

    /// DESIGN.md §10.5 Mono: last-note-priority held-note stack. Index
    /// [count-1] is the currently-sounding (topmost/most-recent) note.
    /// Fixed-size (R3) -- see pushMonoNote()'s own comment for the bound.
    static constexpr int kMaxMonoStack = 32;  // [voicing] generous fixed bound, no realistic
                                               // performance holds this many keys at once
    int mMonoNoteStack[kMaxMonoStack] = {};
    float mMonoVelStack[kMaxMonoStack] = {};
    int mMonoStackCount = 0;

    /// DESIGN.md §10.5 Unison: which note the fixed [0,poly) voice group is
    /// currently all playing, so a NoteOff can tell "this releases the
    /// group" from "this is a stray/mismatched note-off" apart. `false`
    /// once released.
    bool mUnisonActive = false;
    int mUnisonNote = -1;

    /// G7.14/DESIGN.md §10.7 [PERF-7]: count of physical voices not yet
    /// silent-skippable, recomputed once per control block -- see
    /// getDebugActiveVoiceCount()'s own (public) comment for the exact
    /// predicate.
    int mDebugActiveVoiceCount = 0;
    static constexpr double kSilentSkipThreshold = 1e-5;  // [ref] DESIGN.md §10.7: -100dBFS == 10^(-100/20)

    /// G6 (DESIGN.md §1/§11 "Output stage", §5.6's same rationale one stage
    /// further downstream): a THIRD 5 Hz one-pole DC blocker, on the FINAL
    /// summed output, after master volume and output clip. Found, not
    /// assumed: `shapeCubic` (the output clip) is itself an ODD, nonlinear
    /// function -- exactly `shapeTriodeK`'s own class of mechanism (DESIGN.md
    /// §5.6) -- and DRIVE (this gate) is a second odd nonlinearity ahead of
    /// it. Measured: with output clip OFF, the two existing per-voice
    /// blockers (mixer + post-LPF) already reduce drive's own DC to
    /// numerical noise (~1e-10) at every {PW, drive, HPF} corner tested; with
    /// output clip ON, a non-half-wave-symmetric signal (any pulse at duty
    /// != 50%, or a saw) reaching the clip's nonlinear region re-introduces
    /// DC that NEITHER existing blocker can remove, because both sit
    /// upstream of the clip -- worst measured corner 1.94e-3 (PW=25%,
    /// Drive=0%, HPF bypassed, output clip on), ~20x the project's standard
    /// 1e-4 bound.
    ///
    /// G8 DECISION (DESIGN.md §11's own "G8 is the gate positioned to decide
    /// whether this needs to become per-channel once L and R can genuinely
    /// differ" -- a decision this plan explicitly deferred, not one it made):
    /// NOW PER CHANNEL, `mOutputDcBlock[0]` = L, `[1]` = R. Once stereo mode
    /// gives L and R independent content (DESIGN.md §9), a single shared
    /// instance fed two DIFFERENT interleaved signals would not be either
    /// channel's correct 5 Hz high-pass -- it is a stateful IIR filter, and
    /// its one-sample memory would end up holding some blend of both
    /// channels' history. Splitting into two independent instances costs
    /// nothing new for MONO mode: both are reset identically in reset() and,
    /// while mono, are fed the IDENTICAL input sequence (mixSumL==mixSumR
    /// every sample, DESIGN.md §9 mono pan gains gL=gR=1.0) from an
    /// IDENTICAL starting state, so a deterministic per-sample IIR recursion
    /// necessarily produces BIT-IDENTICAL output sequences from both
    /// instances -- this is exactly what keeps G8.1 (mono L==R bit-exact)
    /// and every pre-G8 golden/unit AC (which only ever compared a single
    /// shared value duplicated to both channels) unperturbed.
    OnePoleHP mOutputDcBlock[2];
    double mDebugLpfCutoffHz = 0.0;  ///< G4.9: last clamped (UNMODULATED) LPF cutoff -- see
                                      ///< getDebugLpfCutoff()'s own comment for why this stays
                                      ///< unmodulated even after G5.

    // ===== G5: the LPF slope crossfade (DESIGN.md §5.1, docs/GATES.md G5.4) =====
    // `kLpfSlope` is a single INSTRUMENT-WIDE param (DESIGN.md §11), so the
    // crossfade's TIMING (which structure is "from", which is "to", how far
    // along) is tracked ONCE here, shared by every voice -- each voice's own
    // Voice::lpfLadder/lpfSvf hold the per-voice STATE the shared timing
    // drives. `mLpfSlopeSettled` is which structure is authoritative when no
    // crossfade is running (mirrors `snapshot.lpfSlope` once settled);
    // during a crossfade both structures run and are mixed per DESIGN.md
    // §5.1's 20ms window (docs/GATES.md G5.4).
    int mLpfSlopeSettled = static_cast<int>(LpfSlope::Db24);  ///< [voicing] DESIGN.md §11 default
    bool mLpfCrossfadeActive = false;
    int mLpfCrossfadeFromSlope = static_cast<int>(LpfSlope::Db24);
    int mLpfCrossfadeToSlope = static_cast<int>(LpfSlope::Db24);
    int mLpfCrossfadeSamplesTotal = 0;    ///< round(0.020 * fs), computed when a crossfade starts
    int mLpfCrossfadeSamplesElapsed = 0;  ///< advances once per AUDIO sample while active

    /// [voicing] DESIGN.md §5.1: "crossfades over 20ms" -- an engineering
    /// choice (G5.4's own tolerance is +/-2ms), not a measured hardware
    /// figure, hence [voicing] rather than [ref].
    static constexpr double kLpfCrossfadeSeconds = 0.020;

    // ===== G6: the HPF slope/bypass selection (DESIGN.md §5.5) =====
    // `kHpfSlope`/`kHpfCutoff` are single INSTRUMENT-WIDE params (DESIGN.md
    // §11), so -- exactly like the LPF's mLpfSlopeSettled above -- how many
    // poles run and whether the HPF is bypassed at all are each decided
    // ONCE per control block, shared by every voice, not per voice. Unlike
    // the LPF there is no crossfade to track (no G6 AC exercises a live
    // kHpfSlope switch), so this is just two plain cached values, not a
    // state machine.
    ///
    /// [voicing] DESIGN.md §5.5/§11: the low end of kHpfCutoff's 20..2000 Hz
    /// range doubles as the HARD BYPASS threshold ("kHpfCutoff at its
    /// minimum (20 Hz) is a HARD BYPASS... not a 20 Hz filter -- an actual
    /// bypass", docs/GATES.md G6.4). Compared against the RAW param value
    /// (before key-follow modulation), matching "the leftmost position of a
    /// real instrument's high-pass switch" -- a knob position, not a
    /// modulated result.
    static constexpr float kHpfCutoffMinHz = 20.0f;
    bool mHpfBypassed = true;  ///< re-derived from the current atomics on every reset()/first control block
    int mHpfNumPoles = 2;      ///< 2 (Db12) or 4 (Db24); matches kHpfSlope's default (Db12)

    // ===== G8: mono<->stereo pan-law blend (DESIGN.md §9, docs/GATES.md G8.9) =====
    // A mid-note `kStereoMode` toggle changes TWO things simultaneously for
    // every sounding voice: chain 0's OWN pan gains jump discontinuously
    // (mono's gL=gR=1.0 is a fundamentally different pan STATE from any
    // spread setting, not merely a special case of it), and chain 1
    // appears/disappears from cold (unstepped oscillator phase, at-rest
    // filter state) while ENV-A may already be fully open (a fresh note-on's
    // OWN phase reset is masked by its envelope's own attack from 0,
    // DESIGN.md §3.2 -- there is no such masking here).
    //
    // R11 FINDING (this gate's own report, found via measurement, TWICE):
    // (1) a first attempt fading only chain 1's contribution over a short
    // LINEAR 2ms ramp (DESIGN.md §10.4's own voice-steal convention) left
    // chain 0's own pan-gain jump untouched, and still measured 8-17 dB/ms
    // (against a 1dB/ms bound) once a wrap-coincidence measurement artifact
    // was ruled out (see docs/GATES.md's own G6.15/G7.12 "whole number of
    // periods" precedent for that class of artifact) -- e.g. R's OWN level
    // instantly drops toward its much lower target pan gain (spread=70%:
    // 1.0 -> 0.15, a -16.5dB step) the moment the switch takes effect,
    // regardless of how gently chain 1 fades in. (2) A LINEAR ramp cannot
    // fix this in principle, not just in degree: dB is logarithmic, so a
    // FIXED-DURATION linear-in-gain ramp toward a low (and, at Spread=100%,
    // EXACTLY ZERO) target has an UNBOUNDED dB/ms rate near the end of the
    // ramp, however long the ramp is made.
    //
    // Fixed with an EXPONENTIAL (constant-dB/ms) approach instead -- the
    // textbook remedy for exactly this class of problem, and the reason
    // pan/level automation in real mixing consoles is done in the log
    // domain. `mStereoBlend` (0 = mono pan law, chain 1 absent; 1 = the
    // configured target spread's linear pan law, chain 1 fully present)
    // approaches its target with `y += coeff*(target-y)` -- IDENTICAL in
    // shape to AdsrEnv's own control-rate stepping (SynthCore::
    // controlRateUpdate() advances it once per control block, not per
    // sample: DESIGN.md §2 already accepts a 32-sample/0.667ms hold for
    // "everything except the three interpolated scalars", and this is a
    // ONE-TIME transition spanning many control blocks, not a per-note
    // steady-state quantity). A one-pole APPROACHING a target gives a
    // CONSTANT rate of dB change per unit time even when that target is
    // EXACTLY ZERO gain (g(t) = g0*exp(-t/tau) => dB(t) is perfectly LINEAR
    // in t), unlike the linear-gain ramp this replaced -- `kStereoBlendMs`
    // (15ms) is chosen so the WORST-CASE initial rate, 8.686/tau_ms dB/ms
    // (Spread=100%, chasing a target of exactly 0), comfortably clears the
    // 1dB/ms bound with margin (measured worst case after this fix: see
    // this gate's own report).
    //
    // The EFFECTIVE pan gains actually used in the audio-rate loop
    // (`mEffPanGainL0/R0/L1/R1`, computed once per control block from
    // `mStereoBlend` and the snapshot's own exact target gains) interpolate
    // linearly between the MONO law {1,1,0,0} and the TARGET stereo law --
    // bit-exact identity at `mStereoBlend`==0.0 or ==1.0 EXACTLY (x*1.0==x,
    // x+0.0==x), which is what keeps G8.1/G8.2/G8.4/G8.6/G8.7/G8.8's own
    // bit-exactness/tolerance arguments unperturbed (those scenarios never
    // toggle mid-render, so `mStereoBlend` is pinned at an exact 0/1 the
    // whole time -- see `mControlRateEverRun`'s own comment below).
    //
    // `mControlRateEverRun` is what keeps this from firing a SPURIOUS blend
    // ramp on the very first control block of every stereo-mode render:
    // `init()`'s `reset()` call runs BEFORE a caller's own
    // `setStereoMode(true)` in the ordinary "configure params, then
    // process()" sequence every test/host uses, so comparing
    // `snapshot.stereoMode` against a reset-time value of `mChain1Active`
    // would misread "stereo mode was already configured at render start" as
    // "a transition happened on sample 0". The first control block any
    // instance ever runs (post reset()) instead SNAPS `mChain1Active` and
    // `mStereoBlend` directly from that block's own snapshot, no ramp; only
    // a control block AFTER that one can detect a genuine change.
    bool mControlRateEverRun = false;
    bool mChain1Active = false;
    double mStereoBlend = 0.0;
    double mEffPanGainL0 = 1.0, mEffPanGainR0 = 1.0, mEffPanGainL1 = 0.0, mEffPanGainR1 = 0.0;
    /// [voicing] Chosen so 8.686/kStereoBlendMs (the worst-case initial
    /// dB/ms rate of a one-pole chasing a target of exactly 0, DESIGN.md
    /// §9's Spread=100% case) clears the 1dB/ms bound with margin -- see the
    /// class comment above for the full derivation.
    static constexpr double kStereoBlendMs = 15.0;

    /// Runs chain `c`'s structure selected by `slope` (LpfSlope::Db24 (0) ->
    /// LadderFilter, Db12 (1) -> SvfFilter) on one sample `x`. DESIGN.md §2:
    /// `gLpf` is interpolated PER SAMPLE, so this first advances that
    /// structure's coefficient to its current per-sample-interpolated value
    /// (advanceCoeff(), Source/DSP/synth_filter.h -- an add for the ramp
    /// itself, R12's own "adds, not transcendentals" exception) before
    /// calling its already-R12-clean process() (G4.11), then steps the
    /// interpolation forward by one sample for next time. No transcendental,
    /// no atomic load anywhere in this function, so it is safe to call from
    /// inside the AUDIO-RATE LOOP (R12). G8: takes a `Voice::Chain&`
    /// (previously `Voice&`) so the SAME function serves either chain --
    /// the caller passes `v.chain[c]`.
    static inline double runLpfStructure(Voice::Chain& c, int slope, double x) {
        if (slope == static_cast<int>(LpfSlope::Db24)) {
            c.lpfLadder.advanceCoeff(c.gLpfLadderCur);
            c.gLpfLadderCur += c.gLpfLadderStep;
            return c.lpfLadder.process(x);
        }
        c.lpfSvf.advanceCoeff(c.gLpfSvfCur);
        c.gLpfSvfCur += c.gLpfSvfStep;
        return c.lpfSvf.process(x);
    }

    // ===== DESIGN.md §12.3: SIMD-over-voices =====
    // Only compiled/used when `nassau_real == double` (i.e. NASSAU_DSP_FLOAT
    // is OFF, the default build) -- Vec2d (synth_simd.h) has a well-defined
    // 2-wide `double` backend only; §12.2's opt-in float path is a separate,
    // ARM-motivated escape hatch this gate does not touch (see
    // synth_simd.h's own header comment for the full reasoning). Under
    // NASSAU_DSP_FLOAT, process() renders every voice through
    // processChainFilters() below one at a time, exactly as it always has.
#if !defined(NASSAU_DSP_FLOAT)
    /// The SIMD-paired twin of runLpfStructure() above: runs chain `ca`'s and
    /// chain `cb`'s LPF structure selected by `slope` TOGETHER, one sample
    /// each, via Source/DSP/synth_simd.h's ladderPairProcess()/
    /// svfPairProcess(). Bit-identical to two separate runLpfStructure()
    /// calls (see synth_simd.h's own bit-exactness argument) -- same
    /// advance-then-step-the-ramp shape, just two chains at once.
    static inline void runLpfStructurePair(Voice::Chain& ca, Voice::Chain& cb, int slope, double xA,
                                            double xB, double& yA, double& yB) {
        if (slope == static_cast<int>(LpfSlope::Db24)) {
            ladderPairProcess(ca.lpfLadder, cb.lpfLadder, ca.gLpfLadderCur, cb.gLpfLadderCur, xA, xB, yA, yB);
            ca.gLpfLadderCur += ca.gLpfLadderStep;
            cb.gLpfLadderCur += cb.gLpfLadderStep;
            return;
        }
        svfPairProcess(ca.lpfSvf, cb.lpfSvf, ca.gLpfSvfCur, cb.gLpfSvfCur, xA, xB, yA, yB);
        ca.gLpfSvfCur += ca.gLpfSvfStep;
        cb.gLpfSvfCur += cb.gLpfSvfStep;
    }
#endif

    /// One chain's worth of osc -> mix -> mixer DC block -> drive -> HPF ->
    /// LPF (single structure, or both blended during a slope crossfade) ->
    /// post-LPF DC block, for ONE sample -- factored out of the audio-rate
    /// loop (Source/DSP/synth_core.cpp) so the scalar remainder path, the
    /// two fade slots, and (under NASSAU_DSP_FLOAT) every voice all share
    /// EXACTLY one implementation with the SIMD-paired path below (DESIGN.md
    /// §12.3: "same operations, same order" is only checkable if there is
    /// only one copy of the scalar sequence to check). Sets ch's own
    /// debugMixOut/debugDriveOut/debugHpfOut/lastLpfOutput readbacks exactly
    /// as the pre-§12.3 inline code did. Returns the post-postLpfDcBlock
    /// filter output (pre gain/pan/fade-gain -- the caller applies those,
    /// which differ between a main-pool voice and a fade slot).
    double processChainFilters(Voice::Chain& ch, double ynoise, const ParamSnapshot& snapshot,
                                double crossfadeT);

#if !defined(NASSAU_DSP_FLOAT)
    /// The SIMD-paired twin of processChainFilters() above: runs TWO chains'
    /// osc/mix/drive scalar (unchanged, unvectorized -- DESIGN.md §12's own
    /// "vectorize?" table says no here) and their mixer DC block / HPF / LPF
    /// / post-LPF DC block TOGETHER via synth_simd.h's pair functions. `chA`/
    /// `chB` may be the SAME voice's two stereo chains (they share
    /// modulation but keep independent filter state, DESIGN.md §9) or two
    /// DIFFERENT voices' chain 0 (the mono/[PERF-7] compacted-list pairing,
    /// DESIGN.md §12.3) -- this function does not care which; the caller
    /// (process()) decides the pairing and is responsible for the
    /// gain/pan/peak bookkeeping this function does not do (see
    /// processChainFilters()'s own comment). Bit-identical to calling
    /// processChainFilters(chA, ...) then processChainFilters(chB, ...)
    /// separately -- see synth_simd.h's header comment for the argument.
    void processChainPairFilters(Voice::Chain& chA, double ynoiseA, Voice::Chain& chB, double ynoiseB,
                                  const ParamSnapshot& snapshot, double crossfadeT, double& outA,
                                  double& outB);
#endif

    static constexpr uint32_t kLfoShSeed = 0x5EED1234u;  // [voicing] fixed S&H seed, R8/R13

    /// Applies a single note-scoped event to the voice pool / mLfo (NoteOn/
    /// NoteOff/PitchBend/Sustain/AllNotesOff/AllSoundOff -- G7 wires all six,
    /// DESIGN.md §10.3/§10.6/§7.6). `snapshot` supplies the instrument-wide,
    /// non-per-voice terms an event needs (kPolyphony, kVoiceMode,
    /// kBendRange, kVelToVca/Filter, kStereoDetune for Unison's spread);
    /// `fs` is process()'s own already-read-once sample rate (DESIGN.md §2.2
    /// -- passed in rather than re-loading the mSampleRate atomic here, so
    /// this stays a single read per host block).
    void applyEvent(const NoteEvent& ev, const ParamSnapshot& snapshot, double fs);

    void handleNoteOn(int note, float vel, const ParamSnapshot& snapshot, double fs);
    void handleNoteOff(int note, const ParamSnapshot& snapshot);
    void handleSustain(bool down);
    void handleAllNotesOff();
    void handleAllSoundOff(double fs);

    /// Moves `mainPoolIndex`'s CURRENT occupant, unchanged, into the
    /// oldest-used of the two fade slots (DESIGN.md §10.4) and starts its
    /// 2ms linear fade -- a plain struct copy (Voice is POD-shaped: no
    /// pointers, no owning resources), never an allocation (R3). The caller
    /// is responsible for then overwriting `mainPoolIndex` itself (via
    /// hardRetrigger() or setVoiceState(..., Idle)); this function only
    /// evicts, it never assigns the freed slot's new content.
    void stealToFadeSlot(int mainPoolIndex, double fs);

    /// A full, fresh strike of physical voice `mainPoolIndex` for `note`
    /// (DESIGN.md §3.2: oscillator phase reset to 0; envelope noteOn(); a
    /// fresh glide-target snap, no portamento from whatever this slot's
    /// PREVIOUS occupant -- if any -- was playing). Used for every Poly
    /// allocation (fresh-Idle or post-steal) and for every Unison voice's
    /// per-note-on retrigger; NOT used for Mono-legato (legatoRetargetVoice
    /// below) or for Poly's "retrigger a sounding SAME note" reuse path,
    /// each of which has different envelope/glide semantics of its own.
    /// Does NOT decide whether to steal first -- the caller does that.
    void hardRetrigger(int mainPoolIndex, int note, float vel, const ParamSnapshot& snapshot,
                        double unisonDetuneCentsVal, bool freshEnvelope);

    /// Re-targets an ALREADY-SOUNDING voice at a new note WITHOUT touching
    /// its envelope or oscillator phase (DESIGN.md §10.5 Mono: "legato --
    /// no envelope retrigger while a key is still held"). Glide is NOT
    /// snapped here (the whole point: the pitch actually glides from
    /// wherever it currently is toward the new target). Velocity terms ARE
    /// updated (a genuine new NoteEvent's own velocity, or -- on a
    /// NoteOff-triggered fallback to an older held note -- that older
    /// note's OWN originally-captured velocity).
    void legatoRetargetVoice(Voice& v, int note, float vel, const ParamSnapshot& snapshot);

    /// A note-off that is actually silencing physical voice `mainPoolIndex`
    /// right now (as opposed to Mono's "revert to an older held note"
    /// non-event): routes to Held if the sustain pedal is currently down
    /// (DESIGN.md §10.3), else starts the envelope Release and marks
    /// Released.
    void releaseVoiceOrHold(int mainPoolIndex);

    /// Transitions physical voice `mainPoolIndex` to `newState`, maintaining
    /// mHeldVoiceCount (DESIGN.md §7's LFO 0->1 note-on edge) on every
    /// Idle<->non-Idle boundary crossed. The ONE place that ever writes
    /// Voice::state, so this invariant cannot be forgotten at a call site.
    void setVoiceState(int mainPoolIndex, nassau_alloc::SlotState newState);

    /// Removes `note` from the Mono-mode last-note-priority stack (the LAST
    /// matching occurrence, standard last-note-priority semantics) if
    /// present; a no-op if `note` is not on the stack (e.g. a stray
    /// NoteOff). Fixed-size, bounded silent drop past kMaxMonoStack on push
    /// (R3, mirrors mPendingEvents' own convention) -- not reachable by any
    /// realistic performance.
    void pushMonoNote(int note, float vel);
    void popMonoNote(int note);

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
