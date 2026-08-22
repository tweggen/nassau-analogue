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
    for (int i = 0; i < kG3Voices; ++i) {
        mVoices[i].osc1.setSampleRate(sampleRate);
        mVoices[i].osc2.setSampleRate(sampleRate);
        // [dsp] Mixer DC blocker at 5 Hz -- the same corner nassau-zermatt
        // uses for its coupling-cap models, and far enough below the lowest
        // musical fundamental (16' at MIDI 0 is 4.1 Hz, but no envelope
        // sustains a note that low audibly) to be transparent in band.
        // DESIGN.md §4.
        mVoices[i].dcBlock.setFc(5.0, sampleRate);
    }

    // Sizes/prepares all fixed (non-allocating, R3) buffers for this rate.
    // Everything else fixed-size for kMaxVoices + 2 fade slots (DESIGN.md
    // §10.3) is G7's job; G3's kG3Voices-sized array is already fixed-size.
    reset();
}

void SynthCore::reset() {
    // DESIGN.md §11 "Reset semantics": clears state, never touches
    // parameters. G3 adds oscillator phase, envelope state, noise-generator
    // seeds and the (provisional, G3-only) voice array/LFO here; later gates
    // add filter state and the real G7 allocator under the same "never touch
    // a parameter" rule.
    mControlPhase = 0;

    for (int i = 0; i < kG3Voices; ++i) {
        mVoices[i].dcBlock.reset();
        Voice& v = mVoices[i];
        v.osc1.reset();   // full reset (phase + triangle integrator state) --
        v.osc2.reset();   // distinct from the PHASE-ONLY resetPhase() a note-on
                           // uses (DESIGN.md §3.2), see applyEvent()'s comment.
        v.sub.reset();
        v.noise.init(i);  // [dsp] DESIGN.md §3.5/R8/R13: seeded from voice index
        v.envF.reset();
        v.envA.reset();
        v.active = false;
        v.note = -1;
        v.vcaGainStart = 0.0;
        v.vcaGainEnd = 0.0;
        v.debugLfoPitchModSemis = 0.0;
    }
    mActiveVoiceCount = 0;
    mLfo.init(kLfoShSeed);  // [dsp] R8/R13: fixed S&H seed; Lfo::init() also resets phase/delay
    mLastLfoValue = 0.0;

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
            double mixSum = 0.0;
            for (int vi = 0; vi < kG3Voices; ++vi) {
                Voice& v = mVoices[vi];
                if (!v.active) continue;

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

                // G4 NOTE (DESIGN.md §5.1/§1, docs/GATES.md's own "driving
                // the structures directly from the test... is the expected
                // shape" guidance): the mixer -> LPF -> VCA signal-chain
                // wiring is deliberately NOT done here yet. Routing the real
                // per-voice audio through LadderFilter/SvfFilter here was
                // tried during this gate and reverted: DESIGN.md §5.3's own
                // "Reff recomputed from the PREVIOUS control block's peak"
                // scheduling is *causal, not time-symmetric* by explicit
                // specification, so wiring it into a real (periodic,
                // resonance-fed) voice signal necessarily makes the filter's
                // coefficients periodically time-varying at block rate -- and
                // that is enough, for ANY spec-correct implementation, to
                // break the exact odd/half-period symmetry a purely-LTI
                // stage would preserve, leaking a small (~1e-4, stable,
                // non-growing -- confirmed by direct measurement, not a
                // leak/instability) resonance-dependent DC into the signal
                // that G3.12's pre-existing (pre-G4) "no DC" bound (written
                // when no filter sat in this path) does not tolerate. This
                // is a genuine interaction the plan did not name (R11); it
                // is recorded in the G4 gate note rather than silently
                // routed around. G4's own ACs (G4.1-G4.11) are all satisfied
                // by driving LadderFilter/SvfFilter directly (Tests/
                // filter_tests.cpp), matching Zermatt's cabinet_tests.cpp
                // precedent -- full per-voice signal-chain integration
                // (alongside the slope crossfade and modulation that also
                // touch this exact code path) is G5's job.
                const double gain = mDebugDisableVcaInterpolation
                                         ? v.vcaGainEnd
                                         : v.vcaGainStart + (v.vcaGainEnd - v.vcaGainStart) * frac;
                mixSum += mix * gain;
            }
            outL[n + i] = static_cast<float>(mixSum);
            outR[n + i] = static_cast<float>(mixSum);
        }
        // ---- AUDIO-RATE LOOP END ----

        n += chunk;
        mControlPhase += chunk;
        if (mControlPhase >= mControlBlock) {
            mControlPhase = 0;

            // ---- control-rate update point ----
            // Any events queued by a PREVIOUS call that couldn't reach a
            // boundary within their own call are due at THIS, the first
            // boundary a call reaches -- drain them first (chronologically
            // they precede this call's own events).
            for (int p = 0; p < mPendingCount; ++p) applyEvent(mPendingEvents[p]);
            mPendingCount = 0;

            // This call's own events with sampleOffset < n (i.e. within the
            // chunk[s] just rendered, [.., n)) are due at this boundary too.
            while (eventIdx < numEvents && events[eventIdx].sampleOffset < n) {
                applyEvent(events[eventIdx]);
                ++eventIdx;
            }

            // Per control block: ADSRs advance one step, the LFO advances
            // one step (globally, once — not per voice), pitch/VCA-gain are
            // recomputed (DESIGN.md §2).
            controlRateUpdate(snapshot, fs);
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
}

