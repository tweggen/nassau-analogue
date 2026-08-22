#include "synth_core.h"

#include <algorithm>

// ===== Constructor & Initialization =====

SynthCore::SynthCore() {
    // All parameter atomics are already default-initialised at their in-class
    // default member initializers (see synth_core.h) — DESIGN.md §11's
    // default column, transcribed there with a [voicing] provenance tag.
    // Nothing else to do here: G0 has no DSP state to construct.
}

SynthCore::~SynthCore() {
    // No dynamic allocation; nothing to clean up.
}

void SynthCore::init(float sampleRate) {
    // [dsp] Accept the same sample-rate band nassau-zermatt's AmpCore does,
    // which covers the 5 rates G0.6 must demonstrate explicitly
    // (44100/48000/88200/96000/192000). Bogus/host-probe rates are ignored,
    // leaving the sample rate at its last valid value.
    if (sampleRate < 8000.0f || sampleRate > 192000.0f) {
        return;
    }
    mSampleRate.store(sampleRate, std::memory_order_relaxed);

    // Sizes/prepares all fixed (non-allocating, R3) buffers for this rate.
    // At G0 there are none yet — later gates' oscillator/filter/envelope/
    // voice state will be prepared here, all fixed-size for kMaxVoices + 2
    // fade slots (DESIGN.md §10.3), never allocated in process().
    reset();
}

void SynthCore::reset() {
    // DESIGN.md §11 "Reset semantics": clears state, never touches
    // parameters. At G0 the only internal state that exists is the
    // control-rate grid position (DESIGN.md §2) — later gates add oscillator
    // phase, filter state, envelope state, noise-generator seeds and the
    // voice allocator here, all under the same "never touch a parameter"
    // rule.
    mControlPhase = 0;
}

// ===== Audio Processing =====

void SynthCore::process(const NoteEvent* events, int numEvents,
                         float* outL, float* outR, int numSamples) {
    // G0: no voice allocation yet (that lands in G7, DESIGN.md §10.3). The
    // framework-free NoteEvent interface (DESIGN.md §10.1, R14) is already
    // load-bearing as an API shape — accepted here — but not yet consumed.
    (void)events;
    (void)numEvents;

    // DESIGN.md §2.2: exactly ONE ParamSnapshot per host block, every atomic
    // loaded exactly once (R3's mechanism — the audio-rate loop below never
    // touches an atomic, satisfying R12 too). G0 has no voice/filter/envelope
    // math to feed it yet, so the snapshot is built and discarded; G2-G8
    // progressively consume it instead of loading their own atomics per
    // sample.
    const ParamSnapshot snapshot = buildSnapshot();
    (void)snapshot;

    // DESIGN.md §2 [PERF-1]: control-rate sub-blocking, `mControlBlock`
    // samples per control block. `mControlPhase` is a PERSISTENT member (see
    // synth_core.h's process() doc comment and DESIGN.md §2) — NOT reset at
    // the top of this call — so the grid is anchored to the sample stream,
    // not to host block boundaries. This is what docs/GATES.md G3.3
    // (block-size invariance) depends on.
    //
    // At G0 there is nothing to do at a control-block boundary yet (no ADSR,
    // no LFO, no per-voice pitch/cutoff/gain recompute exists). G3 lands that
    // work at the "control-rate update point" marked below, inside this same
    // loop shape — the shape itself does not change from G0 onward.
    int n = 0;
    while (n < numSamples) {
        const int samplesToBoundary = mControlBlock - mControlPhase;
        const int samplesRemaining = numSamples - n;
        const int chunk = std::min(samplesToBoundary, samplesRemaining);

        // Audio-rate loop (R12: no transcendentals, no atomic loads in here —
        // trivially true at G0 since the body is a constant write). G0: exact
        // silence — no oscillator, filter or VCA exists yet (G2-G6 land the
        // real per-sample signal path behind this same loop).
        for (int i = 0; i < chunk; ++i) {
            outL[n + i] = 0.0f;
            outR[n + i] = 0.0f;
        }

        n += chunk;
        mControlPhase += chunk;
        if (mControlPhase >= mControlBlock) {
            mControlPhase = 0;
            // ---- control-rate update point (G3 lands real work here) ----
            // Per control block: ADSRs advance one step, the LFO advances one
            // step (globally, once — not per voice), pitch/filter-cutoff/
            // VCA-gain/mixer levels are recomputed (DESIGN.md §2). Nothing to
            // do yet at G0.
        }
    }
}

// ===== Per-host-block parameter snapshot (DESIGN.md §2.2) =====

