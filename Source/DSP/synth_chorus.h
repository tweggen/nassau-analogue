#pragma once
// JunoChorus -- the G12 output-stage BBD chorus (DESIGN.md §13).
//
// R2-legal sibling of synth_dsp.h/synth_osc.h/synth_filter.h: dependency-free
// C++17 (only <algorithm>/<cmath> plus synth_dsp.h's own OnePoleLP), no SDK,
// no IPlug2, ever. Header-only and driven directly by Tests/chorus_tests.cpp
// the same way dsp_tests.cpp drives synth_dsp.h -- it does not need a
// SynthCore to be exercised at all.
//
// WHAT THIS MODELS
// ----------------
// The chorus on a Roland Juno-6/60/106: a pair of bucket-brigade delay lines
// of a few milliseconds, swept by a triangle LFO, summed back with the dry
// signal -- one added, one SUBTRACTED, which is where the instrument's
// famously wide stereo comes from. The front-panel control is not a knob but
// two buttons, and the three positions they can produce (I, II, both) are
// the three modes below; releasing both is Off. That is the ENTIRE
// user-facing surface, which is why this lands as ONE parameter
// (`kChorus`, DESIGN.md §11 param 53) and not the five or six a generic
// chorus effect would expose.
//
// THE THREE MODES  [voicing]
// --------------------------
// Rates are the figures that circulate for the Juno-106's chorus LFO
// (I ~0.5 Hz, II ~0.86 Hz, I+II ~9.8 Hz -- the last is why both-buttons-down
// is a seasick wobble rather than a wider chorus). The DELAY figures here are
// NOT lifted from a measurement of real hardware -- no Juno was measured for
// this -- they are chosen so the resulting peak pitch deviation lands where
// each mode is described as sitting by ear, which is the quantity a listener
// actually hears:
//
//   deviation = d(delay)/dt, and for a triangle of peak `depth` at rate `f`
//   the slope is constant at 4*depth*f, so:
//
//     I     4 * 1.30 ms * 0.513 Hz = 0.00267  ->  4.6 cents   slow, gentle
//     II    4 * 1.80 ms * 0.863 Hz = 0.00621  -> 10.7 cents   faster, richer
//     I+II  4 * 0.25 ms * 9.750 Hz = 0.00975  -> 16.8 cents   fast vibrato
//
// (cents = 1200*log2(1+deviation).) The ORDERING of those three numbers is
// the part that matters and it is the part that is audibly characteristic:
// I is the mildest, II is about twice I, and I+II is not "more chorus" but a
// different effect entirely. Tests/chorus_tests.cpp checks the rates and the
// delay envelopes against these tables, so the tables are the single source
// of truth -- change them here, not in a test.
//
// WHY ONE CHANNEL IS SUBTRACTED
// -----------------------------
// L = dry + wet, R = dry - wet is what produces the Juno's width. It has a
// known consequence and it is deliberate, not an oversight: a listener who
// sums this to mono gets `2*dry + (wetL - wetR)`, i.e. the chorus does NOT
// vanish (a single shared delay line would make it cancel exactly), but it
// does change character, because the two lines are swept in ANTIPHASE and so
// never hold the same signal. Two lines instead of one is the specific choice
// that keeps mono listenable; the inversion is the choice that keeps stereo
// sounding like a Juno.
//
// PLACEMENT AND THE BIT-EXACTNESS CONTRACT
// ----------------------------------------
// This sits in the output stage, between the voice accumulator and master
// volume (DESIGN.md §1/§13) -- the Juno's own order, where the chorus is
// after the VCAs and before the volume slider.
//
// With the chorus OFF, process() below is a BIT-EXACT passthrough: it writes
// its inputs into the delay lines (so that switching the chorus on later
// starts from real signal history rather than a buffer full of stale audio
// or silence) and then returns those same two doubles unchanged, by
// assignment -- not `x + 0.0*wet`, which is only bit-exact for x != -0.0.
// That is what lets both golden batteries keep verifying at exactly
// 0.000e+00 across this gate, and what keeps G8.1's mono L==R bit-identity
// intact (DESIGN.md §13).
//
// R12: process() is called from inside SynthCore's AUDIO-RATE LOOP, so it
// contains no transcendental and no atomic load -- the only per-sample work
// is compare/add/multiply, two array stores, two interpolated reads and four
// one-pole steps. Everything with an exp() in it (the BBD low-pass
// coefficients, the mode-change glide coefficient) is computed at init or at
// block rate and passed in.

