#include "synth_core.h"

#include <algorithm>
#include <cmath>

// ===== Constructor & Initialization =====

SynthCore::SynthCore() {
    // All parameter atomics are already default-initialised at their in-class
    // default member initializers (see synth_core.h) — DESIGN.md §11's
    // default column, transcribed there with a [voicing] provenance tag.
    // Voice/LFO state (G3) is likewise default-member-initialised
    // (Osc/AdsrEnv/etc. all zero/Idle by construction) -- reset() below is
    // what makes that state deterministic across reset()/fresh-instance
    // comparisons (R13), not this constructor.
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

    // G3: (re)derive the triangle leaky-integrator's per-sample leak
    // coefficient for the new rate (Osc::setSampleRate(), synth_osc.h) --
    // this is the ONE oscillator quantity that genuinely depends on fs
    // rather than being recomputed every control block, so it lives here
    // (called once per init(), not once per control block/sample, R12).
    // G7: ALL kMaxVoices+kNumFadeSlots physical slots -- the two fade slots
    // (DESIGN.md §10.4) are just as real a voice as the 16 main-pool ones
    // (a fade slot renders a full copy of a stolen voice's DSP chain, see
    // synth_core.h's own G7 top-of-file note), so they need their
    // sample-rate-dependent state prepared here too, once, in init() (R3:
    // never in process()).
    // G8: BOTH chain slots (DESIGN.md §9) get their sample-rate-dependent
    // state prepared here, unconditionally -- chain[1] is only ever STEPPED
    // in the audio-rate loop while kStereoMode is on, but it is always
    // CONSTRUCTED (R3), so it must be just as sample-rate-ready as chain[0]
    // the instant stereo mode is switched on, mid-note or otherwise.
    for (int i = 0; i < kMaxVoices + kNumFadeSlots; ++i) {
        for (int c = 0; c < Voice::kNumChains; ++c) {
            Voice::Chain& ch = mVoices[i].chain[c];
            ch.osc1.setSampleRate(sampleRate);
            ch.osc2.setSampleRate(sampleRate);
            // [dsp] Mixer DC blocker at 5 Hz -- the same corner nassau-zermatt
            // uses for its coupling-cap models, and far enough below the lowest
            // musical fundamental (16' at MIDI 0 is 4.1 Hz, but no envelope
            // sustains a note that low audibly) to be transparent in band.
            // DESIGN.md §4.
            ch.dcBlock.setFc(5.0, sampleRate);
            ch.postLpfDcBlock.setFc(5.0, sampleRate);
        }
    }
    // [dsp] G6: the THIRD (final-output) DC blocker -- see its own field
    // comment in synth_core.h for why it exists. Same 5Hz corner as the
    // other two (DESIGN.md §4/§11), for the identical reason. G8: now ONE
    // PER CHANNEL (mOutputDcBlock[0]==L, [1]==R) -- see mOutputDcBlock's own
    // field comment for why a single shared instance stopped being correct
    // once L and R can carry genuinely different content (DESIGN.md §9).
    for (int ch = 0; ch < 2; ++ch) {
        mOutputDcBlock[ch].setFc(5.0, sampleRate);
    }

    // G12 (DESIGN.md §13): the output-stage chorus's own fs-dependent
    // preparation -- delay-time-to-samples scale, the per-sample step of its
    // 20 ms wet fade, and the two BBD-bandwidth one-pole coefficients.
    // BEFORE reset() below, not after: JunoChorus::init() ends by calling
    // its own reset(), and doing it in this order means the reset() on the
    // next line is simply redundant for the chorus rather than being undone
    // by it.
    mChorus.init(static_cast<double>(sampleRate));

    // Sizes/prepares all fixed (non-allocating, R3) buffers for this rate.
    // G7: kMaxVoices+kNumFadeSlots is already a fixed-size array (synth_core.h).
    reset();
}

void SynthCore::reset() {
    // DESIGN.md §11 "Reset semantics": clears state, never touches
    // parameters. G3 adds oscillator phase, envelope state, noise-generator
    // seeds and the (provisional, G3-only) voice array/LFO here; later gates
    // add filter state and the real G7 allocator under the same "never touch
    // a parameter" rule.
    mControlPhase = 0;

    for (int i = 0; i < kMaxVoices + kNumFadeSlots; ++i) {
        Voice& v = mVoices[i];
        // G8: BOTH chain slots reset unconditionally (DESIGN.md §9) -- see
        // init()'s own comment on why chain[1] must stay reset-ready even
        // while kStereoMode is off.
        for (int c = 0; c < Voice::kNumChains; ++c) {
            Voice::Chain& ch = v.chain[c];
            ch.dcBlock.reset();
            ch.postLpfDcBlock.reset();
            ch.osc1.reset();   // full reset (phase + triangle integrator state) --
            ch.osc2.reset();   // distinct from the PHASE-ONLY resetPhase() a note-on
                               // uses (DESIGN.md §3.2), see applyEvent()'s comment.
            ch.sub.reset();
            ch.lpfLadder.reset();  // G5: both structures exist per chain regardless of kLpfSlope
            ch.lpfSvf.reset();     // (DESIGN.md §5.1 [PERF-8] crossfade), so both must reset here.
            ch.hpf.reset();        // G6: DESIGN.md §5.5
            ch.gHpfCur = 0.0;
            ch.gHpfStep = 0.0;
            ch.debugMixOut = 0.0;
            ch.debugDriveOut = 0.0;
            ch.debugHpfOut = 0.0;
            ch.lastLpfOutput = 0.0;
            ch.gLpfLadderCur = 0.0;
            ch.gLpfLadderStep = 0.0;
            ch.gLpfSvfCur = 0.0;
            ch.gLpfSvfStep = 0.0;
        }
        v.noise.init(i);  // [dsp] DESIGN.md §3.5/R8/R13: seeded from voice index; ONE
                           // generator per voice, shared by both chains -- not reset per chain.
        v.envF.reset();
        v.envA.reset();
        v.debugHpfCutoffHz = 0.0;
        v.debugLpfCutoffHz = 0.0;
        v.note = -1;
        v.vcaGainStart = 0.0;
        v.vcaGainEnd = 0.0;
        v.debugLfoPitchModSemis = 0.0;
        // ---- G7: allocation/glide/velocity/unison/fade state ----
        v.state = nassau_alloc::SlotState::Idle;
        v.startedAt = 0;
        v.releasedAt = 0;
        v.glideCurrentSemis = 0.0;
        v.glideTargetSemis = 0.0;
        v.velVcaGain = 1.0;
        v.velFilterOct = 0.0;
        v.unisonDetuneCents = 0.0;
        v.fadeActive = false;
        v.fadeSamplesTotal = 0;
        v.fadeSamplesElapsed = 0;
        v.fadeGainCur = 1.0;
        v.fadeGainStep = 0.0;
        v.curBlockPeakAccum = 0.0;
        v.peakPrevBlock = 0.0;
    }
    // DESIGN.md §12.3: the compacted active-chain list is fully REBUILT every
    // control block (controlRateUpdate()), never incrementally -- so leaving
    // stale index values in mActiveVoiceList here would be harmless either
    // way. Reset the count anyway (R13: a fresh instance and a reset()
    // instance should agree on every observable field, not merely on every
    // field something currently reads).
    mActiveVoiceCount = 0;
    for (int i = 0; i < kMaxVoices; ++i) mActiveVoiceList[i] = 0;

    mHeldVoiceCount = 0;
    mLfo.init(kLfoShSeed);  // [dsp] R8/R13: fixed S&H seed; Lfo::init() also resets phase/delay
    mLastLfoValue = 0.0;
    // G6/G8: one instance per channel now -- see mOutputDcBlock's own field
    // comment in synth_core.h.
    mOutputDcBlock[0].reset();
    mOutputDcBlock[1].reset();

    // ---- G7: allocator/performance-control reset (DESIGN.md §11 "Reset
    // semantics": clears state, never touches a parameter) ----
    mVoiceClock = 0;
    mNextFadeSlot = 0;
    mSustainHeld = false;
    mBendSemis = 0.0;
    mMonoStackCount = 0;
    mUnisonActive = false;
    mUnisonNote = -1;
    mDebugActiveVoiceCount = 0;

    // G5: the crossfade never survives a reset (DESIGN.md §11 "Reset
    // semantics": clears state, never touches params) -- re-settle on
    // whichever slope kLpfSlope's CURRENT atomic value says, so a reset()
    // immediately after setLpfSlope(X) does not spuriously start a
    // crossfade the next time controlRateUpdate() runs (it would otherwise
    // see mLpfSlopeSettled still at its stale pre-reset value).
    mLpfSlopeSettled = mLpfSlope.load(std::memory_order_relaxed);
    mLpfCrossfadeActive = false;
    mLpfCrossfadeFromSlope = mLpfSlopeSettled;
    mLpfCrossfadeToSlope = mLpfSlopeSettled;
    mLpfCrossfadeSamplesTotal = 0;
    mLpfCrossfadeSamplesElapsed = 0;

    // G6 (DESIGN.md §5.5): re-derive the shared HPF bypass/pole-count from
    // the CURRENT atomics, mirroring mLpfSlopeSettled's own reset()
    // pattern just above -- so a reset() immediately after setHpfCutoffHz()/
    // setHpfSlope() reflects that new value right away rather than a stale
    // pre-reset cache.
    mHpfBypassed = mHpfCutoffHz.load(std::memory_order_relaxed) <= kHpfCutoffMinHz;
    mHpfNumPoles = (mHpfSlope.load(std::memory_order_relaxed) == static_cast<int>(HpfSlope::Db24)) ? 4 : 2;

    // G8.9 (DESIGN.md §9): see the field comments in synth_core.h.
    // `mControlRateEverRun = false` is what makes the FIRST control block
    // after this reset() SNAP mChain1Active/mStereoBlend from whatever
    // kStereoMode currently is, with no ramp -- exactly like
    // mLpfSlopeSettled/mHpfBypassed just above, a reset() must not leave
    // stale state that spuriously looks like a "mid-note toggle" on the very
    // next control block.
    mControlRateEverRun = false;
    mChain1Active = false;
    mStereoBlend = 0.0;
    mEffPanGainL0 = 1.0;
    mEffPanGainR0 = 1.0;
    mEffPanGainL1 = 0.0;
    mEffPanGainR1 = 0.0;

    // G12 (DESIGN.md §13, §11 "Reset semantics"): clears the two delay
    // lines, the chorus LFO phase, the BBD filters and the wet fade gain --
    // and, via JunoChorus's own `mEverRun`, makes the NEXT control block
    // SNAP its geometry and wet gain from whatever kChorus currently says
    // instead of ramping into it. That is the same contract
    // mLpfSlopeSettled/mHpfBypassed/mControlRateEverRun above each have:
    // a reset() must not leave behind state that the next control block
    // mistakes for a runtime toggle. No parameter is touched.
    mChorus.reset();

    mPendingCount = 0;

    // mControlBlock, mDebugAtomicLoadCount and the two mDebug* VCA test hooks
    // are deliberately NOT touched here -- they are test-only instrumentation
    // / harness configuration, not synth state (same precedent as
    // mControlBlock already being left alone by this function since G0).
}

// ===== DESIGN.md §12.3: SIMD-over-voices, per-chain filter section =====
// This pair of functions is the ONE place the osc->mix->dcBlock->drive->
// hpf->lpf->postLpfDcBlock sequence is written out (DESIGN.md §12.3's own
// "same operations, same order" requirement is only checkable if there is
// exactly one copy of the scalar sequence to check): processChainFilters()
// below is a byte-for-byte extraction of the per-chain body every gate
// through G11 already exercised (no behaviour change), and
// processChainPairFilters() is its SIMD-paired twin, both declared in
// synth_core.h next to runLpfStructure()/runLpfStructurePair().