// ===== G3: event application (DESIGN.md §10.1/§10.3, minimal/provisional) ===

void SynthCore::applyEvent(const NoteEvent& ev) {
    switch (ev.type) {
        case NoteEvent::NoteOn: {
            // Reuse a voice already sounding THIS note (avoids piling up a
            // second voice on a fast repeated NoteOn -- not G7.5's full
            // "reuse" AC, but the same idea; free to add here).
            int slot = -1;
            for (int i = 0; i < kG3Voices; ++i) {
                if (mVoices[i].active && mVoices[i].note == ev.note) { slot = i; break; }
            }
            if (slot < 0) {
                for (int i = 0; i < kG3Voices; ++i) {
                    if (!mVoices[i].active) { slot = i; break; }
                }
            }
            // No allocation-order policy yet (Idle/oldest-Released/oldest-
            // Playing is G7's job, DESIGN.md §10.3) -- if every slot is
            // active and none matches this note, this minimal path just
            // reuses slot 0. Not tested by any G3 AC.
            if (slot < 0) slot = 0;

            Voice& v = mVoices[slot];
            v.note = ev.note;
            // DESIGN.md §3.2: "both oscillators reset to 0" at note-on --
            // PHASE only (resetPhase()), not the full reset() SynthCore::
            // reset()/init() use, which would also clear the triangle
            // leaky-integrator's running state. G2's own note left this
            // exact choice ("resetPhase() vs reset() at note-on") open for
            // "G7's voice design" -- G3 is in fact the first gate to build a
            // voice, so the choice is made here instead, against DESIGN.md
            // §3.2's literal wording (a decision the plan did not name G3
            // for, flagged in this gate's report per R11).
            v.osc1.resetPhase();
            v.osc2.resetPhase();
            v.sub.reset();  // keep the sub's wrap-index counter consistent with osc1's fresh phase=0
            // Noise is deliberately NOT reset at note-on (only reset()/
            // init() reseed it) -- a real analogue noise source runs
            // continuously; DESIGN.md/R13 only require voice-index seeding
            // at init()/reset(), not at every note-on.
            v.envF.noteOn();
            v.envA.noteOn();
            if (!v.active) {
                v.active = true;
                ++mActiveVoiceCount;
                if (mActiveVoiceCount == 1) mLfo.noteOnEdge();  // DESIGN.md §7: only 0->1
            }
            break;
        }
        case NoteEvent::NoteOff: {
            for (int i = 0; i < kG3Voices; ++i) {
                if (mVoices[i].active && mVoices[i].note == ev.note) {
                    mVoices[i].envF.noteOff();
                    mVoices[i].envA.noteOff();
                    break;  // first match only -- G7.2's oldest/allocation-order policy is out of scope
                }
            }
            break;
        }
        case NoteEvent::AllNotesOff: {
            // DESIGN.md §10.3: "releases every voice normally." Real CC 123
            // handling (this event's own G7.7 AC) is G7's job; releasing
            // every sounding voice via noteOff() is a correct subset of that
            // and free to add now.
            for (int i = 0; i < kG3Voices; ++i) {
                if (mVoices[i].active) {
                    mVoices[i].envF.noteOff();
                    mVoices[i].envA.noteOff();
                }
            }
            break;
        }
        case NoteEvent::AllSoundOff: {
            // DESIGN.md §10.4's click-free fade-out slots are G7's job; this
            // minimal path just hard-silences every voice immediately
            // (provisional -- not G7.7's precise 2.8 ms bound).
            for (int i = 0; i < kG3Voices; ++i) {
                if (mVoices[i].active) {
                    mVoices[i].envF.reset();
                    mVoices[i].envA.reset();
                    mVoices[i].active = false;
                    --mActiveVoiceCount;
                    mVoices[i].vcaGainStart = 0.0;
                    mVoices[i].vcaGainEnd = 0.0;
                }
            }
            break;
        }
        case NoteEvent::PitchBend:
        case NoteEvent::Sustain:
        default:
            // G7's job (DESIGN.md §10.3/§10.6/§7.6) -- no-op at G3, but
            // accepted (not a crash/UB) so G0.8's "every event type" grid
            // stays green (R1).
            break;
    }
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

    // G4.9 (DESIGN.md §5.4's clamp): kLpfCutoff clamped into [10, 0.45*fs] --
    // the ROBUSTNESS half of §5.4's clamp (tan(pi*fc/fs) diverges at
    // Nyquist), proven live here so getDebugLpfCutoff() reflects the exact
    // value the real control-rate path would feed a filter's
    // setControlRate() (Source/DSP/synth_filter.h). The MODULATION half of
    // §5.4 (key follow/ENV-F/LFO -> cutoff) is G5's job. G4's filter
    // structures themselves are not yet wired into this voice's audio
    // summation -- see the AUDIO-RATE LOOP's own G4 comment above for why.
    // Computed once per control block, not per sample (R12).
    const double lpfFcClamped = std::clamp(static_cast<double>(snapshot.lpfCutoffHz), 10.0, 0.45 * fs);
    mDebugLpfCutoffHz = lpfFcClamped;  // G4.9 readback

    for (int i = 0; i < kG3Voices; ++i) {
        Voice& v = mVoices[i];

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

        if (v.active && v.envA.isIdle()) {
            v.active = false;
            --mActiveVoiceCount;
        }

        v.vcaGainStart = v.vcaGainEnd;
        v.vcaGainEnd = mDebugForceUnityVca ? 1.0 : v.envA.y;  // G3.5: unity-gain reference render

        if (!v.active) continue;  // idle voices don't need pitch/PW recomputed (nor rendered, see process())

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
        const double noteForOsc2 = snapshot.osc2KeyTrack ? static_cast<double>(v.note) : 60.0;

        const double osc1Semis = static_cast<double>(v.note) + lfoPitchModSemis +
                                  octaveOffsetSemis(static_cast<Octave>(snapshot.osc1Octave)) +
                                  static_cast<double>(snapshot.osc1FineCents) * 0.01;
        const double osc2Semis = noteForOsc2 + lfoPitchModSemis +
                                  octaveOffsetSemis(static_cast<Octave>(snapshot.osc2Octave)) +
                                  static_cast<double>(snapshot.osc2Semi) +
                                  static_cast<double>(snapshot.osc2FineCents) * 0.01;

        // [dsp] DESIGN.md §3.2: f = 440 * 2^((semitones-69)/12), phaseInc clamped to 0.49.
        const double f1 = 440.0 * std::exp2((osc1Semis - 69.0) / 12.0);
        const double f2 = 440.0 * std::exp2((osc2Semis - 69.0) / 12.0);
        v.osc1.setDt(std::clamp(f1 / std::max(fs, 1.0), 0.0, 0.49));
        v.osc2.setDt(std::clamp(f2 / std::max(fs, 1.0), 0.0, 0.49));

        v.osc1.setPwPercent(static_cast<double>(snapshot.osc1PwPercent) + lfoPwmModPercent);
        v.osc2.setPwPercent(static_cast<double>(snapshot.osc2PwPercent) + lfoPwmModPercent);
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

void SynthCore::setDebugControlBlock(int samples) {
    mControlBlock = std::max(1, samples);
    mControlPhase = 0; // keep the grid position consistent when the size changes mid-test
}
