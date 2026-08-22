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
    for (int i = 0; i < kMaxVoices + kNumFadeSlots; ++i) {
        mVoices[i].osc1.setSampleRate(sampleRate);
        mVoices[i].osc2.setSampleRate(sampleRate);
        // [dsp] Mixer DC blocker at 5 Hz -- the same corner nassau-zermatt
        // uses for its coupling-cap models, and far enough below the lowest
        // musical fundamental (16' at MIDI 0 is 4.1 Hz, but no envelope
        // sustains a note that low audibly) to be transparent in band.
        // DESIGN.md §4.
        mVoices[i].dcBlock.setFc(5.0, sampleRate);
        mVoices[i].postLpfDcBlock.setFc(5.0, sampleRate);
    }
    // [dsp] G6: the THIRD (final-output) DC blocker -- see its own field
    // comment in synth_core.h for why it exists. Same 5Hz corner as the
    // other two (DESIGN.md §4/§11), for the identical reason.
    mOutputDcBlock.setFc(5.0, sampleRate);

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
        mVoices[i].dcBlock.reset();
        mVoices[i].postLpfDcBlock.reset();
        Voice& v = mVoices[i];
        v.osc1.reset();   // full reset (phase + triangle integrator state) --
        v.osc2.reset();   // distinct from the PHASE-ONLY resetPhase() a note-on
                           // uses (DESIGN.md §3.2), see applyEvent()'s comment.
        v.sub.reset();
        v.noise.init(i);  // [dsp] DESIGN.md §3.5/R8/R13: seeded from voice index
        v.envF.reset();
        v.envA.reset();
        v.lpfLadder.reset();  // G5: both structures exist per voice regardless of kLpfSlope
        v.lpfSvf.reset();     // (DESIGN.md §5.1 [PERF-8] crossfade), so both must reset here.
        v.hpf.reset();        // G6: DESIGN.md §5.5
        v.gHpfCur = 0.0;
        v.gHpfStep = 0.0;
        v.debugMixOut = 0.0;
        v.debugDriveOut = 0.0;
        v.debugHpfOut = 0.0;
        v.debugHpfCutoffHz = 0.0;
        v.lastLpfOutput = 0.0;
        v.debugLpfCutoffHz = 0.0;
        v.gLpfLadderCur = 0.0;
        v.gLpfLadderStep = 0.0;
        v.gLpfSvfCur = 0.0;
        v.gLpfSvfStep = 0.0;
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
    mHeldVoiceCount = 0;
    mLfo.init(kLfoShSeed);  // [dsp] R8/R13: fixed S&H seed; Lfo::init() also resets phase/delay
    mLastLfoValue = 0.0;
    mOutputDcBlock.reset();  // G6: see its own field comment in synth_core.h

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

    mPendingCount = 0;

    // mControlBlock, mDebugAtomicLoadCount and the two mDebug* VCA test hooks
    // are deliberately NOT touched here -- they are test-only instrumentation
    // / harness configuration, not synth state (same precedent as
    // mControlBlock already being left alone by this function since G0).
}

// ===== Audio Processing =====