double SynthCore::processChainFilters(Voice::Chain& ch, double ynoise, const ParamSnapshot& snapshot,
                                       double crossfadeT) {
    const double prePhase1 = ch.osc1.phase;
    const auto r1 = ch.osc1.step();
    double y2 = ch.osc2.step().y;
    if (snapshot.osc2Sync && r1.wrapped) {
        y2 = ch.osc2.hardSync(r1.wrapFrac / ch.osc1.dt, y2);
    }
    const double ysub = ch.sub.step(prePhase1, ch.osc1.dt, r1.wrapped, true);

    // DC blocker on the mixer output (DESIGN.md §4) -- see this file's
    // pre-§12.3 per-chain loop comment (still present in synth_core.h's
    // Voice::Chain::dcBlock field comment) for the full "why it must sit
    // before the drive/filter" rationale.
    const double mixRaw = MixerBlock::mix(r1.y, snapshot.osc1LevelPercent, y2, snapshot.osc2LevelPercent,
                                           ysub, snapshot.subLevelPercent, ynoise, snapshot.noiseLevelPercent);
    const double mix = ch.dcBlock.process(mixRaw);
    ch.debugMixOut = mix;  // G6.7/G8 test-only readback, see Voice::Chain::debugMixOut

    // DESIGN.md §4/§1 step 7: drive sits between the mixer DC blocker and the
    // HPF. At Drive=0, snapshot.drivePre/driveKnee are EXACTLY 1.0/0.0, so
    // this whole line is bit-exact identity (DESIGN.md §4, G6.7).
    const double driven = shapeTriodeK(mix * snapshot.drivePre, snapshot.driveKnee) / snapshot.drivePre;
    ch.debugDriveOut = driven;  // G6.4/G8 test-only readback

    // DESIGN.md §5.5: HPF, hard-bypassed (an ARCHITECTURAL skip, not a very
    // low corner, G6.4) at kHpfCutoff's minimum.
    double hpfOut;
    if (mHpfBypassed) {
        hpfOut = driven;
    } else {
        ch.hpf.advanceCoeff(ch.gHpfCur);
        ch.gHpfCur += ch.gHpfStep;
        hpfOut = ch.hpf.process(driven, mHpfNumPoles);
    }
    ch.debugHpfOut = hpfOut;  // G6.4/G8 test-only readback

    // DESIGN.md §5.1/§5.4: the LPF -- single structure, or both blended
    // during a 20ms slope crossfade (DESIGN.md §5.1 [PERF-8], G5.4).
    double lpfOut;
    if (mLpfCrossfadeActive) {
        const double yFrom = runLpfStructure(ch, mLpfCrossfadeFromSlope, hpfOut);
        const double yTo = runLpfStructure(ch, mLpfCrossfadeToSlope, hpfOut);
        lpfOut = yFrom * (1.0 - crossfadeT) + yTo * crossfadeT;
    } else {
        lpfOut = runLpfStructure(ch, mLpfSlopeSettled, hpfOut);
    }
    ch.lastLpfOutput = lpfOut;  // G5.4's crossfade-seed source, see seedFromOutput()'s own comment

    // DESIGN.md §5.6: the post-filter DC blocker. Caller applies gain/pan/
    // fade-gain to this return value (DESIGN.md §12.3: this function stops
    // here so the main-pool-paired path and the fade-slot/remainder scalar
    // path, which apply those differently, share this one filter sequence).
    return ch.postLpfDcBlock.process(lpfOut);
}

#if !defined(NASSAU_DSP_FLOAT)
void SynthCore::processChainPairFilters(Voice::Chain& chA, double ynoiseA, Voice::Chain& chB, double ynoiseB,
                                         const ParamSnapshot& snapshot, double crossfadeT, double& outA,
                                         double& outB) {
    // Phase 1 (osc/mix): scalar, per lane, unchanged shape -- DESIGN.md
    // §12's own "vectorize?" table says no for oscillators/mixer (PolyBLEP
    // is branchy and a poor SIMD fit, and each is only ~2ns/voice).
    const double prePhase1A = chA.osc1.phase;
    const auto r1A = chA.osc1.step();
    double y2A = chA.osc2.step().y;
    if (snapshot.osc2Sync && r1A.wrapped) {
        y2A = chA.osc2.hardSync(r1A.wrapFrac / chA.osc1.dt, y2A);
    }
    const double ysubA = chA.sub.step(prePhase1A, chA.osc1.dt, r1A.wrapped, true);
    const double mixRawA = MixerBlock::mix(r1A.y, snapshot.osc1LevelPercent, y2A, snapshot.osc2LevelPercent,
                                            ysubA, snapshot.subLevelPercent, ynoiseA,
                                            snapshot.noiseLevelPercent);

    const double prePhase1B = chB.osc1.phase;
    const auto r1B = chB.osc1.step();
    double y2B = chB.osc2.step().y;
    if (snapshot.osc2Sync && r1B.wrapped) {
        y2B = chB.osc2.hardSync(r1B.wrapFrac / chB.osc1.dt, y2B);
    }
    const double ysubB = chB.sub.step(prePhase1B, chB.osc1.dt, r1B.wrapped, true);
    const double mixRawB = MixerBlock::mix(r1B.y, snapshot.osc1LevelPercent, y2B, snapshot.osc2LevelPercent,
                                            ysubB, snapshot.subLevelPercent, ynoiseB,
                                            snapshot.noiseLevelPercent);

    // Phase 2 (the SIMD-eligible middle, DESIGN.md §12's cost table): mixer
    // DC block -> drive (scalar) -> HPF -> LPF -> post-LPF DC block, two
    // chains at once via Source/DSP/synth_simd.h's pair functions. Bit-
    // identical to two processChainFilters() calls -- see synth_simd.h's
    // own header comment for the argument.
    double mixA, mixB;
    onePoleHpPairProcess(chA.dcBlock, chB.dcBlock, mixRawA, mixRawB, mixA, mixB);
    chA.debugMixOut = mixA;
    chB.debugMixOut = mixB;

    const double drivenA = shapeTriodeK(mixA * snapshot.drivePre, snapshot.driveKnee) / snapshot.drivePre;
    const double drivenB = shapeTriodeK(mixB * snapshot.drivePre, snapshot.driveKnee) / snapshot.drivePre;
    chA.debugDriveOut = drivenA;
    chB.debugDriveOut = drivenB;

    // mHpfBypassed is a single, instrument-wide, control-rate flag (DESIGN.md
    // §5.5) -- both lanes always take the SAME branch here, never diverge.
    double hpfOutA, hpfOutB;
    if (mHpfBypassed) {
        hpfOutA = drivenA;
        hpfOutB = drivenB;
    } else {
        hpfCascadePairProcess(chA.hpf, chB.hpf, chA.gHpfCur, chB.gHpfCur, drivenA, drivenB, mHpfNumPoles,
                               hpfOutA, hpfOutB);
        chA.gHpfCur += chA.gHpfStep;
        chB.gHpfCur += chB.gHpfStep;
    }
    chA.debugHpfOut = hpfOutA;
    chB.debugHpfOut = hpfOutB;

    // mLpfCrossfadeActive/mLpfCrossfadeFromSlope/ToSlope/mLpfSlopeSettled are
    // likewise single, instrument-wide, control-rate values -- both lanes
    // always take the same branch here too.
    double lpfOutA, lpfOutB;
    if (mLpfCrossfadeActive) {
        double yFromA, yFromB, yToA, yToB;
        runLpfStructurePair(chA, chB, mLpfCrossfadeFromSlope, hpfOutA, hpfOutB, yFromA, yFromB);
        runLpfStructurePair(chA, chB, mLpfCrossfadeToSlope, hpfOutA, hpfOutB, yToA, yToB);
        lpfOutA = yFromA * (1.0 - crossfadeT) + yToA * crossfadeT;
        lpfOutB = yFromB * (1.0 - crossfadeT) + yToB * crossfadeT;
    } else {
        runLpfStructurePair(chA, chB, mLpfSlopeSettled, hpfOutA, hpfOutB, lpfOutA, lpfOutB);
    }
    chA.lastLpfOutput = lpfOutA;
    chB.lastLpfOutput = lpfOutB;

    onePoleHpPairProcess(chA.postLpfDcBlock, chB.postLpfDcBlock, lpfOutA, lpfOutB, outA, outB);
}
#endif

// ===== Audio Processing =====