SynthCore::ParamSnapshot SynthCore::buildSnapshot() const {
    ParamSnapshot s{};
    s.masterVolumeDb          = mMasterVolumeDb.load(std::memory_order_relaxed);
    s.outputClip              = mOutputClip.load(std::memory_order_relaxed) != 0.0f;
    s.osc1Wave                = mOsc1Wave.load(std::memory_order_relaxed);
    s.osc1Octave               = mOsc1Octave.load(std::memory_order_relaxed);
    s.osc1FineCents           = mOsc1FineCents.load(std::memory_order_relaxed);
    s.osc1PwPercent           = mOsc1PwPercent.load(std::memory_order_relaxed);
    s.osc1LevelPercent        = mOsc1LevelPercent.load(std::memory_order_relaxed);
    s.osc2Wave                = mOsc2Wave.load(std::memory_order_relaxed);
    s.osc2Octave               = mOsc2Octave.load(std::memory_order_relaxed);
    s.osc2Semi                = mOsc2Semi.load(std::memory_order_relaxed);
    s.osc2FineCents           = mOsc2FineCents.load(std::memory_order_relaxed);
    s.osc2PwPercent           = mOsc2PwPercent.load(std::memory_order_relaxed);
    s.osc2LevelPercent        = mOsc2LevelPercent.load(std::memory_order_relaxed);
    s.osc2Sync                = mOsc2Sync.load(std::memory_order_relaxed) != 0.0f;
    s.osc2KeyTrack            = mOsc2KeyTrack.load(std::memory_order_relaxed) != 0.0f;
    s.subOctave                = mSubOctave.load(std::memory_order_relaxed);
    s.subLevelPercent         = mSubLevelPercent.load(std::memory_order_relaxed);
    s.noiseColor                = mNoiseColor.load(std::memory_order_relaxed);
    s.noiseLevelPercent       = mNoiseLevelPercent.load(std::memory_order_relaxed);
    s.envFAttackMs            = mEnvFAttackMs.load(std::memory_order_relaxed);
    s.envFDecayMs             = mEnvFDecayMs.load(std::memory_order_relaxed);
    s.envFSustainPercent      = mEnvFSustainPercent.load(std::memory_order_relaxed);
    s.envFReleaseMs           = mEnvFReleaseMs.load(std::memory_order_relaxed);
    s.envAAttackMs            = mEnvAAttackMs.load(std::memory_order_relaxed);
    s.envADecayMs             = mEnvADecayMs.load(std::memory_order_relaxed);
    s.envASustainPercent      = mEnvASustainPercent.load(std::memory_order_relaxed);
    s.envAReleaseMs           = mEnvAReleaseMs.load(std::memory_order_relaxed);
    s.lfoWave                  = mLfoWave.load(std::memory_order_relaxed);
    s.lfoRateHz               = mLfoRateHz.load(std::memory_order_relaxed);
    s.lfoDelayMs              = mLfoDelayMs.load(std::memory_order_relaxed);
    s.lfoPitchAmountPercent   = mLfoPitchAmountPercent.load(std::memory_order_relaxed);
    s.lfoPwmAmountPercent     = mLfoPwmAmountPercent.load(std::memory_order_relaxed);
    s.lpfSlope                  = mLpfSlope.load(std::memory_order_relaxed);
    s.lpfCutoffHz             = mLpfCutoffHz.load(std::memory_order_relaxed);
    s.lpfResonancePercent     = mLpfResonancePercent.load(std::memory_order_relaxed);
    s.lpfEnvAmountPercent     = mLpfEnvAmountPercent.load(std::memory_order_relaxed);
    s.lpfKeyFollowPercent     = mLpfKeyFollowPercent.load(std::memory_order_relaxed);
    s.lpfLfoAmountPercent     = mLpfLfoAmountPercent.load(std::memory_order_relaxed);
    s.drivePercent            = mDrivePercent.load(std::memory_order_relaxed);
    s.hpfSlope                  = mHpfSlope.load(std::memory_order_relaxed);
    s.hpfCutoffHz             = mHpfCutoffHz.load(std::memory_order_relaxed);
    s.hpfKeyFollowPercent     = mHpfKeyFollowPercent.load(std::memory_order_relaxed);
    s.pmEnvFToOsc2Percent     = mPmEnvFToOsc2Percent.load(std::memory_order_relaxed);
    s.pmEnvFToPwPercent       = mPmEnvFToPwPercent.load(std::memory_order_relaxed);
    s.polyphony                 = mPolyphony.load(std::memory_order_relaxed);
    s.voiceMode                 = mVoiceMode.load(std::memory_order_relaxed);
    s.glideTimeMs             = mGlideTimeMs.load(std::memory_order_relaxed);
    s.bendRangeSemitones      = mBendRangeSemitones.load(std::memory_order_relaxed);
    s.velToVcaPercent         = mVelToVcaPercent.load(std::memory_order_relaxed);
    s.velToFilterPercent      = mVelToFilterPercent.load(std::memory_order_relaxed);
    s.stereoMode               = mStereoMode.load(std::memory_order_relaxed) != 0.0f;
    s.stereoDetuneCents       = mStereoDetuneCents.load(std::memory_order_relaxed);
    s.stereoSpreadPercent     = mStereoSpreadPercent.load(std::memory_order_relaxed);
    return s;
}