#include "synth_dsp.h"

#include <algorithm>
#include <cmath>

// ============================================================================
// JunoChorus
// ============================================================================
struct JunoChorus {
  /// The front-panel positions. Off is "neither button down"; OneTwo is
  /// "both down", which on the real instrument is a THIRD sound, not the sum
  /// of the other two. Values are the stored parameter's enum indices
  /// (DESIGN.md §11 param 53) -- append-only like every other enum here.
  enum class Mode : int { Off = 0, One = 1, Two = 2, OneTwo = 3 };
  static constexpr int kNumModes = 4;

  /// [voicing] LFO rate per mode, Hz. Index by Mode. Off's entry is 0 and is
  /// never used (the LFO is not advanced while off -- see setControlRate()).
  static constexpr double kRateHz[kNumModes] = {0.0, 0.513, 0.863, 9.750};

  /// [voicing] Delay-line centre per mode, ms. Constant across the three
  /// live modes: on the hardware the buttons change the LFO, not the BBD's
  /// clock centre.
  static constexpr double kCenterMs[kNumModes] = {3.2, 3.2, 3.2, 3.2};

  /// [voicing] Triangle peak deviation per mode, ms -- see the header
  /// comment for the pitch-deviation arithmetic these were chosen by.
  static constexpr double kDepthMs[kNumModes] = {0.0, 1.30, 1.80, 0.25};

  /// [voicing] How much delayed signal is mixed against a dry of exactly
  /// 1.0. Not 1.0: an equal mix peaks at +6 dB when the two arrive in phase,
  /// and a chorus button should not double the instrument's level. At 0.7 the
  /// worst case is +4.6 dB and the typical (decorrelated) case is
  /// sqrt(1 + 0.49) = +1.7 dB, which is the "it got bigger" a Juno's chorus
  /// actually does.
  static constexpr double kWetGain = 0.7;

  /// [voicing] Linear fade applied to the wet path when the chorus is
  /// switched on or off, ms. Same 20 ms window, and the same reasoning, as
  /// the LPF slope crossfade (DESIGN.md §5.1): long enough that a mid-note
  /// toggle does not click, short enough to feel instantaneous.
  static constexpr double kFadeMs = 20.0;

  /// [voicing] One-pole glide applied to centre/depth when switching
  /// BETWEEN two live modes (e.g. I -> I+II), ms. A jump straight from
  /// 1.80 ms of depth to 0.25 ms is an instant, audible pitch step; gliding
  /// it turns that into a short bend, which is also what a real analogue
  /// circuit's own settling does. Not used when switching from Off -- there
  /// the wet is at zero and nothing is audible to glide, so the values snap.
  static constexpr double kGlideMs = 50.0;

  /// [voicing] The BBD's bandwidth, modelled as two cascaded one-poles on
  /// the WET path only. A bucket-brigade line is clocked, band-limited and
  /// noticeably darker than its input; this is the cheap stand-in for its
  /// anti-alias/reconstruction filter pair. It is also what makes the linear
  /// interpolation below the right choice rather than a compromise: the
  /// interpolator's error is concentrated at high frequencies, and this
  /// stage attenuates exactly that band one step later.
  static constexpr double kBbdCutoffHz = 8000.0;

  /// Delay-line length in samples. Power of two so the wrap is a mask.
  /// 2048 holds 10.6 ms at 192 kHz, against a worst-case delay of
  /// 3.2 + 1.8 = 5.0 ms -- better than 2x headroom at the highest supported
  /// rate, and 32 KB of fixed, never-reallocated storage (R3).
  static constexpr int kBufferSize = 2048;
  static constexpr int kMask = kBufferSize - 1;

  /// Triangle, +1 at phase 0, -1 at phase 0.5, one cycle over [0,1).
  /// Deliberately a triangle and not a sine: a triangle sweeps the delay at
  /// a CONSTANT rate, so the pitch deviation it produces is a constant
  /// offset that flips sign twice a cycle -- which is what a BBD chorus
  /// swept by the Juno's own triangle LFO does, and is why the header
  /// comment's `4*depth*rate` deviation arithmetic is exact rather than a
  /// peak-value approximation.
  static inline double tri(double phase) {
    return 4.0 * std::fabs(phase - 0.5) - 1.0;
  }