void SynthCore::process(const NoteEvent* events, int numEvents,
                         float* outL, float* outR, int numSamples) {
    // R11/G11 FINDING: ScopedNoDenormals was lifted verbatim in G1, verified by
    // G1.18 to genuinely set FTZ/DAZ rather than degrade to a no-op... and then
    // called from NOWHERE. It was absent from the audio path for nine gates.
    //
    // This is not theoretical. Every one-pole here decays exponentially, and the
    // output-stage DC blocker keeps running when the instrument is silent, so its
    // tail walks into denormal range and stays there. Measured on this box:
    //
    //     idle, fresh instance        18.7 ns/sample
    //     idle, after 60 s of silence 96.9 ns/sample     <- 5.2x, no notes played
    //
    // i.e. leaving the plugin open quintuples its CPU cost. On a weak machine
    // that is the difference between working and not. Isolated, the ladder alone
    // measured 222 ns/voice with denormal state against 12.6 ns with FTZ -- 17x.
    //
    // RAII, so the host's FP control word is restored on exit (G1.18).
    const ScopedNoDenormals noDenormals;

    // DESIGN.md §2.2: exactly ONE ParamSnapshot per host block, every atomic
    // loaded exactly once (R3's mechanism — the audio-rate loop below never
    // touches an atomic, satisfying R12 too). G3.1's instrumented count is
    // driven by buildSnapshot()'s own counting wrapper (see below) — nothing
    // else in this function increments it.
    ParamSnapshot snapshot = buildSnapshot();

    // fs is read exactly ONCE per host block too (block rate, not audio
    // rate) -- deliberately OUTSIDE buildSnapshot() so it does not count
    // against G3.1's param-count figure (mSampleRate is configuration, not
    // one of DESIGN.md §11's parameters). finishSnapshot() below computes
    // every block-rate DERIVED constant (ADSR coefficients, LFO increment)
    // from it plus the raw snapshot fields — DESIGN.md §2.2.
    const double fs = static_cast<double>(mSampleRate.load(std::memory_order_relaxed));
    const double fsControl = fs / std::max(1, mControlBlock);
    finishSnapshot(snapshot, fsControl);

    // Configure the LFO's wave/rate/delay from THIS host block's snapshot
    // BEFORE any event is applied below. This matters on a fresh instance's
    // (or first-note-of-a-block's) very first control-rate update: an event
    // this call delivers can call mLfo.noteOnEdge() (DESIGN.md §7's 0->1
    // retrigger) at the FIRST control-block boundary crossed, and
    // noteOnEdge() reads mLfo.delaySeconds to decide whether to gate depth
    // to 0 -- if that read happened before delaySeconds was ever set from
    // kLfoDelayMs (still at Lfo's own construction-time default of 0), a
    // configured delay would be silently skipped on the very note it should
    // apply to. Doing this once per HOST block (not per control block) is
    // also simply correct: delay/rate/wave are param-derived, not something
    // that needs re-deriving every control step.
    mLfo.wave = static_cast<Lfo::Wave>(snapshot.lfoWave);
    mLfo.setRateHz(snapshot.lfoRateHz, snapshot.fsControl);
    mLfo.setDelayMs(snapshot.lfoDelayMs);

    // DESIGN.md §10.2: events are quantised forward to the control-block
    // grid. `eventIdx` walks this call's own (assumed time-ordered, per
    // standard MIDI-queue convention) events array; any left unconsumed when
    // this call ends (because no further control-block boundary was reached)
    // are queued into mPendingEvents and drained at the very first boundary
    // a LATER call reaches — see that member's own comment.
    int eventIdx = 0;

    // DESIGN.md §2 [PERF-1]: control-rate sub-blocking, `mControlBlock`
    // samples per control block. `mControlPhase` is a PERSISTENT member (see
    // synth_core.h's process() doc comment and DESIGN.md §2) — NOT reset at
    // the top of this call — so the grid is anchored to the sample stream,
    // not to host block boundaries. This is what docs/GATES.md G3.3
    // (block-size invariance) depends on.
    int n = 0;
    while (n < numSamples) {
        // ---- control-rate update point ----
        // AT THE TOP, before rendering, whenever the grid sits on a boundary.
        //
        // This used to sit at the BOTTOM, after the chunk was rendered, which
        // cost a full extra control block of latency for any event landing
        // exactly on a boundary: an event at offset 0 with the grid already at
        // phase 0 was applied only after 32 samples had been rendered. That is
        // an off-by-one, not a design choice, and it is what made G7.8 measure
        // s+33 against its own [s, s+31] bound. Applying at the top makes an
        // event at a boundary take effect on the very next sample rendered.
        //
        // Note `<= n`, not `< n`: at the top of an iteration n is the position
        // about to be rendered, so an event AT n is due now. The old bottom
        // form used `< n` because n had already advanced past the chunk.
        if (mControlPhase == 0) {
            // Events queued by a PREVIOUS call that never reached a boundary
            // are chronologically earlier than this call's own -- drain first.
            for (int p = 0; p < mPendingCount; ++p) applyEvent(mPendingEvents[p], snapshot, fs);
            mPendingCount = 0;

            while (eventIdx < numEvents && events[eventIdx].sampleOffset <= n) {
                applyEvent(events[eventIdx], snapshot, fs);
                ++eventIdx;
            }

            // Per control block: ADSRs advance one step, the LFO advances one
            // step (globally, once — not per voice), pitch/VCA-gain are
            // recomputed (DESIGN.md §2).
            controlRateUpdate(snapshot, fs);
        }

        const int samplesToBoundary = mControlBlock - mControlPhase;
        const int samplesRemaining = numSamples - n;
        const int chunk = std::min(samplesToBoundary, samplesRemaining);
        const int blockSizeForFrac = std::max(1, mControlBlock);

        // G11.11(a): `frac` (posInBlock/blockSizeForFrac) is a FIXED per-
        // sample step within a control block -- it was previously
        // recomputed as a fresh integer->double DIVISION every sample
        // (`posInBlock / blockSizeForFrac`), even though posInBlock only
        // ever increases by exactly 1 between samples. One division per
        // CHUNK (not per sample) plus a per-sample ADD produces the
        // identical sequence of values: frac(i) = mControlPhase/block +
        // i*(1/block) is exactly what the division form computed, just
        // accumulated instead of re-divided (both are plain IEEE-754
        // double arithmetic, R12-legal either way -- this was never a
        // TRANSCENDENTAL, which is exactly why the R12 grep never flagged
        // it, docs/GATES.md G11.11).
        const double fracStep = 1.0 / static_cast<double>(blockSizeForFrac);
        double frac = static_cast<double>(mControlPhase) / static_cast<double>(blockSizeForFrac);

        // ---- AUDIO-RATE LOOP BEGIN (R12/R3: no transcendentals, no atomic
        // loads anywhere between this marker and AUDIO-RATE LOOP END --
        // G3.2's grep-based inspection test (Tests/envlfo_tests.cpp) checks
        // this region of THIS FILE verbatim, so keep any future edit to this
        // loop inside these two markers and free of tan/exp/exp2/pow/log/
        // sin/cos/.load(). The three DESIGN.md §2 interpolated scalars
        // (gLpf, gHpf, vcaGain) are a lerp -- add/sub/mul/div only. ----
        for (int i = 0; i < chunk; ++i) {

            // G5 (DESIGN.md §5.1's 20ms slope crossfade): this SAMPLE's
            // crossfade mix position, shared by every voice (the crossfade
            // is one instrument-wide event, DESIGN.md §11's kLpfSlope is a
            // single param -- see synth_core.h's mLpfCrossfade* comment).
            // Plain int-ratio division, exactly the same shape as `frac`
            // above (already R12-legal in this loop) -- not a transcendental.
            const double crossfadeT =
                mLpfCrossfadeActive
                    ? std::min(1.0, static_cast<double>(mLpfCrossfadeSamplesElapsed) /
                                         static_cast<double>(std::max(1, mLpfCrossfadeSamplesTotal)))
                    : 0.0;

            double mixSumL = 0.0, mixSumR = 0.0;
            // G8 (DESIGN.md §9): the number of chains genuinely STEPPED this
            // sample -- 1 in mono mode, 2 in stereo mode (or still 2 while
            // `mStereoBlend` has not yet fully settled to 0 after a mid-note
            // kStereoMode=off toggle, G8.9 -- see mStereoBlend's own field
            // comment). A single, shared, CONTROL-RATE value (only changes
            // once per control block, controlRateUpdate()), not a per-voice
            // or per-sample-recomputed-from-atomics decision (mChain1Active/
            // mStereoBlend are plain members, not atomics, R3/R12).
            const bool chain1Needed = mChain1Active || mStereoBlend > 0.0;
            const int numChains = chain1Needed ? 2 : 1;

            // DESIGN.md §12.3 (SIMD-over-voices): the MAIN POOL renders via
            // the compacted `mActiveVoiceList` (controlRateUpdate(), same
            // active/idle set and same ascending order the old plain
            // `vi`-scan used -- see that list's own field comment), paired
            // two at a time, scalar remainder on an odd count. Under
            // NASSAU_DSP_FLOAT (§12.2's separate, ARM-motivated opt-in path)
            // this reduces to the plain per-voice scalar scan, unchanged.
#if !defined(NASSAU_DSP_FLOAT)
            if (numChains == 2) {
                // STEREO: a voice's own two chains SHARE modulation
                // (ynoise/gain, [PERF-5]) but keep independent filter state
                // (DESIGN.md §9) -- they pair naturally, one SIMD pair per
                // active voice, no remainder ever needed (2 chains is always
                // even).
                for (int li = 0; li < mActiveVoiceCount; ++li) {
                    Voice& v = mVoices[mActiveVoiceList[li]];
                    const double ynoise = v.noise.step(static_cast<NoiseSource::Color>(snapshot.noiseColor));
                    const double gain = v.vcaGainStart + (v.vcaGainEnd - v.vcaGainStart) * frac;

                    double out0, out1;
                    processChainPairFilters(v.chain[0], ynoise, v.chain[1], ynoise, snapshot, crossfadeT,
                                             out0, out1);

                    // `fadeGainCur` is exactly 1.0 for every main-pool voice
                    // (DESIGN.md §10.4) -- x*1.0 is bit-exact IEEE-754
                    // identity, matching the pre-§12.3 scalar loop's own
                    // comment on this multiply.
                    const double contribution0 = out0 * gain * v.fadeGainCur;
                    const double contribution1 = out1 * gain * v.fadeGainCur;

                    // Same left-to-right accumulation as the pre-§12.3 scalar
                    // per-chain loop: chain 0's term before chain 1's, both
                    // added from a 0.0 start (DESIGN.md §9 pan law).
                    const double voiceL = contribution0 * mEffPanGainL0 + contribution1 * mEffPanGainL1;
                    const double voiceR = contribution0 * mEffPanGainR0 + contribution1 * mEffPanGainR1;
                    mixSumL += voiceL;
                    mixSumR += voiceR;

                    const double abs0 = contribution0 < 0.0 ? -contribution0 : contribution0;
                    const double abs1 = contribution1 < 0.0 ? -contribution1 : contribution1;
                    const double peakThisSample = abs0 > abs1 ? abs0 : abs1;
                    if (peakThisSample > v.curBlockPeakAccum) v.curBlockPeakAccum = peakThisSample;
                }
            } else {
                // MONO: pair adjacent ACTIVE voices' own chain 0 (DESIGN.md
                // §12.3's "pairing strategy" -- voices are not contiguous, so
                // this pairs list POSITIONS, not raw slot indices). Each
                // lane belongs to a DIFFERENT voice here, so its contribution
                // is added to mixSumL/R on its own, in ascending list order
                // (== ascending `vi` order, since the list is built that
                // way) -- exactly the order the old plain scalar scan summed
                // in, voice by voice.
                int li = 0;
                for (; li + 1 < mActiveVoiceCount; li += 2) {
                    Voice& vA = mVoices[mActiveVoiceList[li]];
                    Voice& vB = mVoices[mActiveVoiceList[li + 1]];
                    const double ynoiseA = vA.noise.step(static_cast<NoiseSource::Color>(snapshot.noiseColor));
                    const double ynoiseB = vB.noise.step(static_cast<NoiseSource::Color>(snapshot.noiseColor));
                    const double gainA = vA.vcaGainStart + (vA.vcaGainEnd - vA.vcaGainStart) * frac;
                    const double gainB = vB.vcaGainStart + (vB.vcaGainEnd - vB.vcaGainStart) * frac;

                    double outA, outB;
                    processChainPairFilters(vA.chain[0], ynoiseA, vB.chain[0], ynoiseB, snapshot, crossfadeT,
                                             outA, outB);

                    const double contribA = outA * gainA * vA.fadeGainCur;
                    mixSumL += contribA * mEffPanGainL0;
                    mixSumR += contribA * mEffPanGainR0;
                    const double absA = contribA < 0.0 ? -contribA : contribA;
                    if (absA > vA.curBlockPeakAccum) vA.curBlockPeakAccum = absA;

                    const double contribB = outB * gainB * vB.fadeGainCur;
                    mixSumL += contribB * mEffPanGainL0;
                    mixSumR += contribB * mEffPanGainR0;
                    const double absB = contribB < 0.0 ? -contribB : contribB;
                    if (absB > vB.curBlockPeakAccum) vB.curBlockPeakAccum = absB;
                }
                // Scalar remainder: at most one voice, when mActiveVoiceCount
                // is odd (DESIGN.md §12.3: "a scalar remainder for an odd
                // count").
                for (; li < mActiveVoiceCount; ++li) {
                    Voice& v = mVoices[mActiveVoiceList[li]];
                    const double ynoise = v.noise.step(static_cast<NoiseSource::Color>(snapshot.noiseColor));
                    const double gain = v.vcaGainStart + (v.vcaGainEnd - v.vcaGainStart) * frac;
                    const double out = processChainFilters(v.chain[0], ynoise, snapshot, crossfadeT);
                    const double contribution = out * gain * v.fadeGainCur;
                    mixSumL += contribution * mEffPanGainL0;
                    mixSumR += contribution * mEffPanGainR0;
                    const double absContribution = contribution < 0.0 ? -contribution : contribution;
                    if (absContribution > v.curBlockPeakAccum) v.curBlockPeakAccum = absContribution;
                }
            }
#else
            for (int li = 0; li < mActiveVoiceCount; ++li) {
                Voice& v = mVoices[mActiveVoiceList[li]];
                const double ynoise = v.noise.step(static_cast<NoiseSource::Color>(snapshot.noiseColor));
                const double gain = v.vcaGainStart + (v.vcaGainEnd - v.vcaGainStart) * frac;

                double voiceL = 0.0, voiceR = 0.0;
                double peakThisSample = 0.0;
                for (int c = 0; c < numChains; ++c) {
                    Voice::Chain& ch = v.chain[c];
                    const double out = processChainFilters(ch, ynoise, snapshot, crossfadeT);
                    const double contribution = out * gain * v.fadeGainCur;
                    if (c == 0) {
                        voiceL += contribution * mEffPanGainL0;
                        voiceR += contribution * mEffPanGainR0;
                    } else {
                        voiceL += contribution * mEffPanGainL1;
                        voiceR += contribution * mEffPanGainR1;
                    }
                    const double absContribution = contribution < 0.0 ? -contribution : contribution;
                    if (absContribution > peakThisSample) peakThisSample = absContribution;
                }
                mixSumL += voiceL;
                mixSumR += voiceR;
                if (peakThisSample > v.curBlockPeakAccum) v.curBlockPeakAccum = peakThisSample;
            }
#endif

            // Fade slots (DESIGN.md §10.4): always the scalar path. Only two
            // slots exist, they are short-lived (2ms) and -- unlike main-pool
            // voices -- `fadeActive` can flip mid-CHUNK on their own
            // per-sample counter below, so they cannot be folded into a
            // once-per-control-block compacted list the way main-pool
            // voices are (DESIGN.md §12.3). Iterated AFTER the main pool,
            // exactly as the old plain `vi`-ascending scan did (fade slots
            // sit at indices >= kMaxVoices), so mixSumL/R accumulate in the
            // SAME overall order as before.
            for (int vi = kMaxVoices; vi < kMaxVoices + kNumFadeSlots; ++vi) {
                Voice& v = mVoices[vi];
                if (!v.fadeActive) continue;

                const double ynoise = v.noise.step(static_cast<NoiseSource::Color>(snapshot.noiseColor));
                const double gain = v.vcaGainStart + (v.vcaGainEnd - v.vcaGainStart) * frac;

                double voiceL = 0.0, voiceR = 0.0;
                double peakThisSample = 0.0;
                for (int c = 0; c < numChains; ++c) {
                    Voice::Chain& ch = v.chain[c];
                    const double out = processChainFilters(ch, ynoise, snapshot, crossfadeT);
                    const double contribution = out * gain * v.fadeGainCur;
                    if (c == 0) {
                        voiceL += contribution * mEffPanGainL0;
                        voiceR += contribution * mEffPanGainR0;
                    } else {
                        voiceL += contribution * mEffPanGainL1;
                        voiceR += contribution * mEffPanGainR1;
                    }
                    const double absContribution = contribution < 0.0 ? -contribution : contribution;
                    if (absContribution > peakThisSample) peakThisSample = absContribution;
                }
                mixSumL += voiceL;
                mixSumR += voiceR;
                if (peakThisSample > v.curBlockPeakAccum) v.curBlockPeakAccum = peakThisSample;

                // G7 (DESIGN.md §10.4): advance this fade slot's own 2ms
                // linear ramp by one sample and retire it the instant it
                // completes -- same shape as the LPF slope crossfade's own
                // per-sample completion tracking just below (plain int/
                // double arithmetic only, R12).
                v.fadeGainCur += v.fadeGainStep;
                ++v.fadeSamplesElapsed;
                if (v.fadeSamplesElapsed >= v.fadeSamplesTotal) {
                    v.fadeActive = false;
                    v.fadeGainCur = 0.0;
                }
            }

            // G5: advance the crossfade's shared, AUDIO-RATE sample counter
            // and finalise it the instant it completes -- mid control-block
            // is fine (and correct: DESIGN.md §5.1's 20ms is a TIME window,
            // not a control-block-aligned one). Plain int arithmetic only,
            // no transcendental/atomic (R12).
            if (mLpfCrossfadeActive) {
                ++mLpfCrossfadeSamplesElapsed;
                if (mLpfCrossfadeSamplesElapsed >= mLpfCrossfadeSamplesTotal) {
                    mLpfCrossfadeActive = false;
                    mLpfSlopeSettled = mLpfCrossfadeToSlope;
                }
            }


            // G6 (DESIGN.md §11 "Output stage"): master volume then output
            // clip, applied to the SUMMED voice accumulator -- "post-
            // summation and post-master, so it limits the stack, not the
            // individual voice" (DESIGN.md §1's ordering constraint #4).
            // snapshot.masterVolumeLinear is a BLOCK-RATE derived constant
            // (finishSnapshot(), R12); shapeCubic() itself is pure
            // arithmetic (no transcendental), so it is audio-rate legal
            // (same category as shapeTriodeK above). G8: now applied
            // INDEPENDENTLY to mixSumL/mixSumR -- see mOutputDcBlock's own
            // field comment for the "G8 decision" this codifies. In mono
            // mode mixSumL==mixSumR bit-exact (established above), both
            // branches below run the IDENTICAL scalar math on IDENTICAL
            // inputs, so outL[n+i]==outR[n+i] bit-exact (G8.1) even though
            // each channel now goes through its OWN mOutputDcBlock[] instance.
            // G12 (DESIGN.md §1/§13): the Juno-style BBD chorus, on the
            // SUMMED accumulator and BEFORE master volume -- the real
            // instrument's own order (chorus after the VCAs, ahead of the
            // volume slider), which also leaves master volume as the last
            // trim before the clip's ceiling so the chorus's own +1.7 dB
            // typical gain is something the user can dial back out.
            //
            // With kChorus = Off this is a BIT-EXACT passthrough by
            // assignment (synth_chorus.h): `chorusL`/`chorusR` receive
            // `mixSumL`/`mixSumR` unchanged, which is what keeps both golden
            // batteries verifying at exactly 0.000e+00 and keeps G8.1's
            // mono L==R bit-identity intact across this gate. The delay
            // lines are still WRITTEN while off, so switching the chorus on
            // mid-note starts from real signal history -- two stores, no
            // branch on the hot path, and nothing ever has to clear 32 KB
            // from the audio thread.
            //
            // R12: everything in JunoChorus::process() is compare/add/
            // multiply plus two masked array reads -- no transcendental, no
            // atomic load. `frac` is the SAME control-block position the VCA
            // gain is interpolated with, which is what makes the delay sweep
            // block-size invariant (G3.3).
            double chorusL = mixSumL, chorusR = mixSumR;
            mChorus.process(mixSumL, mixSumR, frac, chorusL, chorusR);

            double outSampleL = chorusL * snapshot.masterVolumeLinear;
            double outSampleR = chorusR * snapshot.masterVolumeLinear;
            if (snapshot.outputClip) {
                outSampleL = shapeCubic(outSampleL, 2.0);  // [dsp] DESIGN.md §11, L=2.0 -> +6dBFS ceiling
                outSampleR = shapeCubic(outSampleR, 2.0);
                // G6 finding (see mOutputDcBlock's own field comment): the
                // clip just above is an odd nonlinearity with nothing
                // downstream to remove the DC it can re-introduce from a
                // non-half-wave-symmetric signal -- this third blocker is
                // that removal. Deliberately only run WHEN the clip itself
                // runs (not unconditionally every sample): with the clip
                // off there is no clip-introduced DC to begin with (the two
                // existing per-voice blockers already leave drive's own DC
                // at numerical noise, measured ~1e-10 at every corner), and
                // gating it this way is what keeps kOutputClip=off a
                // bit-exact passthrough of the master-scaled sum (DESIGN.md
                // §11, docs/GATES.md G6.12).
                outSampleL = mOutputDcBlock[0].process(outSampleL);
                outSampleR = mOutputDcBlock[1].process(outSampleR);
            }
            outL[n + i] = static_cast<float>(outSampleL);
            outR[n + i] = static_cast<float>(outSampleR);

            // G11.11(a): accumulate `frac` for the NEXT sample instead of
            // re-dividing (see the fracStep comment above this loop) -- a
            // plain add, R12-legal.
            frac += fracStep;
        }
        // ---- AUDIO-RATE LOOP END ----

        n += chunk;
        mControlPhase += chunk;
        if (mControlPhase >= mControlBlock) {
            mControlPhase = 0;
        }
    }

    // Any events this call never reached a boundary for: queue them for the
    // next call (DESIGN.md §10.2 — see mPendingEvents's own comment). Silent,
    // bounded drop past kMaxPendingEvents rather than an allocation (R3);
    // not reachable by any realistic event stream at this queue's size.
    for (; eventIdx < numEvents && mPendingCount < kMaxPendingEvents; ++eventIdx) {
        mPendingEvents[mPendingCount++] = events[eventIdx];
    }
}