// ===== Parameter setters =====
// Every setter stores into its atomic with relaxed ordering: params are
// snapshotted (not synchronised against other state) once per block by
// process() via buildSnapshot(), and relaxed stores/loads are sufficient for
// a single scalar value with no ordering dependency on other memory (R3,
// G0.9).

void SynthCore::setMasterVolumeDb(float db) {
    mMasterVolumeDb.store(db, std::memory_order_relaxed);
}
void SynthCore::setOutputClip(bool enabled) {
    mOutputClip.store(enabled ? 1.0f : 0.0f, std::memory_order_relaxed);
}
void SynthCore::setOsc1Wave(Wave w) {
    mOsc1Wave.store(static_cast<int>(w), std::memory_order_relaxed);
}
void SynthCore::setOsc1Octave(Octave o) {
    mOsc1Octave.store(static_cast<int>(o), std::memory_order_relaxed);
}
void SynthCore::setOsc1FineCents(float cents) {
    mOsc1FineCents.store(cents, std::memory_order_relaxed);
}
void SynthCore::setOsc1PwPercent(float pct) {
    mOsc1PwPercent.store(pct, std::memory_order_relaxed);
}
void SynthCore::setOsc1LevelPercent(float pct) {
    mOsc1LevelPercent.store(pct, std::memory_order_relaxed);
}
void SynthCore::setOsc2Wave(Wave w) {
    mOsc2Wave.store(static_cast<int>(w), std::memory_order_relaxed);
}
void SynthCore::setOsc2Octave(Octave o) {
    mOsc2Octave.store(static_cast<int>(o), std::memory_order_relaxed);
}
void SynthCore::setOsc2Semi(int semitones) {
    mOsc2Semi.store(semitones, std::memory_order_relaxed);
}
void SynthCore::setOsc2FineCents(float cents) {
    mOsc2FineCents.store(cents, std::memory_order_relaxed);
}
void SynthCore::setOsc2PwPercent(float pct) {
    mOsc2PwPercent.store(pct, std::memory_order_relaxed);
}
void SynthCore::setOsc2LevelPercent(float pct) {
    mOsc2LevelPercent.store(pct, std::memory_order_relaxed);
}
void SynthCore::setOsc2Sync(bool enabled) {
    mOsc2Sync.store(enabled ? 1.0f : 0.0f, std::memory_order_relaxed);
}
void SynthCore::setOsc2KeyTrack(bool enabled) {
    mOsc2KeyTrack.store(enabled ? 1.0f : 0.0f, std::memory_order_relaxed);
}
void SynthCore::setSubOctave(SubOctave o) {
    mSubOctave.store(static_cast<int>(o), std::memory_order_relaxed);
}
void SynthCore::setSubLevelPercent(float pct) {
    mSubLevelPercent.store(pct, std::memory_order_relaxed);
}
void SynthCore::setNoiseColor(NoiseColor c) {
    mNoiseColor.store(static_cast<int>(c), std::memory_order_relaxed);
}
void SynthCore::setNoiseLevelPercent(float pct) {
    mNoiseLevelPercent.store(pct, std::memory_order_relaxed);
}
void SynthCore::setEnvFAttackMs(float ms) {
    mEnvFAttackMs.store(ms, std::memory_order_relaxed);
}
void SynthCore::setEnvFDecayMs(float ms) {
    mEnvFDecayMs.store(ms, std::memory_order_relaxed);
}
void SynthCore::setEnvFSustainPercent(float pct) {
    mEnvFSustainPercent.store(pct, std::memory_order_relaxed);
}
void SynthCore::setEnvFReleaseMs(float ms) {
    mEnvFReleaseMs.store(ms, std::memory_order_relaxed);
}
void SynthCore::setEnvAAttackMs(float ms) {
    mEnvAAttackMs.store(ms, std::memory_order_relaxed);
}
void SynthCore::setEnvADecayMs(float ms) {
    mEnvADecayMs.store(ms, std::memory_order_relaxed);
}
void SynthCore::setEnvASustainPercent(float pct) {
    mEnvASustainPercent.store(pct, std::memory_order_relaxed);
}
void SynthCore::setEnvAReleaseMs(float ms) {
    mEnvAReleaseMs.store(ms, std::memory_order_relaxed);
}
void SynthCore::setLfoWave(LfoWave w) {
    mLfoWave.store(static_cast<int>(w), std::memory_order_relaxed);
}
void SynthCore::setLfoRateHz(float hz) {
    mLfoRateHz.store(hz, std::memory_order_relaxed);
}
void SynthCore::setLfoDelayMs(float ms) {
    mLfoDelayMs.store(ms, std::memory_order_relaxed);
}
void SynthCore::setLfoPitchAmountPercent(float pct) {
    mLfoPitchAmountPercent.store(pct, std::memory_order_relaxed);
}
void SynthCore::setLfoPwmAmountPercent(float pct) {
    mLfoPwmAmountPercent.store(pct, std::memory_order_relaxed);
}
void SynthCore::setLpfSlope(LpfSlope s) {
    mLpfSlope.store(static_cast<int>(s), std::memory_order_relaxed);
}
void SynthCore::setLpfCutoffHz(float hz) {
    mLpfCutoffHz.store(hz, std::memory_order_relaxed);
}
void SynthCore::setLpfResonancePercent(float pct) {
    mLpfResonancePercent.store(pct, std::memory_order_relaxed);
}
void SynthCore::setLpfEnvAmountPercent(float pct) {
    mLpfEnvAmountPercent.store(pct, std::memory_order_relaxed);
}
void SynthCore::setLpfKeyFollowPercent(float pct) {
    mLpfKeyFollowPercent.store(pct, std::memory_order_relaxed);
}
void SynthCore::setLpfLfoAmountPercent(float pct) {
    mLpfLfoAmountPercent.store(pct, std::memory_order_relaxed);
}
void SynthCore::setDrivePercent(float pct) {
    mDrivePercent.store(pct, std::memory_order_relaxed);
}
void SynthCore::setHpfSlope(HpfSlope s) {
    mHpfSlope.store(static_cast<int>(s), std::memory_order_relaxed);
}
void SynthCore::setHpfCutoffHz(float hz) {
    mHpfCutoffHz.store(hz, std::memory_order_relaxed);
}
void SynthCore::setHpfKeyFollowPercent(float pct) {
    mHpfKeyFollowPercent.store(pct, std::memory_order_relaxed);
}
void SynthCore::setPmEnvFToOsc2Percent(float pct) {
    mPmEnvFToOsc2Percent.store(pct, std::memory_order_relaxed);
}
void SynthCore::setPmEnvFToPwPercent(float pct) {
    mPmEnvFToPwPercent.store(pct, std::memory_order_relaxed);
}
void SynthCore::setPolyphony(Polyphony p) {
    mPolyphony.store(static_cast<int>(p), std::memory_order_relaxed);
}
void SynthCore::setVoiceMode(VoiceMode m) {
    mVoiceMode.store(static_cast<int>(m), std::memory_order_relaxed);
}
void SynthCore::setGlideTimeMs(float ms) {
    mGlideTimeMs.store(ms, std::memory_order_relaxed);
}
void SynthCore::setBendRangeSemitones(int semitones) {
    mBendRangeSemitones.store(semitones, std::memory_order_relaxed);
}
void SynthCore::setVelToVcaPercent(float pct) {
    mVelToVcaPercent.store(pct, std::memory_order_relaxed);
}
void SynthCore::setVelToFilterPercent(float pct) {
    mVelToFilterPercent.store(pct, std::memory_order_relaxed);
}
void SynthCore::setStereoMode(bool enabled) {
    mStereoMode.store(enabled ? 1.0f : 0.0f, std::memory_order_relaxed);
}
void SynthCore::setStereoDetuneCents(float cents) {
    mStereoDetuneCents.store(cents, std::memory_order_relaxed);
}
void SynthCore::setStereoSpreadPercent(float pct) {
    mStereoSpreadPercent.store(pct, std::memory_order_relaxed);
}

// ===== Test-only debug accessors =====

void SynthCore::setDebugControlBlock(int samples) {
    mControlBlock = std::max(1, samples);
    mControlPhase = 0; // keep the grid position consistent when the size changes mid-test
}