  // ===== Lifecycle =====

  /// Prepare for a sample rate. Sets everything that depends on fs and
  /// nothing that depends on a parameter; calls reset(). Never called from
  /// the audio thread (R3) -- it contains exp() via OnePoleLP::setFc.
  void init(double fs) {
    mSamplesPerMs = fs * 1e-3;
    // Linear, so the ramp lands on EXACTLY 0.0 or 1.0 (std::min/std::max
    // below clamp it there) rather than approaching them asymptotically --
    // an exact 0.0 is what lets process() drop back to its bit-exact
    // passthrough path after a fade-out, and an exact 1.0 keeps a settled
    // wet level independent of how long ago the chorus was switched on.
    mWetStep = 1.0 / std::max(1.0, kFadeMs * 1e-3 * fs);
    for (int ch = 0; ch < 2; ++ch) {
      for (int k = 0; k < kBbdPoles; ++k) mBbd[ch][k].setFc(kBbdCutoffHz, fs);
    }
    reset();
  }

  /// Clear all state; touch no parameter and no fs-derived coefficient
  /// (DESIGN.md §11 "Reset semantics"). `mEverRun = false` is what makes the
  /// next setControlRate() SNAP instead of ramp -- the same contract
  /// SynthCore::mControlRateEverRun has, and for the same reason: a reset()
  /// must not leave state that the next control block mistakes for a
  /// mid-note toggle.
  void reset() {
    for (int i = 0; i < kBufferSize; ++i) {
      mBuf[0][i] = 0.0;
      mBuf[1][i] = 0.0;
    }
    mWrite = 0;
    mPhase = 0.0;
    mLfoIncrement = 0.0;
    mCenterSamples = 0.0;
    mDepthSamples = 0.0;
    for (int ch = 0; ch < 2; ++ch) {
      mDelayStart[ch] = 0.0;
      mDelayEnd[ch] = 0.0;
      for (int k = 0; k < kBbdPoles; ++k) mBbd[ch][k].reset();
    }
    mOn = false;
    mWetGain = 0.0;
    mWetTarget = 0.0;
    mEverRun = false;
  }

  // ===== Control rate (once per control block, DESIGN.md §2) =====