// ===== Per-host-block parameter snapshot (DESIGN.md §2.2) =====

SynthCore::ParamSnapshot SynthCore::buildSnapshot() const {
    // G3.1: every .load() below goes through this counting wrapper, so a
    // debug build (or a Release one -- the counter is cheap and unconditional,
    // there is no "debug build" config distinction in this tree) can assert
    // "exactly kNumParams * numHostBlocks" (docs/GATES.md G3.1 -- 53 when
    // that AC was written, 54 since G12 appended kChorus; the test reads
    // SynthCore::kNumParams rather than the literal). A generic lambda
    // (C++17) so it works uniformly across the float/int/bool-as-float
    // atomics below without a template function per type.
    auto ld = [this](const auto& atomicRef) {
        ++mDebugAtomicLoadCount;
        return atomicRef.load(std::memory_order_relaxed);
    };

    ParamSnapshot s{};
    s.masterVolumeDb          = ld(mMasterVolumeDb);
    s.outputClip              = ld(mOutputClip) != 0.0f;
    s.osc1Wave                = ld(mOsc1Wave);
    s.osc1Octave               = ld(mOsc1Octave);
    s.osc1FineCents           = ld(mOsc1FineCents);
    s.osc1PwPercent           = ld(mOsc1PwPercent);
    s.osc1LevelPercent        = ld(mOsc1LevelPercent);
    s.osc2Wave                = ld(mOsc2Wave);
    s.osc2Octave               = ld(mOsc2Octave);
    s.osc2Semi                = ld(mOsc2Semi);
    s.osc2FineCents           = ld(mOsc2FineCents);
    s.osc2PwPercent           = ld(mOsc2PwPercent);
    s.osc2LevelPercent        = ld(mOsc2LevelPercent);
    s.osc2Sync                = ld(mOsc2Sync) != 0.0f;
    s.osc2KeyTrack            = ld(mOsc2KeyTrack) != 0.0f;
    s.subOctave                = ld(mSubOctave);
    s.subLevelPercent         = ld(mSubLevelPercent);
    s.noiseColor                = ld(mNoiseColor);
    s.noiseLevelPercent       = ld(mNoiseLevelPercent);
    s.envFAttackMs            = ld(mEnvFAttackMs);
    s.envFDecayMs             = ld(mEnvFDecayMs);
    s.envFSustainPercent      = ld(mEnvFSustainPercent);
    s.envFReleaseMs           = ld(mEnvFReleaseMs);
    s.envAAttackMs            = ld(mEnvAAttackMs);
    s.envADecayMs             = ld(mEnvADecayMs);
    s.envASustainPercent      = ld(mEnvASustainPercent);
    s.envAReleaseMs           = ld(mEnvAReleaseMs);
    s.lfoWave                  = ld(mLfoWave);
    s.lfoRateHz               = ld(mLfoRateHz);
    s.lfoDelayMs              = ld(mLfoDelayMs);
    s.lfoPitchAmountPercent   = ld(mLfoPitchAmountPercent);
    s.lfoPwmAmountPercent     = ld(mLfoPwmAmountPercent);
    s.lpfSlope                  = ld(mLpfSlope);
    s.lpfCutoffHz             = ld(mLpfCutoffHz);
    s.lpfResonancePercent     = ld(mLpfResonancePercent);
    s.lpfEnvAmountPercent     = ld(mLpfEnvAmountPercent);
    s.lpfKeyFollowPercent     = ld(mLpfKeyFollowPercent);
    s.lpfLfoAmountPercent     = ld(mLpfLfoAmountPercent);
    s.drivePercent            = ld(mDrivePercent);
    s.hpfSlope                  = ld(mHpfSlope);
    s.hpfCutoffHz             = ld(mHpfCutoffHz);
    s.hpfKeyFollowPercent     = ld(mHpfKeyFollowPercent);
    s.pmEnvFToOsc2Percent     = ld(mPmEnvFToOsc2Percent);
    s.pmEnvFToPwPercent       = ld(mPmEnvFToPwPercent);
    s.polyphony                 = ld(mPolyphony);
    s.voiceMode                 = ld(mVoiceMode);
    s.glideTimeMs             = ld(mGlideTimeMs);
    s.bendRangeSemitones      = ld(mBendRangeSemitones);
    s.velToVcaPercent         = ld(mVelToVcaPercent);
    s.velToFilterPercent      = ld(mVelToFilterPercent);
    s.stereoMode               = ld(mStereoMode) != 0.0f;
    s.stereoDetuneCents       = ld(mStereoDetuneCents);
    s.stereoSpreadPercent     = ld(mStereoSpreadPercent);
    s.chorusMode                = ld(mChorusMode);
    return s;
}

// ===== G3: block-rate derived constants (DESIGN.md §2.2) =====

void SynthCore::finishSnapshot(ParamSnapshot& s, double fsControl) {
    s.fsControl = fsControl;
    // [dsp] DESIGN.md §6 -- AdsrEnv's own derivation (attack/decay/release
    // divisors), computed via AdsrEnv::coeffForMs(), not re-derived here.
    s.envFAttackCoeff  = AdsrEnv::coeffForMs(s.envFAttackMs,  AdsrEnv::attackDivisor(),  fsControl);
    s.envFDecayCoeff   = AdsrEnv::coeffForMs(s.envFDecayMs,   AdsrEnv::decayDivisor(),   fsControl);
    s.envFReleaseCoeff = AdsrEnv::coeffForMs(s.envFReleaseMs, AdsrEnv::releaseDivisor(), fsControl);
    s.envFSustainLevel = std::clamp(static_cast<double>(s.envFSustainPercent) * 0.01, 0.0, 1.0);
    s.envAAttackCoeff  = AdsrEnv::coeffForMs(s.envAAttackMs,  AdsrEnv::attackDivisor(),  fsControl);
    s.envADecayCoeff   = AdsrEnv::coeffForMs(s.envADecayMs,   AdsrEnv::decayDivisor(),   fsControl);
    s.envAReleaseCoeff = AdsrEnv::coeffForMs(s.envAReleaseMs, AdsrEnv::releaseDivisor(), fsControl);
    s.envASustainLevel = std::clamp(static_cast<double>(s.envASustainPercent) * 0.01, 0.0, 1.0);
    // [dsp] DESIGN.md §7: LFO increment is a plain divide, no transcendental.
    s.lfoIncrement = static_cast<double>(s.lfoRateHz) / std::max(fsControl, 1e-6);
    s.lfoDelaySeconds = std::max(static_cast<double>(s.lfoDelayMs), 0.0) * 0.001;

    // [dsp] DESIGN.md §4: pre = 1 + 2*(Drive/100); knee = 3*(Drive/100)^1.5.
    // At Drive=0 these are EXACTLY 1.0 and 0.0 (not merely close): 1+2*0.0
    // is exact IEEE-754 arithmetic, and std::pow(0.0, 1.5) == 0.0 exactly --
    // this is what makes G6.7's drive-bypass bit-exactness hold. Block-rate
    // only (std::pow is transcendental, R12) -- Drive is a single
    // instrument-wide param, not per-voice, so this belongs here alongside
    // the ADSR coefficients above, not in the per-voice control-rate loop.
    const double drivePct = static_cast<double>(s.drivePercent) * 0.01;
    s.drivePre = 1.0 + 2.0 * drivePct;
    s.driveKnee = 3.0 * std::pow(drivePct, 1.5);

    // [dsp] DESIGN.md §11 "Output stage": MasterVolume_linear = 10^(dB/20).
    // Also block-rate only (std::pow), also a single instrument-wide param.
    s.masterVolumeLinear = std::pow(10.0, static_cast<double>(s.masterVolumeDb) / 20.0);

    // [dsp] G7/DESIGN.md §10.6: "tau = t/4.605 -- same convention as the
    // envelope decay (§6), deliberately" -- literally AdsrEnv::coeffForMs()
    // with the decay divisor (ln(100)), not a separately re-derived
    // formula. kGlideTime is a single instrument-wide param, so this
    // belongs here (block-rate, R12: std::exp via coeffForMs). At
    // glideTimeMs==0 (coeffForMs's own ms-floor of 1e-3ms), tau is so small
    // relative to fsControl that exp(-1/(tau*fsControl)) underflows to
    // exactly 0.0 in IEEE double, making this EXACTLY 1.0 -- a full jump to
    // target on the very first control step ("instantaneous (first control
    // block)", G7.10) -- and matches AdsrEnv's own "y += coeff*(target-y)"
    // stepping convention used throughout this class (controlRateUpdate()).
    s.glideStepCoeff = AdsrEnv::coeffForMs(static_cast<double>(s.glideTimeMs), AdsrEnv::decayDivisor(), fsControl);

    // [dsp] G8/DESIGN.md §9: the linear pan law, verbatim. kStereoSpread is a
    // single instrument-wide param (not per-voice, not per-chain, not
    // modulated by anything), so -- like drivePre/driveKnee/masterVolumeLinear
    // above -- its derived gains are computed ONCE per host block here, not
    // per control block or per sample (pure add/mul, no transcendental --
    // R12 would be satisfied either way, but there is no reason to redo this
    // more than once per block). s = Spread/100; chain 0: gL=0.5+0.5s,
    // gR=0.5-0.5s; chain 1: gL=0.5-0.5s, gR=0.5+0.5s. Deliberately written as
    // PLAIN 0.5 +/- (0.5*s) -- never as `1.0 - gR` or any other reconstruction
    // -- so that `0.5*x + 0.5*x == x` bit-exactly (IEEE-754: halving and
    // doubling are both exact) at s==0, which is what DESIGN.md §9's/
    // docs/GATES.md G8.2's mono/stereo bit-identity property is built on, and
    // so that gL+gR==1.0 EXACTLY at every spread (G8.6 -- verified over a
    // fine grid of spread values as part of this gate's own testing, not
    // merely at the three named checkpoints 0/50/100).
    // G8.9 R11 FINDING: this used to branch on `s.stereoMode` (spread law
    // when on, hardcoded mono law {1,1,0,0} when off) -- but
    // SynthCore::controlRateUpdate()'s mono<->stereo BLEND (mStereoBlend,
    // mEffPanGainL0/R0/L1/R1) needs `s.panGainL0/R0/L1/R1` to always hold
    // the TRUE SPREAD-LAW TARGET, not a value that has ALREADY collapsed to
    // the mono law the instant `stereoMode` goes false -- otherwise, on a
    // mid-note kStereoMode=off toggle, the blend's own "target" (this
    // snapshot's pan gains) becomes indistinguishable from its "mono"
    // starting point on the VERY FIRST control block after the toggle, and
    // no actual ramp ever happens (found by measurement: an earlier draft
    // of this fix left G8.9's on->off direction at an UNCHANGED ~3.3dB/ms,
    // identical to before the blend existed, because both sides of that
    // blend were silently the same value). Always compute the spread law
    // here; `mStereoBlend`==0.0 EXACTLY in steady mono mode is what zeroes
    // its contribution out (0.0*x==0.0 for any finite x), which is what
    // keeps G8.1's mono bit-identity unaffected by this change.
    const double spread01 = std::clamp(static_cast<double>(s.stereoSpreadPercent), 0.0, 100.0) * 0.01;
    const double halfSpread = 0.5 * spread01;  // exact: multiplying by 0.5 never rounds
    s.panGainL0 = 0.5 + halfSpread;
    s.panGainR0 = 0.5 - halfSpread;
    s.panGainL1 = 0.5 - halfSpread;
    s.panGainR1 = 0.5 + halfSpread;

    // [dsp] G8.9: mStereoBlend's control-rate one-pole coefficient -- a
    // PLAIN exponential approach (divisor 1.0, i.e. AdsrEnv::coeffForMs's
    // own `tau = ms/divisor` collapses to `tau = kStereoBlendMs` exactly),
    // not one of AdsrEnv's own overshoot-target-calibrated divisors (those
    // are for the ENVELOPE's specific 1.15/100x/1.05-0.05 crossing
    // conventions, DESIGN.md §6 -- this is a generic "reach the target
    // asymptotically" ramp, no overshoot semantics at all).
    s.stereoBlendCoeff = AdsrEnv::coeffForMs(kStereoBlendMs, 1.0, fsControl);

    // [dsp] G12/DESIGN.md §13: the chorus LFO's control-step increment and
    // the one-pole coefficient its centre/depth glide on when the mode
    // changes between two LIVE positions. Both are single-instrument-wide
    // derived constants of a single param, and both are things the control
    // rate must never compute (a divide is merely wasteful there; the
    // exp() inside coeffForMs is an outright R12 violation) -- so, like
    // glideStepCoeff and stereoBlendCoeff above, they are computed once per
    // host block here. The rate comes from JunoChorus's own per-mode table
    // (synth_chorus.h), which is the single source of truth for it; the
    // index is clamped there too, so a corrupt stored value cannot read off
    // the end of the table on either side of this boundary.
    const int chorusModeIdx = std::clamp(s.chorusMode, 0, JunoChorus::kNumModes - 1);
    s.chorusLfoIncrement = JunoChorus::kRateHz[chorusModeIdx] / std::max(fsControl, 1e-6);
    // Same "generic asymptotic approach, divisor 1.0" use of coeffForMs as
    // stereoBlendCoeff just above -- not an envelope-segment coefficient.
    s.chorusGlideCoeff = AdsrEnv::coeffForMs(JunoChorus::kGlideMs, 1.0, fsControl);
}