void SynthCore::process(const NoteEvent* events, int numEvents,
                         float* outL, float* outR, int numSamples) {
    // DESIGN.md §2.2: exactly ONE ParamSnapshot per host block, every atomic
    // loaded exactly once (R3's mechanism — the audio-rate loop below never
    // touches an atomic, satisfying R12 too). G3.1's instrumented count is
    // driven by buildSnapshot()'s own counting wrapper (see below) — nothing
    // else in this function increments it.
    ParamSnapshot snapshot = buildSnapshot();

    // fs is read exactly ONCE per host block too (block rate, not audio
    // rate) -- deliberately OUTSIDE buildSnapshot() so it does not count
    // against G3.1's "53 params" figure (mSampleRate is configuration, not
    // one of DESIGN.md §11's 53 parameters). finishSnapshot() below computes
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

        // ---- AUDIO-RATE LOOP BEGIN (R12/R3: no transcendentals, no atomic
        // loads anywhere between this marker and AUDIO-RATE LOOP END --
        // G3.2's grep-based inspection test (Tests/envlfo_tests.cpp) checks
        // this region of THIS FILE verbatim, so keep any future edit to this
        // loop inside these two markers and free of tan/exp/exp2/pow/log/
        // sin/cos/.load(). The three DESIGN.md §2 interpolated scalars
        // (gLpf, gHpf, vcaGain) are a lerp -- add/sub/mul/div only. ----
        for (int i = 0; i < chunk; ++i) {
            const int posInBlock = mControlPhase + i;
            const double frac = static_cast<double>(posInBlock) / static_cast<double>(blockSizeForFrac);

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

            double mixSum = 0.0;
            for (int vi = 0; vi < kMaxVoices + kNumFadeSlots; ++vi) {
                Voice& v = mVoices[vi];
                // G7: main-pool voices [0,kMaxVoices) skip on Idle (DESIGN.md
                // §10.7 [PERF-7] -- the VCA-after-filter chain ordering below
                // pins vcaGain, and therefore this voice's WHOLE
                // contribution, at exactly 0.0 the control block after ENV-A
                // reaches Idle, which is exactly when `state` becomes Idle
                // too (controlRateUpdate()) -- see getDebugActiveVoiceCount()
                // 's own header comment for the full argument). Fade slots
                // [kMaxVoices, kMaxVoices+kNumFadeSlots) skip on
                // !fadeActive instead: DESIGN.md §10.4's 2ms fade is a
                // property of the SLOT, independent of that copied voice's
                // own envelope state.
                const bool isMainPool = vi < kMaxVoices;
                if (isMainPool ? (v.state == nassau_alloc::SlotState::Idle) : !v.fadeActive) continue;

                const double prePhase1 = v.osc1.phase;
                const auto r1 = v.osc1.step();
                double y2 = v.osc2.step().y;
                if (snapshot.osc2Sync && r1.wrapped) {
                    y2 = v.osc2.hardSync(r1.wrapFrac / v.osc1.dt, y2);
                }
                const double ysub = v.sub.step(prePhase1, v.osc1.dt, r1.wrapped, true);
                const double ynoise = v.noise.step(static_cast<NoiseSource::Color>(snapshot.noiseColor));

                const double mixRaw = MixerBlock::mix(r1.y, snapshot.osc1LevelPercent, y2,
                                                    snapshot.osc2LevelPercent, ysub, snapshot.subLevelPercent,
                                                    ynoise, snapshot.noiseLevelPercent);
                // DC blocker on the mixer output (DESIGN.md §4). A pulse of
                // duty d carries DC of exactly (2d - 1); at PW=25% that is
                // -0.50. Two mul + two add per sample per voice, and it must
                // sit BEFORE the drive and the resonant filter, not at the
                // output: DC into a saturator biases it into asymmetric
                // clipping, which would make the timbre track pulse width in
                // a way that is not the PWM sound anyone wants.
                const double mix = v.dcBlock.process(mixRaw);
                v.debugMixOut = mix;  // G6.7 test-only readback, see Voice::debugMixOut's comment

                // G6 (DESIGN.md §1 step 7, §4): DRIVE sits between the
                // mixer DC blocker and the HPF -- never after the filters
                // (DESIGN.md §1's own "ordering constraints": "the mixer's
                // summed level is what pushes it, exactly as in a real
                // instrument where the VCA-input stage is what runs out of
                // headroom first"). snapshot.drivePre/driveKnee are BLOCK-
                // RATE derived constants (finishSnapshot(), R12) -- at
                // Drive=0 they are EXACTLY 1.0/0.0, so this whole line is
                // bit-exact identity (mix*1.0==mix, shapeTriodeK(x,0)==x,
                // x/1.0==x, DESIGN.md §4's own "bit-exact identity" claim,
                // docs/GATES.md G6.7).
                const double driven = shapeTriodeK(mix * snapshot.drivePre, snapshot.driveKnee) / snapshot.drivePre;
                v.debugDriveOut = driven;  // G6.4 test-only readback, see Voice::debugDriveOut's comment

                // G6 (DESIGN.md §1 step 8, §5.5): HPF, hard-bypassed at
                // kHpfCutoff's minimum (DESIGN.md §5.5: "not a 20Hz filter
                // -- an actual bypass"). mHpfBypassed/mHpfNumPoles are
                // shared, instrument-wide, computed once per control block
                // below (kHpfSlope/kHpfCutoff are single params, not
                // per-voice) -- this is an ARCHITECTURAL skip (the whole
                // HpfCascade call is never made), which is what makes the
                // bypass path BIT-EXACT (docs/GATES.md G6.4) rather than
                // merely "a very low corner". gHpfCur/Step is this voice's
                // own per-sample gHpf interpolation (DESIGN.md §2's third
                // interpolated quantity, alongside gLpf/vcaGain).
                double hpfOut;
                if (mHpfBypassed) {
                    hpfOut = driven;
                } else {
                    v.hpf.advanceCoeff(v.gHpfCur);
                    v.gHpfCur += v.gHpfStep;
                    hpfOut = v.hpf.process(driven, mHpfNumPoles);
                }
                v.debugHpfOut = hpfOut;  // G6.4 test-only readback, see Voice::debugHpfOut's comment

                // G5 (DESIGN.md §1 step 9, §5): the LPF is now GENUINELY IN
                // THE SIGNAL PATH -- the interaction G4's gate note flagged
                // (wiring the SVF's causal, previous-control-block Reff into
                // a real periodic voice signal broke G3.12's "no DC" bound,
                // measured up to -7.96e-4) no longer applies: the mixer DC
                // blocker just above now sits BEFORE this stage (it did not
                // yet exist in that form when G4's attempt was made), so the
                // filter's own input is already DC-free (DESIGN.md §4.1
                // measures 1e-9..1e-10 residual at every duty).
                //
                // That is NOT sufficient on its own, and an earlier version of
                // this comment wrongly claimed it was. A DC-free INPUT does not
                // give a DC-free OUTPUT here: both structures' feedback
                // saturators are odd functions, and an odd function fed a
                // zero-mean but not half-wave-symmetric signal re-introduces a
                // nonzero time-average. Measured with only the mixer blocker:
                // 2.3e-2 DC / 3.69 peak at {SVF, res 99%, PW 30%, fc 500 Hz},
                // against a 1e-4 bound -- while a symmetric 50% pulse at the
                // same resonance gave 1.5e-6, which is what identifies the
                // mechanism. Hence postLpfDcBlock below (DESIGN.md §5.6).
                double lpfOut;
                if (mLpfCrossfadeActive) {
                    const double yFrom = runLpfStructure(v, mLpfCrossfadeFromSlope, hpfOut);
                    const double yTo = runLpfStructure(v, mLpfCrossfadeToSlope, hpfOut);
                    lpfOut = yFrom * (1.0 - crossfadeT) + yTo * crossfadeT;
                } else {
                    lpfOut = runLpfStructure(v, mLpfSlopeSettled, hpfOut);
                }
                v.lastLpfOutput = lpfOut;
                lpfOut = v.postLpfDcBlock.process(lpfOut);

                const double gain = mDebugDisableVcaInterpolation
                                         ? v.vcaGainEnd
                                         : v.vcaGainStart + (v.vcaGainEnd - v.vcaGainStart) * frac;

                // G7 (DESIGN.md §10.4): `fadeGainCur` is 1.0, permanently,
                // for every main-pool voice (never written outside the fade
                // branch below), so this multiply is a bit-exact IEEE-754
                // identity (x*1.0==x) there -- it only actually attenuates a
                // fade slot's copied signal. Plain multiply, R12-legal.
                const double contribution = lpfOut * gain * v.fadeGainCur;
                mixSum += contribution;

                // G7/DESIGN.md §10.7 [PERF-7]: track this voice's own peak
                // |contribution| over the control block IN PROGRESS (plain
                // fabs/max, no transcendental/atomic, R12) -- copied into
                // `peakPrevBlock` at the next control-rate boundary
                // (controlRateUpdate()), which is the quantity
                // getDebugActiveVoiceCount() actually reads.
                const double absContribution = contribution < 0.0 ? -contribution : contribution;
                if (absContribution > v.curBlockPeakAccum) v.curBlockPeakAccum = absContribution;

                // G7 (DESIGN.md §10.4): advance a live fade slot's own 2ms
                // linear ramp by one sample and retire it the instant it
                // completes -- same shape as the LPF slope crossfade's own
                // per-sample completion tracking just below (plain int/
                // double arithmetic only, R12).
                if (!isMainPool && v.fadeActive) {
                    v.fadeGainCur += v.fadeGainStep;
                    ++v.fadeSamplesElapsed;
                    if (v.fadeSamplesElapsed >= v.fadeSamplesTotal) {
                        v.fadeActive = false;
                        v.fadeGainCur = 0.0;
                    }
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
            // (same category as shapeTriodeK above).
            double outSample = mixSum * snapshot.masterVolumeLinear;
            if (snapshot.outputClip) {
                outSample = shapeCubic(outSample, 2.0);  // [dsp] DESIGN.md §11, L=2.0 -> +6dBFS ceiling
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
                outSample = mOutputDcBlock.process(outSample);
            }
            outL[n + i] = static_cast<float>(outSample);
            outR[n + i] = static_cast<float>(outSample);
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
    // "exactly 53 * numHostBlocks" (docs/GATES.md G3.1). A generic lambda
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
    v.osc1.resetPhase();
    v.osc2.resetPhase();
    v.sub.reset();  // keep the sub's wrap-index counter consistent with osc1's fresh phase=0
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

        v.osc1.wave = static_cast<Osc::Wave>(snapshot.osc1Wave);
        v.osc2.wave = static_cast<Osc::Wave>(snapshot.osc2Wave);
        v.sub.octave = static_cast<SubOsc::Octave>(snapshot.subOctave);

        // [voicing] kOsc2KeyTrack off -> VCO2 ignores the played note and
        // sits at a fixed reference pitch (note 60, middle C) instead --
        // DESIGN.md §11 names this param but does not spell out its
        // behaviour; this is the conventional Prophet/Jupiter-family reading
        // of an oscillator "key track" switch (a fixed-pitch drone/FM-
        // operator use case), and no G3 AC exercises it either way. Flagged
        // per R11 as a decision the plan did not cover.
        const double noteForOsc2 = snapshot.osc2KeyTrack ? v.glideCurrentSemis : 60.0;

        // [ref] DESIGN.md §8 [PERF-6]: Poly-Mod, control-rate only.
        // kPmEnvFToOsc2 -- ENV-F to VCO2 pitch, bipolar, +/-24 semitones at
        // 100%; kPmEnvFToPw -- ENV-F to BOTH oscillators' pulse width,
        // bipolar, +/-45% at 100%. `v.envF.y` is this SAME control step's
        // freshly-advanced ENV-F value (v.envF.step() ran a few lines above,
        // matching the way lfoPitchModSemis/lfoPwmModPercent above already
        // reuse this control block's freshly-stepped LFO value).
        const double pmOsc2Semis =
            (static_cast<double>(snapshot.pmEnvFToOsc2Percent) * 0.01) * 24.0 * v.envF.y;
        const double pmPwModPercent =
            (static_cast<double>(snapshot.pmEnvFToPwPercent) * 0.01) * 45.0 * v.envF.y;

        // DESIGN.md §3.2: semitones = note + bend*BendRange + glide +
        // octave*12 + semi + fine/100 + lfoPitch + pmEnvFToOsc2 (VCO2 only).
        // `v.glideCurrentSemis` IS the glide-smoothed "note" term (this
        // struct field's own comment); `mBendSemis` (G7.9) and
        // `v.unisonDetuneCents/100` (G7.13, Unison only, 0 otherwise) are
        // APPENDED after the original G3-G6 term order rather than
        // interleaved, and are EXACTLY 0.0 outside Unison/bend use --
        // appending an exact +0.0 to a finite sum is a bit-exact IEEE-754
        // identity, which is what keeps every existing G0-G6 test/golden
        // case (no bend, no Unison) bit-for-bit unchanged (G7 gate report).
        const double osc1Semis = v.glideCurrentSemis + lfoPitchModSemis +
                                  octaveOffsetSemis(static_cast<Octave>(snapshot.osc1Octave)) +
                                  static_cast<double>(snapshot.osc1FineCents) * 0.01 +
                                  mBendSemis + v.unisonDetuneCents * 0.01;
        const double osc2Semis = noteForOsc2 + lfoPitchModSemis + pmOsc2Semis +
                                  octaveOffsetSemis(static_cast<Octave>(snapshot.osc2Octave)) +
                                  static_cast<double>(snapshot.osc2Semi) +
                                  static_cast<double>(snapshot.osc2FineCents) * 0.01 +
                                  mBendSemis + v.unisonDetuneCents * 0.01;

        // [dsp] DESIGN.md §3.2: f = 440 * 2^((semitones-69)/12), phaseInc clamped to 0.49.
        const double f1 = 440.0 * std::exp2((osc1Semis - 69.0) / 12.0);
        const double f2 = 440.0 * std::exp2((osc2Semis - 69.0) / 12.0);
        v.osc1.setDt(std::clamp(f1 / std::max(fs, 1.0), 0.0, 0.49));
        v.osc2.setDt(std::clamp(f2 / std::max(fs, 1.0), 0.0, 0.49));

        v.osc1.setPwPercent(static_cast<double>(snapshot.osc1PwPercent) + lfoPwmModPercent + pmPwModPercent);
        v.osc2.setPwPercent(static_cast<double>(snapshot.osc2PwPercent) + lfoPwmModPercent + pmPwModPercent);

        // G5.4: THIS voice's own most recent LPF output seeds the incoming
        // structure -- each voice carries a different signal, so each needs
        // its own seed (the shared crossfade TIMING above is instrument-wide,
        // this per-voice STATE seeding is not).
        if (startingLpfCrossfade) {
            if (mLpfCrossfadeToSlope == static_cast<int>(LpfSlope::Db24)) {
                v.lpfLadder.seedFromOutput(v.lastLpfOutput);
            } else {
                v.lpfSvf.seedFromOutput(v.lastLpfOutput);
            }
        }

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
        // unison terms above, for the identical bit-exactness reason.
        const double lpfFcRaw = static_cast<double>(snapshot.lpfCutoffHz) *
                                 std::exp2(lpfKeyFollowOct + lpfEnvOct + lpfLfoOct + v.velFilterOct);
        const double lpfFcVoice = std::clamp(lpfFcRaw, 10.0, 0.45 * fs);
        v.debugLpfCutoffHz = lpfFcVoice;  // G5.6 readback

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
        // it by this step afterward.
        auto prepGInterp = [&](auto& filter, double& gCur, double& gStep) {
            const double gStart = filter.g;
            filter.setControlRate(lpfFcVoice, fs, lpfResonancePercent);
            gStep = (filter.g - gStart) / static_cast<double>(std::max(1, mControlBlock));
            gCur = gStart;
        };

        // DESIGN.md §5.1 [PERF-8]: only the currently-selected structure
        // runs a control block's worth of coefficient recompute, EXCEPT
        // during the 20ms crossfade, when BOTH must (G4's own gate note:
        // "recompute both, unconditionally, every control block... G5 is
        // where that becomes the live design").
        if (mLpfCrossfadeActive) {
            prepGInterp(v.lpfLadder, v.gLpfLadderCur, v.gLpfLadderStep);
            prepGInterp(v.lpfSvf, v.gLpfSvfCur, v.gLpfSvfStep);
        } else if (mLpfSlopeSettled == static_cast<int>(LpfSlope::Db24)) {
            prepGInterp(v.lpfLadder, v.gLpfLadderCur, v.gLpfLadderStep);
        } else {
            prepGInterp(v.lpfSvf, v.gLpfSvfCur, v.gLpfSvfStep);
        }

        // G6 (DESIGN.md §5.5): fc_hp = clamp(HpfCutoff * exp2(HpfKeyFollow/100
        // * (note-60)/12), 10, 0.45*fs). Only recomputed (and the per-sample
        // gHpf ramp only prepared) when the HPF is NOT bypassed this control
        // block -- when bypassed, DESIGN.md §5.5's skip means nothing ever
        // reads v.hpf's coefficients anyway (see process()'s own comment),
        // matching the LPF's own "only the currently-selected structure gets
        // prepGInterp" convention (DESIGN.md §5.1 [PERF-8]).
        if (!mHpfBypassed) {
            const double hpfKeyFollowOct = (static_cast<double>(snapshot.hpfKeyFollowPercent) * 0.01) *
                                            (v.glideCurrentSemis - 60.0) / 12.0;
            const double hpfFcRaw = static_cast<double>(snapshot.hpfCutoffHz) * std::exp2(hpfKeyFollowOct);
            const double hpfFcVoice = std::clamp(hpfFcRaw, 10.0, 0.45 * fs);
            v.debugHpfCutoffHz = hpfFcVoice;  // G6.5 readback

            const double gHpfStart = v.hpf.g;
            v.hpf.setControlRate(hpfFcVoice, fs);
            v.gHpfStep = (v.hpf.g - gHpfStart) / static_cast<double>(std::max(1, mControlBlock));
            v.gHpfCur = gHpfStart;
        } else {
            v.debugHpfCutoffHz = 0.0;  // G6.5: 0 while bypassed, matches this class's convention
        }
    }
    mDebugActiveVoiceCount = activeCount;  // G7.14/PERF-7 readback, see getDebugActiveVoiceCount()
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

void SynthCore::setDebugControlBlock(int samples) {
    mControlBlock = std::max(1, samples);
    mControlPhase = 0; // keep the grid position consistent when the size changes mid-test
}