  /// Advance the chorus LFO one control step and republish the per-channel
  /// delay endpoints the audio-rate loop interpolates between.
  ///
  /// `mode` is the raw stored enum index (clamped here, so a corrupt state
  /// chunk cannot index off the tables). `lfoIncrement` is cycles per
  /// control step (rate / fsControl) and `glideCoeff` the one-pole
  /// coefficient for kGlideMs at the control rate -- both derived once per
  /// HOST block by the caller (SynthCore::finishSnapshot), because both
  /// involve a division or an exp() that has no business running per
  /// control block, let alone per sample (R12).
  void setControlRate(int mode, double lfoIncrement, double glideCoeff) {
    const int m = std::clamp(mode, 0, kNumModes - 1);
    mOn = (m != static_cast<int>(Mode::Off));

    // A "snap" is either the very first control block after init()/reset()
    // (nothing has been rendered yet, so there is nothing to glide from) or
    // a switch out of Off (the wet is at exactly zero, so no centre/depth
    // move is audible). Everything else -- a live mode change -- glides.
    const bool firstBlock = !mEverRun;
    mEverRun = true;
    const bool snap = firstBlock || mWetGain <= 0.0;

    if (mOn) {
      const double centerTarget = kCenterMs[m] * mSamplesPerMs;
      const double depthTarget = kDepthMs[m] * mSamplesPerMs;
      if (snap) {
        mCenterSamples = centerTarget;
        mDepthSamples = depthTarget;
      } else {
        mCenterSamples += glideCoeff * (centerTarget - mCenterSamples);
        mDepthSamples += glideCoeff * (depthTarget - mDepthSamples);
      }
      // The RATE glides too, on the same coefficient -- and this is not
      // cosmetic. Gliding only the depth leaves the new mode's rate driving
      // the OLD mode's depth for as long as the depth takes to settle, and
      // II -> I+II is the corner where that bites: 1.8 ms of depth swept at
      // 9.75 Hz is 4*1.8e-3*9.75 = 0.070 of pitch deviation, i.e. 117 cents,
      // seven times the 16.8 cents I+II itself is supposed to produce --
      // measured as a 0.054 ms-per-control-step delay excursion before this
      // line existed. Gliding both together bounds the worst case at the
      // product's interior maximum, (1.8-1.55a)(0.863+8.887a) peaking at
      // a = 0.53, which is 0.0218 deviation -> 37 cents: a brief swell on a
      // deliberate front-panel gesture rather than a tape-wobble swoop.
      mLfoIncrement += glideCoeff * (lfoIncrement - mLfoIncrement);
      if (snap) mLfoIncrement = lfoIncrement;
    }
    // While off, centre/depth/increment are LEFT WHERE THEY ARE rather than
    // zeroed: the delay lines keep being written (process()), and holding
    // the last live geometry means a re-enable does not have to rebuild it.

    // The LFO only runs while the chorus is audible -- including through a
    // fade-out, so the tail keeps moving instead of freezing on its way to
    // silence.
    if (mOn || mWetGain > 0.0) {
      mPhase += mLfoIncrement;
      if (mPhase >= 1.0) mPhase -= 1.0;
    }

    // The two lines are swept in ANTIPHASE (phase and phase+0.5), so
    // delay[0] + delay[1] == 2*centre exactly at every phase -- the
    // property Tests/chorus_tests.cpp pins.
    mDelayStart[0] = mDelayEnd[0];
    mDelayStart[1] = mDelayEnd[1];
    double p1 = mPhase + 0.5;
    if (p1 >= 1.0) p1 -= 1.0;
    mDelayEnd[0] = clampDelay(mCenterSamples + mDepthSamples * tri(mPhase));
    mDelayEnd[1] = clampDelay(mCenterSamples + mDepthSamples * tri(p1));
    if (snap) {
      mDelayStart[0] = mDelayEnd[0];
      mDelayStart[1] = mDelayEnd[1];
    }

    mWetTarget = mOn ? 1.0 : 0.0;
    if (firstBlock) {
      // First control block after init()/reset(): settle the wet gain
      // immediately rather than fading in, so a render begun with the
      // chorus ALREADY selected starts at full wet. Same contract as
      // SynthCore::mControlRateEverRun's own snap of mStereoBlend, and it
      // exists for the same reason: what the user configured before the
      // first note is a setting, not a gesture, and must not be turned into
      // a 20 ms ramp whose position would then depend on how the host
      // happened to slice the first blocks.
      mWetGain = mWetTarget;
    }
  }

  // ===== Audio rate (per sample) =====

  /// One sample. `frac` is the caller's position within the current control
  /// block, in [0,1) -- the SAME scalar the VCA gain is interpolated with
  /// (SynthCore's audio-rate loop), so the delay sweep is anchored to the
  /// control grid and is therefore block-size invariant.
  ///
  /// BIT-EXACT PASSTHROUGH when the chorus is off and fully faded out: the
  /// two outputs are assigned from the two inputs, not computed from them.
  ///
  /// ---- PER-SAMPLE PROCESS BEGIN ----
  /// (marker pair, matching synth_filter.h's own G4.11 convention: the
  /// region between these two markers is what Tests/chorus_tests.cpp greps
  /// for transcendentals and atomic loads, R12. Keep every future edit to
  /// the per-sample path inside them.)
  inline void process(double inL, double inR, double frac, double& outL, double& outR) {
    // The delay lines are written UNCONDITIONALLY, even while off. Two
    // stores and a masked increment is a price worth paying to make
    // switching the chorus on mid-note start from the signal that was
    // actually playing rather than from silence or from whatever was last
    // in the buffer -- and it means nothing ever has to clear 32 KB from
    // the audio thread.
    const int w = mWrite;
    mBuf[0][w] = inL;
    mBuf[1][w] = inR;
    mWrite = (w + 1) & kMask;

    if (!mOn && mWetGain <= 0.0) {
      outL = inL;
      outR = inR;
      return;
    }

    // Linear per-sample interpolation of the delay itself, between this
    // control block's two endpoints. For a triangle LFO this is EXACT
    // within a segment (a straight line between two points on a straight
    // line), so the pitch deviation it produces is genuinely constant
    // rather than stepped at the control rate.
    const double dL = mDelayStart[0] + (mDelayEnd[0] - mDelayStart[0]) * frac;
    const double dR = mDelayStart[1] + (mDelayEnd[1] - mDelayStart[1]) * frac;

    double wetL = readInterp(0, w, dL);
    double wetR = readInterp(1, w, dR);
    for (int k = 0; k < kBbdPoles; ++k) {
      wetL = mBbd[0][k].process(wetL);
      wetR = mBbd[1][k].process(wetR);
    }

    // Wet fade, per sample and linear, landing on EXACTLY the target.
    if (mWetGain < mWetTarget) {
      mWetGain = std::min(mWetTarget, mWetGain + mWetStep);
    } else if (mWetGain > mWetTarget) {
      mWetGain = std::max(mWetTarget, mWetGain - mWetStep);
    }

    const double g = mWetGain * kWetGain;
    // The inversion on the right is the Juno's own -- see the header
    // comment for what it buys and what it costs.
    outL = inL + g * wetL;
    outR = inR - g * wetR;
  }
  // ---- PER-SAMPLE PROCESS END ----