// ===== G7: event application (DESIGN.md §10.1/§10.3-§10.6) =====

void SynthCore::applyEvent(const NoteEvent& ev, const ParamSnapshot& snapshot, double fs) {
    switch (ev.type) {
        case NoteEvent::NoteOn:
            handleNoteOn(ev.note, ev.value, snapshot, fs);
            break;
        case NoteEvent::NoteOff:
            handleNoteOff(ev.note, snapshot);
            break;
        case NoteEvent::PitchBend:
            // DESIGN.md §3.2/§10.6/G7.9: bend applies to EVERY sounding
            // voice, not just new ones -- stored once here and read by
            // EVERY active voice's pitch recompute in controlRateUpdate(),
            // exactly like mLastLfoValue's own "one shared value, read by
            // every voice" pattern (DESIGN.md §7 [PERF-4]).
            mBendSemis = static_cast<double>(ev.value) * static_cast<double>(snapshot.bendRangeSemitones);
            break;
        case NoteEvent::Sustain:
            handleSustain(ev.value >= 0.5f);
            break;
        case NoteEvent::AllNotesOff:
            handleAllNotesOff();
            break;
        case NoteEvent::AllSoundOff:
            handleAllSoundOff(fs);
            break;
        default:
            break;
    }
}

void SynthCore::setVoiceState(int mainPoolIndex, nassau_alloc::SlotState newState) {
    Voice& v = mVoices[mainPoolIndex];
    const bool wasIdle = (v.state == nassau_alloc::SlotState::Idle);
    const bool willBeIdle = (newState == nassau_alloc::SlotState::Idle);
    v.state = newState;
    if (wasIdle && !willBeIdle) {
        ++mHeldVoiceCount;
        if (mHeldVoiceCount == 1) mLfo.noteOnEdge();  // DESIGN.md §7: only 0->1
    } else if (!wasIdle && willBeIdle) {
        --mHeldVoiceCount;
    }
}

void SynthCore::stealToFadeSlot(int mainPoolIndex, double fs) {
    // DESIGN.md §10.4: "moved into one of two dedicated fade-out slots,
    // where it continues to render with a 2ms linear fade to zero... Two
    // slots is enough... if [a third steal within 2ms] happens the oldest
    // fade slot is simply overwritten." Alternating 0/1 on every steal
    // guarantees the slot picked is always the one used longest ago, with
    // only 2 slots to track (no timestamp comparison needed).
    const int fadeIdx = kMaxVoices + mNextFadeSlot;
    mNextFadeSlot = 1 - mNextFadeSlot;

    Voice& dst = mVoices[fadeIdx];
    dst = mVoices[mainPoolIndex];  // plain struct copy (Voice is POD-shaped, R3: not an allocation)

    // [voicing] DESIGN.md §10.4: "a 2ms linear fade to zero" -- an
    // engineering choice, not a measured hardware figure (mirrors
    // kLpfCrossfadeSeconds's own [voicing] tag for the same reason).
    constexpr double kFadeSeconds = 0.002;
    const int total = std::max(1, static_cast<int>(std::lround(kFadeSeconds * fs)));
    dst.fadeActive = true;
    dst.fadeSamplesTotal = total;
    dst.fadeSamplesElapsed = 0;
    dst.fadeGainCur = 1.0;
    dst.fadeGainStep = -1.0 / static_cast<double>(total);
    dst.curBlockPeakAccum = 0.0;
    dst.peakPrevBlock = 0.0;
}

void SynthCore::hardRetrigger(int mainPoolIndex, int note, float vel, const ParamSnapshot& snapshot,
                               double unisonDetuneCentsVal, bool freshEnvelope) {
    Voice& v = mVoices[mainPoolIndex];
    // `freshEnvelope` (Idle-alloc, post-steal, or every Unison voice, since
    // Unison always retriggers, DESIGN.md §10.5) resets envF/envA/
    // vcaGainStart/vcaGainEnd to a clean 0 before noteOn(). Without this, a
    // STOLEN slot's leftover envelope/vcaGain values (the stolen SIGNAL
    // itself lives on, unaffected, in its own fade slot; only this
    // physical slot's bookkeeping is being repurposed) would let a freshly
    // phase-reset (DESIGN.md §3.2), cold oscillator suddenly appear at
    // whatever gain the UNRELATED stolen note happened to be at -- found by
    // measurement (G7.3's own click test): an envelope delta of 6.8dB/ms
    // and a 0.47 sample-to-sample jump, both over bound, traced to exactly
    // this. The ONE caller that passes false (Poly's "retrigger a note
    // already sounding ON THIS SAME SLOT", G7.5, no steal involved) is
    // exactly the case AdsrEnv::noteOn()'s own "no reset of y" click-free
    // continuity is FOR, and is deliberately left unreset.
    v.note = note;
    // DESIGN.md §3.2: "both oscillators reset to 0" at note-on -- PHASE
    // only (resetPhase()), not the full reset() SynthCore::reset()/init()
    // use (G3's own established choice, kept here unchanged: filter/DC-
    // blocker state is deliberately left running across a note-on, exactly
    // like a real analogue voice card's own capacitors do not discharge
    // between notes).
    // G8 (DESIGN.md §9): BOTH chains' oscillators reset unconditionally,
    // regardless of whether kStereoMode is currently on. Chain 1 is only
    // ever STEPPED in the audio-rate loop while stereo mode is on, but
    // resetting it here too means it is always at a clean, deterministic
    // phase=0 the instant stereo mode is toggled on for an already-sounding
    // voice -- never a stale phase left over from whatever it was doing (or
    // not doing) the last time it ran. Cheap: this runs once per note-on,
    // not per sample.
    for (int c = 0; c < Voice::kNumChains; ++c) {
        v.chain[c].osc1.resetPhase();
        v.chain[c].osc2.resetPhase();
        v.chain[c].sub.reset();  // keep the sub's wrap-index counter consistent with osc1's fresh phase=0
    }
    // Noise is deliberately NOT reset at note-on (only reset()/init()
    // reseed it) -- a real analogue noise source runs continuously;
    // DESIGN.md/R13 only require voice-index seeding at init()/reset().
    if (freshEnvelope) {
        v.envF.reset();
        v.envA.reset();
        v.vcaGainStart = 0.0;
        v.vcaGainEnd = 0.0;
    }
    v.envF.noteOn();
    v.envA.noteOn();
    setVoiceState(mainPoolIndex, nassau_alloc::SlotState::Playing);
    v.startedAt = ++mVoiceClock;

    // DESIGN.md §10.6: glide is a SMOOTHED pitch target -- a fresh strike
    // (fresh-Idle allocation, post-steal reuse, or a Unison retrigger) has
    // nothing to glide FROM (that would be an unrelated previous note's
    // pitch bleeding into this new one), so it snaps immediately. Only
    // legatoRetargetVoice() (Mono legato) leaves this to actually glide.
    v.glideTargetSemis = static_cast<double>(note);
    v.glideCurrentSemis = v.glideTargetSemis;

    // DESIGN.md §11 kVelToVca/kVelToFilter (G7.11). At 100%: vel=0.5 gives
    // a VCA peak 6.02dB below vel=1.0 -- exactly linear vel->gain, since
    // 20*log10(0.5) == -6.02dB -- and lowers the filter corner by one
    // octave. At 0%: velocity has NO effect, for either destination. Both
    // formulas below hit vel==1.0 -> {gain=1.0, octOffset=0.0} EXACTLY,
    // regardless of the percent setting (1.0 - frac*(1.0-1.0) == 1.0;
    // 2.0*(1.0-1.0)*frac == 0.0) -- which is what keeps every existing
    // vel==1.0f test/golden case in this codebase bit-exact (G7 gate
    // report).
    const double velToVcaFrac = static_cast<double>(snapshot.velToVcaPercent) * 0.01;
    v.velVcaGain = 1.0 - velToVcaFrac * (1.0 - static_cast<double>(vel));
    const double velToFilterFrac = static_cast<double>(snapshot.velToFilterPercent) * 0.01;
    v.velFilterOct = 2.0 * (static_cast<double>(vel) - 1.0) * velToFilterFrac;

    v.unisonDetuneCents = unisonDetuneCentsVal;
}

void SynthCore::legatoRetargetVoice(Voice& v, int note, float vel, const ParamSnapshot& snapshot) {
    // DESIGN.md §10.5 Mono: "legato -- no envelope retrigger while a key is
    // still held". Deliberately does NOT touch envF/envA/oscillator phase/
    // state/startedAt, and deliberately does NOT snap glideCurrentSemis --
    // this is the one path where a real audible glide happens.
    v.note = note;
    v.glideTargetSemis = static_cast<double>(note);
    const double velToVcaFrac = static_cast<double>(snapshot.velToVcaPercent) * 0.01;
    v.velVcaGain = 1.0 - velToVcaFrac * (1.0 - static_cast<double>(vel));
    const double velToFilterFrac = static_cast<double>(snapshot.velToFilterPercent) * 0.01;
    v.velFilterOct = 2.0 * (static_cast<double>(vel) - 1.0) * velToFilterFrac;
}

void SynthCore::releaseVoiceOrHold(int mainPoolIndex) {
    Voice& v = mVoices[mainPoolIndex];
    if (mSustainHeld) {
        // DESIGN.md §10.3: "Note-off with sustain (CC 64) held moves the
        // voice to a Held state; it releases when the pedal lifts." The
        // envelope is deliberately NOT told noteOff() here -- it keeps
        // sounding exactly as if the key were still down.
        setVoiceState(mainPoolIndex, nassau_alloc::SlotState::Held);
    } else {
        v.envF.noteOff();
        v.envA.noteOff();
        setVoiceState(mainPoolIndex, nassau_alloc::SlotState::Released);
        v.releasedAt = ++mVoiceClock;
    }
}

void SynthCore::pushMonoNote(int note, float vel) {
    if (mMonoStackCount >= kMaxMonoStack) return;  // [voicing] bounded silent drop, R3 (see header comment)
    mMonoNoteStack[mMonoStackCount] = note;
    mMonoVelStack[mMonoStackCount] = vel;
    ++mMonoStackCount;
}

void SynthCore::popMonoNote(int note) {
    for (int i = mMonoStackCount - 1; i >= 0; --i) {
        if (mMonoNoteStack[i] == note) {
            for (int j = i; j < mMonoStackCount - 1; ++j) {
                mMonoNoteStack[j] = mMonoNoteStack[j + 1];
                mMonoVelStack[j] = mMonoVelStack[j + 1];
            }
            --mMonoStackCount;
            return;
        }
    }
}

void SynthCore::handleNoteOn(int note, float vel, const ParamSnapshot& snapshot, double fs) {
    const VoiceMode mode = static_cast<VoiceMode>(snapshot.voiceMode);
    const int poly = nassau_alloc::polyphonyVoiceCount(snapshot.polyphony);

    // ---- Mono: DESIGN.md §10.5, always physical voice 0 ----
    if (mode == VoiceMode::Mono) {
        const bool wasHeldBefore = (mMonoStackCount > 0);
        pushMonoNote(note, vel);
        Voice& v = mVoices[0];
        if (!wasHeldBefore) {
            if (v.state != nassau_alloc::SlotState::Idle) stealToFadeSlot(0, fs);
            hardRetrigger(0, note, vel, snapshot, 0.0, /*freshEnvelope=*/true);
        } else {
            legatoRetargetVoice(v, note, vel, snapshot);  // legato: no retrigger, glide continues
        }
        return;
    }

    // ---- Unison: DESIGN.md §10.5, all `poly` voices retrigger together ----
    if (mode == VoiceMode::Unison) {
        for (int i = 0; i < poly; ++i) {
            if (mVoices[i].state != nassau_alloc::SlotState::Idle) stealToFadeSlot(i, fs);
            const double detune =
                nassau_alloc::unisonDetuneCentsFor(i, poly, static_cast<double>(snapshot.stereoDetuneCents));
            hardRetrigger(i, note, vel, snapshot, detune, /*freshEnvelope=*/true);
        }
        mUnisonActive = true;
        mUnisonNote = note;
        return;
    }

    // ---- Poly: DESIGN.md §10.3 ----
    // G7.5: retriggering a note already sounding on this exact pitch reuses
    // that voice rather than allocating a second.
    for (int i = 0; i < poly; ++i) {
        if (mVoices[i].state != nassau_alloc::SlotState::Idle && mVoices[i].note == note) {
            hardRetrigger(i, note, vel, snapshot, 0.0, /*freshEnvelope=*/false);  // G7.5: in-place, click-free continuity
            return;
        }
    }

    // G7.2: Idle first, then oldest Released, then oldest Playing/Held
    // (nassau_alloc::chooseVoiceForSteal, synth_alloc.h). `infos` is a
    // small fixed on-stack array (R3: not an allocation).
    nassau_alloc::SlotInfo infos[kMaxVoices];
    for (int i = 0; i < poly; ++i) {
        infos[i].state = mVoices[i].state;
        infos[i].startedAt = mVoices[i].startedAt;
        infos[i].releasedAt = mVoices[i].releasedAt;
    }
    const int idx = nassau_alloc::chooseVoiceForSteal(infos, poly);
    if (idx < 0) return;  // defensive; poly >= 4 always (DESIGN.md §11), never reached
    if (mVoices[idx].state != nassau_alloc::SlotState::Idle) stealToFadeSlot(idx, fs);
    hardRetrigger(idx, note, vel, snapshot, 0.0, /*freshEnvelope=*/true);
}

void SynthCore::handleNoteOff(int note, const ParamSnapshot& snapshot) {
    const VoiceMode mode = static_cast<VoiceMode>(snapshot.voiceMode);

    if (mode == VoiceMode::Mono) {
        popMonoNote(note);
        Voice& v = mVoices[0];
        if (mMonoStackCount > 0) {
            // DESIGN.md §10.5 last-note-priority: a key is STILL held (the
            // stack isn't empty), so this is a legato retarget back to that
            // older note, not a release.
            const int fallbackNote = mMonoNoteStack[mMonoStackCount - 1];
            const float fallbackVel = mMonoVelStack[mMonoStackCount - 1];
            legatoRetargetVoice(v, fallbackNote, fallbackVel, snapshot);
        } else if (v.state != nassau_alloc::SlotState::Idle) {
            releaseVoiceOrHold(0);
        }
        return;
    }

    if (mode == VoiceMode::Unison) {
        if (mUnisonActive && mUnisonNote == note) {
            const int poly = nassau_alloc::polyphonyVoiceCount(snapshot.polyphony);
            for (int i = 0; i < poly; ++i) {
                if (mVoices[i].state != nassau_alloc::SlotState::Idle) releaseVoiceOrHold(i);
            }
            mUnisonActive = false;
        }
        return;
    }

    // ---- Poly ----
    const int poly = nassau_alloc::polyphonyVoiceCount(snapshot.polyphony);
    for (int i = 0; i < poly; ++i) {
        if (mVoices[i].state == nassau_alloc::SlotState::Playing && mVoices[i].note == note) {
            releaseVoiceOrHold(i);
            break;  // first match only, matches G7.2's own single-voice-per-note invariant
        }
    }
}

void SynthCore::handleSustain(bool down) {
    const bool wasDown = mSustainHeld;
    mSustainHeld = down;
    if (wasDown && !down) {
        // DESIGN.md §10.3: "CC 64 off -> all Held voices release together"
        // (G7.6). Held voices only ever exist in the main pool.
        for (int i = 0; i < kMaxVoices; ++i) {
            if (mVoices[i].state == nassau_alloc::SlotState::Held) {
                mVoices[i].envF.noteOff();
                mVoices[i].envA.noteOff();
                setVoiceState(i, nassau_alloc::SlotState::Released);
                mVoices[i].releasedAt = ++mVoiceClock;
            }
        }
    }
}

void SynthCore::handleAllNotesOff() {
    // DESIGN.md §10.3: "CC 123 (all notes off) releases every voice
    // normally" -- an ordinary envelope Release, unconditionally (a panic/
    // all-off command bypasses the sustain pedal's Held detour, unlike an
    // individual NoteOff).
    for (int i = 0; i < kMaxVoices; ++i) {
        if (mVoices[i].state != nassau_alloc::SlotState::Idle) {
            mVoices[i].envF.noteOff();
            mVoices[i].envA.noteOff();
            setVoiceState(i, nassau_alloc::SlotState::Released);
            mVoices[i].releasedAt = ++mVoiceClock;
        }
    }
    mMonoStackCount = 0;
    mUnisonActive = false;
}

void SynthCore::handleAllSoundOff(double fs) {
    // DESIGN.md §10.3/§10.4: "CC 120 (all sound off) routes every voice
    // through the fade-out slots" -- every currently-sounding voice is
    // stolen (click-free, DESIGN.md §10.4) and its physical slot freed
    // immediately, rather than released normally.
    for (int i = 0; i < kMaxVoices; ++i) {
        if (mVoices[i].state != nassau_alloc::SlotState::Idle) {
            stealToFadeSlot(i, fs);
            // The STOLEN SIGNAL lives on, unaffected, in its own fade slot
            // (the copy stealToFadeSlot() just made). This main-pool slot's
            // OWN envF/envA must be reset to Idle here (not merely marked
            // Idle allocation-wise) -- unlike a normal Release, nothing
            // ever calls envA.noteOff() for a CC120-freed voice, so without
            // this its envelope would stay parked in whatever state it was
            // in (e.g. Sustain) forever, which getDebugActiveVoiceCount()
            // (DESIGN.md §10.7, driven by envA.isIdle()) would then never
            // count as silent even though state==Idle already makes this
            // slot's OWN audio-loop contribution exactly 0 -- found by
            // G7.7's own CC120 timing measurement (R11).
            mVoices[i].envF.reset();
            mVoices[i].envA.reset();
            setVoiceState(i, nassau_alloc::SlotState::Idle);
            mVoices[i].note = -1;
            mVoices[i].vcaGainStart = 0.0;
            mVoices[i].vcaGainEnd = 0.0;
        }
    }
    mMonoStackCount = 0;
    mUnisonActive = false;
}

// ===== G3: the control-rate update point (DESIGN.md §2) =====