  // ===== Readback (tests; DESIGN.md §2's debug-accessor convention) =====

  /// Delay currently published for `channel` (0 = L, 1 = R), in ms, as of
  /// the most recent control step -- i.e. the endpoint the audio-rate loop
  /// is interpolating TOWARDS.
  double delayMs(int channel) const {
    return mDelayEnd[channel == 0 ? 0 : 1] / std::max(1e-12, mSamplesPerMs);
  }
  double lfoPhase() const { return mPhase; }
  double wetGain() const { return mWetGain; }
  bool on() const { return mOn; }
  /// True while process() is doing anything other than its bit-exact
  /// passthrough (i.e. the chorus is on, or is still fading out).
  bool audible() const { return mOn || mWetGain > 0.0; }

 private:
  static constexpr int kBbdPoles = 2;

  /// Keep the read pointer inside the buffer with room for the interpolator's
  /// second tap, whatever a mode table or a glide transient asks for.
  static double clampDelay(double d) {
    return std::clamp(d, 1.0, static_cast<double>(kBufferSize - 2));
  }

  /// Two-tap linear read at a fractional delay behind write index `w`.
  /// `d` is pre-clamped to [1, kBufferSize-2], so the integer part is
  /// non-negative and `w - di - 1 + kBufferSize` cannot go negative --
  /// which is why the wrap can be a mask without relying on how `&`
  /// treats a negative operand.
  inline double readInterp(int ch, int w, double d) const {
    const int di = static_cast<int>(d);  // d >= 1 > 0, so truncation == floor
    const double fr = d - static_cast<double>(di);
    const int i0 = (w - di + kBufferSize) & kMask;
    const int i1 = (w - di - 1 + kBufferSize) & kMask;
    return mBuf[ch][i0] * (1.0 - fr) + mBuf[ch][i1] * fr;
  }

  // Fixed storage, never reallocated (R3). Plain `double` rather than
  // `nassau_real`: DESIGN.md §12.2's single-precision opt-in is scoped to
  // filter state/coefficients and the DC blockers, and this is neither --
  // it is a memory of the finished output signal, and narrowing it would
  // put a quantisation floor under the wet path for no per-sample saving
  // (the cost here is two stores either way).
  double mBuf[2][kBufferSize] = {};
  int mWrite = 0;

  OnePoleLP mBbd[2][kBbdPoles];

  double mSamplesPerMs = 48.0;
  double mWetStep = 1.0;

  double mPhase = 0.0;
  double mLfoIncrement = 0.0;
  double mCenterSamples = 0.0;
  double mDepthSamples = 0.0;
  double mDelayStart[2] = {0.0, 0.0};
  double mDelayEnd[2] = {0.0, 0.0};

  bool mOn = false;
  double mWetGain = 0.0;
  double mWetTarget = 0.0;
  bool mEverRun = false;
};