void SynthCore::controlRateUpdate(const ParamSnapshot& snapshot, double fs) {
    // mLfo.wave/rate/delay are already configured for this host block by
    // process() itself, BEFORE any event was applied at this control-rate
    // update point (see process()'s own comment) -- only the per-control-
    // block step happens here.
    mLastLfoValue = mLfo.step(snapshot.fsControl);

    // [ref] DESIGN.md §7: pitch +-50 cents (=0.5 semitone) at 100% amount;
    // PWM +-45% at 100% amount. Shared across BOTH oscillators of EVERY
    // active voice this control block (DESIGN.md §7 [PERF-4]: one global
    // LFO) -- this is the ONE local variable G3.10's getDebugVoiceLfoPitchModSemis()
    // proves every active voice reads identically.
    const double lfoPitchModSemis = (static_cast<double>(snapshot.lfoPitchAmountPercent) * 0.01) *
                                     mLastLfoValue * 0.5;
    const double lfoPwmModPercent = (static_cast<double>(snapshot.lfoPwmAmountPercent) * 0.01) *
                                     mLastLfoValue * 45.0;

    // G4.9 (kept stable into G5, DESIGN.md §5.4's clamp): the RAW kLpfCutoff
    // clamped into [10, 0.45*fs] -- deliberately WITHOUT any of G5's
    // per-voice modulation terms (see getDebugLpfCutoff()'s own header
    // comment for why). Computed once per control block, not per sample
    // (R12). The REAL, per-voice, modulated-and-clamped fc (DESIGN.md §5.4's
    // key follow/ENV-F/LFO terms) is computed inside the per-voice loop
    // below and fed to that voice's filter structure(s)' setControlRate();
    // getDebugVoiceLpfCutoff() reads it back (G5.6).
    const double lpfFcClamped = std::clamp(static_cast<double>(snapshot.lpfCutoffHz), 10.0, 0.45 * fs);
    mDebugLpfCutoffHz = lpfFcClamped;  // G4.9 readback

    // G5 (DESIGN.md §5.1, docs/GATES.md G5.4): a slope switch is a SINGLE
    // instrument-wide event (kLpfSlope, DESIGN.md §11), so detecting it and
    // timing the resulting 20ms crossfade both happen ONCE here, shared by
    // every voice -- only the per-voice SEEDING of the incoming structure's
    // state (below, inside the per-voice loop, from that voice's own most
    // recent LPF output) differs voice to voice. If a crossfade is ALREADY
    // running and the param changes again before it finishes, this
    // deliberately lets the current one finish first rather than compounding
    // two crossfades -- DESIGN.md §5.1 calls a slope switch "a rare,
    // deliberate gesture" and no G5 AC exercises a mid-crossfade re-toggle;
    // flagged as a decision the plan did not name (R11).
    const bool startingLpfCrossfade = !mLpfCrossfadeActive && snapshot.lpfSlope != mLpfSlopeSettled;
    if (startingLpfCrossfade) {
        mLpfCrossfadeFromSlope = mLpfSlopeSettled;
        mLpfCrossfadeToSlope = snapshot.lpfSlope;
        // [voicing] DESIGN.md §5.1: 20ms, see kLpfCrossfadeSeconds's own comment.
        mLpfCrossfadeSamplesTotal = std::max(1, static_cast<int>(std::lround(kLpfCrossfadeSeconds * fs)));
        mLpfCrossfadeSamplesElapsed = 0;
        mLpfCrossfadeActive = true;
    }
    const double lpfResonancePercent = static_cast<double>(snapshot.lpfResonancePercent);

    // G6 (DESIGN.md §5.5): kHpfSlope/kHpfCutoff are single instrument-wide
    // params (DESIGN.md §11), so -- exactly like the LPF slope-settle logic
    // above -- whether the HPF is bypassed and how many poles run are each
    // decided ONCE here, shared by every voice, not per voice. Compared
    // against the RAW hpfCutoffHz (not key-follow-modulated): DESIGN.md
    // §5.5's bypass is "the leftmost position of a real instrument's
    // high-pass switch", a knob position, independent of what note is
    // playing or how much key follow is dialled in.
    mHpfBypassed = snapshot.hpfCutoffHz <= kHpfCutoffMinHz;
    mHpfNumPoles = (snapshot.hpfSlope == static_cast<int>(HpfSlope::Db24)) ? 4 : 2;

    // G8.9 (DESIGN.md §9, synth_core.h's own field comments): detect a
    // GENUINE mid-note kStereoMode toggle and retarget the mono<->stereo
    // pan-law blend, ONCE per control block, shared by every voice --
    // exactly like the LPF slope crossfade's own startingLpfCrossfade
    // detection above. `mControlRateEverRun` distinguishes "stereo mode was
    // already configured before the first process() call" (snap instantly,
    // no ramp -- required for G8.2/G8.4/G8.6's bit-exactness) from an
    // actual runtime toggle. `mStereoBlend` itself is a control-rate
    // one-pole (see its own field comment for the exponential-vs-linear
    // reasoning): it keeps stepping toward WHATEVER `mChain1Active` says
    // right now every block, so re-toggling mid-ramp simply retargets it
    // from its current position, no special-casing needed.
    if (!mControlRateEverRun) {
        mControlRateEverRun = true;
        mChain1Active = snapshot.stereoMode;
        mStereoBlend = mChain1Active ? 1.0 : 0.0;
    } else {
        mChain1Active = snapshot.stereoMode;
        const double target = mChain1Active ? 1.0 : 0.0;
        mStereoBlend += snapshot.stereoBlendCoeff * (target - mStereoBlend);
    }
    // The EFFECTIVE pan gains the audio-rate loop actually multiplies by
    // (process(), G8.9): interpolate between the MONO law {1,1,0,0} and the
    // snapshot's own exact TARGET stereo law -- bit-exact identity at
    // mStereoBlend==0.0 or ==1.0 (x*1.0==x, x+0.0==x), see mStereoBlend's
    // own field comment for the full argument.
    mEffPanGainL0 = (1.0 - mStereoBlend) * 1.0 + mStereoBlend * snapshot.panGainL0;
    mEffPanGainR0 = (1.0 - mStereoBlend) * 1.0 + mStereoBlend * snapshot.panGainR0;
    mEffPanGainL1 = (1.0 - mStereoBlend) * 0.0 + mStereoBlend * snapshot.panGainL1;
    mEffPanGainR1 = (1.0 - mStereoBlend) * 0.0 + mStereoBlend * snapshot.panGainR1;

    // G12 (DESIGN.md §13): the output-stage chorus is ONE instrument-wide
    // block sitting on the summed accumulator, so -- exactly like the LPF
    // slope crossfade and the stereo blend above -- its whole control-rate
    // step happens once here, outside the per-voice loop. It advances its
    // own triangle LFO one control step and republishes the two per-channel
    // delay endpoints the audio-rate loop interpolates between; everything
    // needing a divide or an exp() was already derived at block rate
    // (finishSnapshot()) and is merely passed in (R12).
    mChorus.setControlRate(snapshot.chorusMode, snapshot.chorusLfoIncrement,
                            snapshot.chorusGlideCoeff);

    // Chain 1 needs a full control-rate prep (pitch, filter coefficients)
    // whenever it is either the logical target OR the blend has not yet
    // fully settled back to 0 after a mid-note "off" toggle -- SAME
    // predicate the audio-rate loop uses (below, process()) to decide
    // whether to STEP it this block.
    const bool chain1NeededThisBlock = mChain1Active || mStereoBlend > 0.0;

    int activeCount = 0;  // G7.14/PERF-7: recomputed fresh every control block, see the loop body below
    for (int i = 0; i < kMaxVoices + kNumFadeSlots; ++i) {
        Voice& v = mVoices[i];
        const bool isMainPool = i < kMaxVoices;

        v.envF.aCoeff = snapshot.envFAttackCoeff;
        v.envF.dCoeff = snapshot.envFDecayCoeff;
        v.envF.rCoeff = snapshot.envFReleaseCoeff;
        v.envF.sustainLevel = snapshot.envFSustainLevel;
        v.envF.step();

        v.envA.aCoeff = snapshot.envAAttackCoeff;
        v.envA.dCoeff = snapshot.envADecayCoeff;
        v.envA.rCoeff = snapshot.envAReleaseCoeff;
        v.envA.sustainLevel = snapshot.envASustainLevel;
        v.envA.step();

        // G7: a MAIN-POOL voice's natural release-to-silence transition
        // (unchanged from G3-G6: ENV-A reaching Idle) retires it from the
        // allocator's Playing/Held/Released bookkeeping. A FADE SLOT's
        // envelope reaching Idle mid-fade means nothing to the allocator --
        // it was never counted in mHeldVoiceCount and is retired purely by
        // its own sample counter (the audio-rate loop, DESIGN.md §10.4).
        if (isMainPool && v.state != nassau_alloc::SlotState::Idle && v.envA.isIdle()) {
            setVoiceState(i, nassau_alloc::SlotState::Idle);
            v.note = -1;
        }

        v.vcaGainStart = v.vcaGainEnd;
        // G7.11 (DESIGN.md §11 kVelToVca): velVcaGain==1.0 exactly at
        // vel==1.0 regardless of the percent setting (hardRetrigger()'s own
        // comment) -- x*1.0 is a bit-exact IEEE-754 identity, so this does
        // not perturb any existing vel==1.0f test/golden case.
        v.vcaGainEnd = mDebugForceUnityVca ? 1.0 : (v.envA.y * v.velVcaGain);  // G3.5: unity-gain reference render

        // G11.11(b): `mDebugDisableVcaInterpolation` (G3.5) used to be a
        // per-VOICE, per-SAMPLE branch inside the audio-rate loop ("hold
        // vcaGainEnd for the whole control block instead of interpolating").
        // The flag is a plain (non-atomic) member that cannot change mid-
        // process() call (its own setter's doc comment: "not safe to call
        // concurrently with process()"), so branching on it once per sample
        // per voice was pure overhead -- exactly the kind of thing R12 does
        // NOT catch (it is neither a transcendental nor an atomic load,
        // docs/GATES.md G11.11's own note). Hoisted out to HERE, the one
        // place vcaGainStart/vcaGainEnd are set per control block: forcing
        // vcaGainStart equal to vcaGainEnd makes the audio-rate lerp
        // `vcaGainStart + (vcaGainEnd-vcaGainStart)*frac` collapse to
        // `vcaGainEnd + 0.0*frac == vcaGainEnd` bit-exactly (IEEE-754: a
        // zero product and a zero addend are both exact) for every sample of
        // this control block -- precisely "hold vcaGainEnd for the whole
        // control block", with no branch left in the audio-rate loop at all.
        if (mDebugDisableVcaInterpolation) v.vcaGainStart = v.vcaGainEnd;

        // G7/DESIGN.md §10.7 [PERF-7]: fold this control block's
        // accumulated peak into `peakPrevBlock` (the quantity the AC names,
        // "peak over the PREVIOUS control block") and reset the
        // accumulator for the block about to render. Done for every slot
        // (main pool AND fade), even one about to be skipped below, so a
        // freshly-idled voice's peak reading is never stale.
        v.peakPrevBlock = v.curBlockPeakAccum;
        v.curBlockPeakAccum = 0.0;

        if (isMainPool) {
            // DESIGN.md §10.7's exact predicate: skippable iff ENV-A Idle
            // AND last block's peak was below -100dBFS.
            const bool silentSkippable = v.envA.isIdle() && v.peakPrevBlock < kSilentSkipThreshold;
            if (!silentSkippable) ++activeCount;
        }

        // Idle voices (main pool) / fully-faded slots (fade pool) don't
        // need pitch/PW/filter-coefficient recompute (nor rendering, see
        // process()'s own skip condition, which uses exactly this same
        // isMainPool ? state==Idle : !fadeActive test).
        if (isMainPool ? (v.state == nassau_alloc::SlotState::Idle) : !v.fadeActive) continue;

        // DESIGN.md §10.6: advance this voice's glide one control step
        // toward its target (snapshot.glideStepCoeff==1.0 exactly at
        // kGlideTime==0, per finishSnapshot()'s own comment -- reaches the
        // target on the very first step, i.e. "instantaneous (first control
        // block)", G7.10).
        v.glideCurrentSemis += snapshot.glideStepCoeff * (v.glideTargetSemis - v.glideCurrentSemis);

        v.debugLfoPitchModSemis = lfoPitchModSemis;

        // G8 (DESIGN.md §9 [PERF-5]): number of chains genuinely prepared
        // this control block -- SAME predicate as the audio-rate loop's own
        // `numChains` (process()), so a chain that is not stepped in the
        // audio-rate loop is not given stale/wasted control-rate work either
        // (DESIGN.md's own "run only when kStereoMode is on"). G8.9:
        // `chain1NeededThisBlock` (not the raw `snapshot.stereoMode`) is what
        // keeps chain 1 getting real pitch/filter updates throughout its 2ms
        // fade-out tail after a mid-note kStereoMode=off toggle, instead of
        // freezing mid-ramp.
        const int numChains = chain1NeededThisBlock ? 2 : 1;

        // [voicing] kOsc2KeyTrack off -> VCO2 ignores the played note and
        // sits at a fixed reference pitch (note 60, middle C) instead --
        // DESIGN.md §11 names this param but does not spell out its
        // behaviour; this is the conventional Prophet/Jupiter-family reading
        // of an oscillator "key track" switch (a fixed-pitch drone/FM-
        // operator use case), and no G3 AC exercises it either way. Flagged
        // per R11 as a decision the plan did not cover. SHARED across chains
        // (DESIGN.md §9 [PERF-5]: glide is one of the shared terms) --
        // computed ONCE, outside the per-chain loop below.
        const double noteForOsc2 = snapshot.osc2KeyTrack ? v.glideCurrentSemis : 60.0;

        // [ref] DESIGN.md §8 [PERF-6]: Poly-Mod, control-rate only.
        // kPmEnvFToOsc2 -- ENV-F to VCO2 pitch, bipolar, +/-6 semitones at
        // 100%; kPmEnvFToPw -- ENV-F to BOTH oscillators' pulse width,
        // bipolar, +/-45% at 100%. `v.envF.y` is this SAME control step's
        // freshly-advanced ENV-F value (v.envF.step() ran a few lines above,
        // matching the way lfoPitchModSemis/lfoPwmModPercent above already
        // reuse this control block's freshly-stepped LFO value). SHARED
        // across chains ([PERF-5]: ENV-F is one Voice-level field) --
        // computed ONCE, outside the per-chain loop below.
        const double pmOsc2Semis =
            (static_cast<double>(snapshot.pmEnvFToOsc2Percent) * 0.01) * kPmEnvFToOsc2Semitones * v.envF.y;
        const double pmPwModPercent =
            (static_cast<double>(snapshot.pmEnvFToPwPercent) * 0.01) * 45.0 * v.envF.y;

        // G8 (DESIGN.md §5.4/§9 [PERF-5]): the LPF's modulated+clamped
        // cutoff (key follow, ENV-F, LFO, velocity -- all SHARED terms) is
        // computed ONCE PER VOICE here, not once per chain, and the SAME
        // `lpfFcVoice` value below is fed to EVERY chain's own filter
        // instance's setControlRate() call -- this is exactly what
        // docs/GATES.md G8.5 asserts ("both chains' filter cutoffs are
        // identical at every control block") and is stored in the single,
        // un-indexed `v.debugLpfCutoffHz` field (Voice, not Voice::Chain).
        // [dsp] DESIGN.md §5.4, folded into ONE exp2 call per §2.1 ("write
        // them as one exp2 of a summed octave offset per filter, not one
        // per term"): key follow (referred to note 60), ENV-F (bipolar,
        // +/-6 octaves at 100%), LFO (+/-2 octaves at 100%), then the
        // [10, 0.45*fs] robustness clamp (tan(pi*fc/fs) diverges at
        // Nyquist, same clamp as the unmodulated lpfFcClamped above).
        const double lpfKeyFollowOct = (static_cast<double>(snapshot.lpfKeyFollowPercent) * 0.01) *
                                        (v.glideCurrentSemis - 60.0) / 12.0;
        const double lpfEnvOct =
            6.0 * (static_cast<double>(snapshot.lpfEnvAmountPercent) * 0.01) * v.envF.y;
        const double lpfLfoOct =
            2.0 * (static_cast<double>(snapshot.lpfLfoAmountPercent) * 0.01) * mLastLfoValue;
        // G7.11 (DESIGN.md §11 kVelToFilter): velFilterOct==0.0 exactly at
        // vel==1.0 regardless of the percent setting (hardRetrigger()'s own
        // comment) -- appended exactly like osc1Semis/osc2Semis's own bend/
        // unison terms below, for the identical bit-exactness reason.
        const double lpfFcRaw = static_cast<double>(snapshot.lpfCutoffHz) *
                                 std::exp2(lpfKeyFollowOct + lpfEnvOct + lpfLfoOct + v.velFilterOct);
        const double lpfFcVoice = std::clamp(lpfFcRaw, 10.0, 0.45 * fs);
        v.debugLpfCutoffHz = lpfFcVoice;  // G5.6/G8.5 readback -- ONE value, not one per chain

        // G8 (DESIGN.md §5.5/§9 [PERF-5]): same "computed once per voice"
        // treatment for the HPF's modulated+clamped cutoff -- fc_hp =
        // clamp(HpfCutoff * exp2(HpfKeyFollow/100 * (note-60)/12), 10,
        // 0.45*fs). Only computed (and fed to each chain's own HpfCascade)
        // when the HPF is NOT bypassed this control block.
        double hpfFcVoice = 0.0;
        if (!mHpfBypassed) {
            const double hpfKeyFollowOct = (static_cast<double>(snapshot.hpfKeyFollowPercent) * 0.01) *
                                            (v.glideCurrentSemis - 60.0) / 12.0;
            const double hpfFcRaw = static_cast<double>(snapshot.hpfCutoffHz) * std::exp2(hpfKeyFollowOct);
            hpfFcVoice = std::clamp(hpfFcRaw, 10.0, 0.45 * fs);
            v.debugHpfCutoffHz = hpfFcVoice;  // G6.5/G8.5 readback -- ONE value, not one per chain
        } else {
            v.debugHpfCutoffHz = 0.0;  // G6.5: 0 while bypassed, matches this class's convention
        }

        // DESIGN.md §2: gLpf (the coefficient `g` itself) is linearly
        // interpolated PER SAMPLE across the control block -- one of
        // exactly three such quantities, alongside vcaGain (already
        // interpolated since G3) and gHpf (G6). `filter.g` going into
        // setControlRate() is wherever the PREVIOUS block's interpolation
        // left it (the ramp's start); setControlRate() below computes this
        // block's TARGET g (and the control-rate-only k/Reff resonance
        // terms, unchanged), and the per-sample step is derived from the
        // two. runLpfStructure() (synth_core.h) applies the interpolated
        // value via advanceCoeff() every sample (an audio-rate division,
        // deliberately, see synth_filter.h's own G5 comment) and advances
        // it by this step afterward. `lpfFcVoice`/`lpfResonancePercent` are
        // the SAME (voice-level, not chain-level) values for every chain
        // this lambda is called with, matching G8.5.
        // `gCur`/`gStep` are `nassau_real` (DESIGN.md §12.2), matching
        // Voice::Chain::gLpfLadderCur/Step's own field comment: `gStart`/the
        // subtraction/division below stay `double` (this whole lambda runs
        // once per control block, not per sample -- R12 does not gate it),
        // narrowed once into `gStep`/`gCur` on store, same convention as
        // LadderFilter::setControlRate() itself.
        auto prepGInterp = [&](auto& filter, nassau_real& gCur, nassau_real& gStep) {
            const double gStart = static_cast<double>(filter.g);
            filter.setControlRate(lpfFcVoice, fs, lpfResonancePercent);
            gStep = static_cast<nassau_real>(
                (static_cast<double>(filter.g) - gStart) / static_cast<double>(std::max(1, mControlBlock)));
            gCur = static_cast<nassau_real>(gStart);
        };

        // G8 (DESIGN.md §9): the per-CHAIN loop -- everything that genuinely
        // differs between chain 0 and chain 1: oscillator pitch (fine tunes
        // + stereo detune, NEGATED for chain 1), and each chain's own
        // filter/DC-blocker STATE (seeded/coefficient-prepped here from the
        // SHARED lpfFcVoice/hpfFcVoice computed once above).
        for (int c = 0; c < numChains; ++c) {
            Voice::Chain& ch = v.chain[c];

            ch.osc1.wave = static_cast<Osc::Wave>(snapshot.osc1Wave);
            ch.osc2.wave = static_cast<Osc::Wave>(snapshot.osc2Wave);
            ch.sub.octave = static_cast<SubOsc::Octave>(snapshot.subOctave);

            // DESIGN.md §9: "fine tunings NEGATED when c == 1" -- Osc1Fine,
            // Osc2Fine AND StereoDetune, all three (§9's own list). `chainSign`
            // is +1.0 for chain 0 (used AS CONFIGURED, unchanged from every
            // pre-G8 formula) and -1.0 for chain 1. The stereo-detune term is
            // ADDED to both oscillators (like Unison's own per-voice detune
            // just below it), so with Osc2Fine=+7 cents and StereoDetune=6
            // cents, chain 0's VCO2 sits at +13 cents and chain 1's at -13
            // (docs/GATES.md G8.3's own worked example) -- kStereoMode being
            // ON therefore shifts chain 0's OWN pitch too, not merely
            // introduces a second, differently-tuned chain.
            //
            // The `snapshot.stereoMode ? ... : 0.0` gate is load-bearing and
            // NOT redundant with `numChains==1` in mono mode: chain 0 ALWAYS
            // runs (mono or stereo), with chainSign==+1.0 either way -- so
            // without this explicit gate, kStereoDetune's own nonzero DEFAULT
            // (6 cents, DESIGN.md §11 param 51) would silently detune chain 0
            // even in plain mono mode, the instant G8 landed. Found by exactly
            // this: G3.9/G3.12's own pre-G8 pitch/DC ACs (mono, default
            // params) broke against a ~220 Hz carrier reading ~219.75 Hz
            // instead, the moment this gate's FIRST draft omitted the gate
            // (R11) -- restored here. WITH the gate, mono mode's osc1Semis/
            // osc2Semis are IDENTICAL (bit-exact +0.0) to every pre-G8
            // formula, which is what G8.2's bit-identity argument (and every
            // pre-G8 golden/unit test) depends on.
            const double chainSign = (c == 0) ? 1.0 : -1.0;
            const double stereoTermCents =
                snapshot.stereoMode ? chainSign * static_cast<double>(snapshot.stereoDetuneCents) : 0.0;

            // DESIGN.md §3.2: semitones = note + bend*BendRange + glide +
            // octave*12 + semi + fine/100 + lfoPitch + pmEnvFToOsc2 (VCO2 only).
            // `v.glideCurrentSemis` IS the glide-smoothed "note" term (this
            // struct field's own comment); `mBendSemis` (G7.9),
            // `v.unisonDetuneCents/100` (G7.13, Unison only, 0 otherwise) and
            // `stereoTermCents/100` (G8, stereo only, 0 otherwise) are
            // APPENDED after the original G3-G6 term order rather than
            // interleaved, and are EXACTLY 0.0 outside Unison/bend/stereo use
            // -- appending an exact +0.0 to a finite sum is a bit-exact
            // IEEE-754 identity, which is what keeps every existing G0-G7
            // test/golden case (no bend, no Unison, no stereo) bit-for-bit
            // unchanged.
            const double osc1Semis = v.glideCurrentSemis + lfoPitchModSemis +
                                      octaveOffsetSemis(static_cast<Octave>(snapshot.osc1Octave)) +
                                      chainSign * static_cast<double>(snapshot.osc1FineCents) * 0.01 +
                                      mBendSemis + v.unisonDetuneCents * 0.01 + stereoTermCents * 0.01;
            const double osc2Semis = noteForOsc2 + lfoPitchModSemis + pmOsc2Semis +
                                      octaveOffsetSemis(static_cast<Octave>(snapshot.osc2Octave)) +
                                      static_cast<double>(snapshot.osc2Semi) +
                                      chainSign * static_cast<double>(snapshot.osc2FineCents) * 0.01 +
                                      mBendSemis + v.unisonDetuneCents * 0.01 + stereoTermCents * 0.01;

            // [dsp] DESIGN.md §3.2: f = 440 * 2^((semitones-69)/12), phaseInc clamped to 0.49.
            const double f1 = 440.0 * std::exp2((osc1Semis - 69.0) / 12.0);
            const double f2 = 440.0 * std::exp2((osc2Semis - 69.0) / 12.0);
            ch.osc1.setDt(std::clamp(f1 / std::max(fs, 1.0), 0.0, 0.49));
            ch.osc2.setDt(std::clamp(f2 / std::max(fs, 1.0), 0.0, 0.49));

            ch.osc1.setPwPercent(static_cast<double>(snapshot.osc1PwPercent) + lfoPwmModPercent + pmPwModPercent);
            ch.osc2.setPwPercent(static_cast<double>(snapshot.osc2PwPercent) + lfoPwmModPercent + pmPwModPercent);

            // G5.4: THIS CHAIN's own most recent LPF output seeds the
            // incoming structure -- each chain carries a different signal,
            // so each needs its own seed (the shared crossfade TIMING above
            // is instrument-wide, this per-chain STATE seeding is not).
            if (startingLpfCrossfade) {
                if (mLpfCrossfadeToSlope == static_cast<int>(LpfSlope::Db24)) {
                    ch.lpfLadder.seedFromOutput(ch.lastLpfOutput);
                } else {
                    ch.lpfSvf.seedFromOutput(ch.lastLpfOutput);
                }
            }

            // DESIGN.md §5.1 [PERF-8]: only the currently-selected structure
            // runs a control block's worth of coefficient recompute, EXCEPT
            // during the 20ms crossfade, when BOTH must (G4's own gate note:
            // "recompute both, unconditionally, every control block... G5 is
            // where that becomes the live design").
            if (mLpfCrossfadeActive) {
                prepGInterp(ch.lpfLadder, ch.gLpfLadderCur, ch.gLpfLadderStep);
                prepGInterp(ch.lpfSvf, ch.gLpfSvfCur, ch.gLpfSvfStep);
            } else if (mLpfSlopeSettled == static_cast<int>(LpfSlope::Db24)) {
                prepGInterp(ch.lpfLadder, ch.gLpfLadderCur, ch.gLpfLadderStep);
            } else {
                prepGInterp(ch.lpfSvf, ch.gLpfSvfCur, ch.gLpfSvfStep);
            }

            // G6 (DESIGN.md §5.5): only recomputed (and the per-sample gHpf
            // ramp only prepared) when the HPF is NOT bypassed this control
            // block -- when bypassed, DESIGN.md §5.5's skip means nothing
            // ever reads ch.hpf's coefficients anyway (see process()'s own
            // comment), matching the LPF's own "only the currently-selected
            // structure gets prepGInterp" convention (DESIGN.md §5.1
            // [PERF-8]). `hpfFcVoice` is the SAME (voice-level) value for
            // every chain (G8.5).
            if (!mHpfBypassed) {
                // nassau_real (DESIGN.md §12.2), same prepGInterp convention above.
                const double gHpfStart = static_cast<double>(ch.hpf.g);
                ch.hpf.setControlRate(hpfFcVoice, fs);
                ch.gHpfStep = static_cast<nassau_real>(
                    (static_cast<double>(ch.hpf.g) - gHpfStart) / static_cast<double>(std::max(1, mControlBlock)));
                ch.gHpfCur = static_cast<nassau_real>(gHpfStart);
            }
        }
    }
    mDebugActiveVoiceCount = activeCount;  // G7.14/PERF-7 readback, see getDebugActiveVoiceCount()

    // DESIGN.md §12.3: rebuild the compacted active-MAIN-POOL-voice list from
    // EXACTLY the predicate the audio-rate loop's own skip check uses
    // (`state != Idle`, process()'s own `isMainPool ? (v.state == Idle) :
    // !v.fadeActive` test) -- NOT the stricter PERF-7 `silentSkippable`
    // predicate `activeCount` above uses, which is a separate, more
    // aggressive DEBUG-COUNTER-only metric (getDebugActiveVoiceCount()) that
    // does not actually gate rendering. Using any other predicate here would
    // silently change which voices render. `v.state` is finalised for the
    // whole upcoming control block by this point (every write to it above
    // has already happened), so this list is valid for every sample of the
    // chunk process() is about to render. Fade slots are deliberately
    // excluded (they can flip fadeActive MID-chunk, on their own per-sample
    // counter -- see process()'s own comment on why they always stay on the
    // scalar path).
    mActiveVoiceCount = 0;
    for (int i = 0; i < kMaxVoices; ++i) {
        if (mVoices[i].state != nassau_alloc::SlotState::Idle) {
            mActiveVoiceList[mActiveVoiceCount++] = i;
        }
    }
}

double SynthCore::octaveOffsetSemis(Octave o) {
    switch (o) {  // [ref] DESIGN.md §3.2: 16'/8'/4'/2' -> 220/440/880/1760 Hz at note 69
        case Octave::Ft16: return -12.0;
        case Octave::Ft8:  return 0.0;
        case Octave::Ft4:  return 12.0;
        case Octave::Ft2:  return 24.0;
    }
    return 0.0;
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

void SynthCore::setChorusMode(Chorus m) {
    mChorusMode.store(static_cast<int>(m), std::memory_order_relaxed);
}

void SynthCore::setDebugControlBlock(int samples) {
    mControlBlock = std::max(1, samples);
    mControlPhase = 0; // keep the grid position consistent when the size changes mid-test
}
