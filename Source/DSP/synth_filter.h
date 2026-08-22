#pragma once

// synth_filter.h — NassauAnalogue low-pass filter structures (Gate G4,
// DESIGN.md §5.1-§5.3). Header-only, dependency-free (R2): this file may
// include only <algorithm> <cmath> <cstdint>, plus this directory's own
// sibling header synth_dsp.h (itself R2-legal).
//
// DESIGN.md §5.1: "24/12 dB switchable" is NOT one 4-pole ladder with a tap
// after pole 2 -- a 2-pole cascade inside a negative-feedback loop only
// asymptotes to 180 degrees of phase shift and can never self-oscillate, so
// a tap would silently lose the Jupiter-6's 12 dB resonance. Two DIFFERENT
// structures live here instead:
//
//   LadderFilter — 24 dB mode: 4-pole ZDF ladder (DESIGN.md §5.2).
//   SvfFilter    — 12 dB mode: 2-pole TPT state-variable filter, low-pass
//                  output (DESIGN.md §5.3).
//
// Both reuse TptOnePole/shapeTriodeK from synth_dsp.h (already gate-proven,
// G1) rather than re-deriving the per-pole TPT recursion.
//
// R12/G4.11: EVERY division either structure needs (the ladder's
// `1/(1+k*G^4)`, the SVF's `d = 1/(1+2*Reff*g+g*g)`) is computed ONCE, in
// setControlRate() -- called once per CONTROL BLOCK by the caller (DESIGN.md
// §2), never per sample -- and cached as a reciprocal/product the per-sample
// process() below only multiplies by. process()'s own body is delimited by
// a matching pair of marker comments a few lines below each function
// (mirroring synth_core.cpp's own "AUDIO-RATE LOOP" marker convention,
// G3.2) specifically so Tests/filter_tests.cpp can grep that exact region
// and prove, not just assert, that it contains no division and no
// transcendental call (G4.11).
//
// Scope (docs/GATES.md G4): cutoff modulation (ENV-F/key-follow/LFO), the
// slope crossfade, and the golden fixture are explicitly OUT of scope here
// (G5's job) -- setControlRate() below takes an already-resolved `fc` in Hz,
// nothing more.

#include "synth_dsp.h"

#include <algorithm>
#include <cmath>

// ============================================================================
// LadderFilter — 24 dB ZDF ladder, DESIGN.md §5.2.
//
// Four TptOnePole stages inside a global feedback loop solved in CLOSED FORM
// (Zavalishin's zero-delay-feedback technique): with S_i = (1-G)*s_i the
// zero-input output of stage i (the state each pole would emit this sample if
// its own input were exactly 0),
//
//   Sigma = G^3*S1 + G^2*S2 + G*S3 + S4
//   y4lin = (G^4*x + Sigma) / (1 + k*G^4)      // LINEAR zero-delay solution
//   u     = x - k*shapeTriodeK(y4lin, kLadderSat)   // ONE fixed-point step [PERF-3]
//
// `u` is then run through the four REAL TptOnePole stages to both produce the
// actual (nonlinearly-bounded) output and advance their state for next
// sample. [PERF-3]: this solves the LINEAR loop exactly and applies the
// saturator to the resulting feedback signal in one step, rather than a true
// per-stage nonlinear ladder needing Newton iteration -- stable, one
// control-rate division, and the saturator's GAIN REDUCTION is what bounds
// self-oscillation amplitude (DESIGN.md §5.2's describing-function estimate:
// A ~= 0.6). This placement matters: the loop has genuine excess gain
// (k=4.2 at full resonance, threshold is 4.0), so a gain-reducing saturator
// on the feedback is exactly the right bounding mechanism here -- the
// opposite arrangement from SvfFilter below, where R0 goes NEGATIVE and a
// signal saturator would be destabilising (DESIGN.md §5.3).
// ============================================================================
struct LadderFilter {
  TptOnePole p1, p2, p3, p4;

  double g = 0.0;   // [dsp] tan(pi*fc/fs), control-rate (DESIGN.md §5.2)
  double G = 0.0;   // [dsp] g/(1+g)
  double G2 = 0.0, G3 = 0.0, G4 = 0.0;  // [dsp] powers of G, control-rate cache
  double k = 0.0;   // [voicing] resonance: k = 4.2*(Resonance/100), DESIGN.md §5.2 --
                     // theoretical self-osc threshold of the 4-pole loop is exactly 4.0,
                     // so 4.2 puts self-oscillation comfortably inside the knob's top end.
  double invDenom = 1.0;  // [dsp] 1/(1 + k*G^4) -- see the class comment: computed HERE
                           // (control rate), never in process() (R12/G4.11).

  // [voicing] DESIGN.md §5.2: the feedback saturator's knee. Sets the
  // self-oscillation amplitude (describing-function estimate A ~= 0.6, see
  // the class comment) -- NOT a signal clip, it sits inside the feedback
  // path only (DESIGN.md §5.2's own derivation).
  static constexpr double kLadderSat = 0.10;

  void reset() {
    p1.reset();
    p2.reset();
    p3.reset();
    p4.reset();
  }

  /// Control-rate coefficient update (DESIGN.md §2/§5.2): call once per
  /// control block (32 samples in production, DESIGN.md §2), never per
  /// sample. `fc` is the ALREADY-CLAMPED cutoff in Hz (the [10, 0.45*fs]
  /// clamp of DESIGN.md §5.4 is the caller's job -- SynthCore's
  /// controlRateUpdate() applies it once and shares the clamped value with
  /// getDebugLpfCutoff(), G4.9); `resonancePercent` is 0..100 (DESIGN.md §11
  /// kLpfResonance). Robustness clamp here too (TptOnePole::setFc's own
  /// [1, 0.49*fs] guard) so a caller that forgot §5.4's clamp cannot make
  /// tan() diverge.
  void setControlRate(double fc, double fs, double resonancePercent) {
    g = std::tan(kAmpPi * std::clamp(fc, 10.0, 0.45 * fs) / fs);  // [dsp] DESIGN.md §5.2/§5.4
    G = g / (1.0 + g);
    G2 = G * G;
    G3 = G2 * G;
    G4 = G3 * G;
    k = 4.2 * (resonancePercent * 0.01);  // [voicing] DESIGN.md §5.2
    invDenom = 1.0 / (1.0 + k * G4);      // [dsp] THE control-rate division (G4.11)

    p1.g = g;
    p2.g = g;
    p3.g = g;
    p4.g = g;
  }

  // R12/G4.11: no division, no transcendental, no atomic load anywhere in
  // this function -- Tests/filter_tests.cpp greps the region between the
  // two marker comments immediately below (a plain substring search, no
  // comment-stripping) for a literal division character and for every
  // forbidden call this project's R12 names, so keep any future edit to
  // process() itself free of them, and keep the marked region free of any
  // comment containing a literal division character too.
  // ---- PER-SAMPLE PROCESS BEGIN ----
  inline double process(double x) {
    const double S1 = (1.0 - G) * p1.s;
    const double S2 = (1.0 - G) * p2.s;
    const double S3 = (1.0 - G) * p3.s;
    const double S4 = (1.0 - G) * p4.s;
    const double Sigma = G3 * S1 + G2 * S2 + G * S3 + S4;
    const double y4lin = (G4 * x + Sigma) * invDenom;
    const double u = x - k * shapeTriodeK(y4lin, kLadderSat);
    const double y1 = p1.process(u);
    const double y2 = p2.process(y1);
    const double y3 = p3.process(y2);
    const double y4 = p4.process(y3);
    return y4;
  }
  // ---- PER-SAMPLE PROCESS END ----
};

// ============================================================================
// SvfFilter — 12 dB TPT state-variable filter, low-pass output, DESIGN.md
// §5.3.
//
//   g  = tan(pi*fc/fs)
//   d  = 1 / (1 + 2*Reff*g + g*g)
//   hp = (x - (2*Reff+g)*ic1 - ic2) * d
//   bp = g*hp + ic1 ;  ic1 = g*hp + bp
//   lp = g*bp + ic2 ;  ic2 = g*bp + lp        // lp is the output
//
// Resonance maps to the DAMPING R, not to Q (DESIGN.md §5.3's "two things
// got wrong on the first pass"): the obvious `Q = 0.5 + 19.5*res` maps to
// `R = 1/(2Q)`, which stays strictly positive at any Q -- a 2-pole SVF with
// positive R cannot self-oscillate at any setting. Instead:
//
//   R0   = 1.0 - 1.005*(Resonance/100)                    // res=0 -> +1.0, res=100 -> -0.005
//   Reff = clamp(R0 + kSvfSat*peakBpPrev^2, -0.02, 2.0)    // kSvfSat = 0.02
//
// and the nonlinearity regulates the DAMPING (Reff grows with amplitude),
// never the signal: a `shapeTriodeK` on `ic1` inside `(2R+g)` would have
// gain <= 1, which only ever REDUCES that term's magnitude -- destabilising
// once R0 < 0, the opposite of what's needed. Growing Reff with amplitude
// instead gives a Van-der-Pol-style limit cycle settling exactly where
// Reff = 0, i.e. peakBp = sqrt(-R0/kSvfSat) = 0.5 at full resonance.
//
// `peakBpPrev` is the peak |bp| over the PREVIOUS control block, so Reff (and
// the division `d`) is recomputed once per control block, not per sample.
// ============================================================================
struct SvfFilter {
  double g = 0.0;       // [dsp] tan(pi*fc/fs), control-rate
  double Reff = 1.0;    // [dsp] control-rate damping, see the class comment
  double d = 1.0;       // [dsp] 1/(1+2*Reff*g+g*g), control-rate reciprocal (G4.11)
  double twoReffPlusG = 0.0;  // [dsp] cached (2*Reff+g), control-rate

  double ic1 = 0.0, ic2 = 0.0;  // [dsp] TPT integrator state (bp/lp memories)

  double peakBpPrev = 0.0;   // [dsp] peak |bp| over the PREVIOUS control block (feeds Reff)
  double peakBpAccum = 0.0;  // [dsp] peak |bp| accumulated over THIS (in-progress) control block

  // [voicing] DESIGN.md §5.3: R0 mapping constant (res=100 -> R0=-0.005) and
  // the damping-regulation gain. Sets the self-oscillation limit amplitude to
  // exactly sqrt(-R0/kSvfSat) = 0.5 at full resonance (class comment above).
  static constexpr double kR0Scale = 1.005;
  static constexpr double kSvfSat = 0.02;

  void reset() {
    ic1 = 0.0;
    ic2 = 0.0;
    peakBpPrev = 0.0;
    peakBpAccum = 0.0;
  }

  /// Control-rate coefficient update (DESIGN.md §2/§5.3): call once per
  /// control block, never per sample. `fc` is the already-clamped cutoff in
  /// Hz (see LadderFilter::setControlRate's own comment on this);
  /// `resonancePercent` is 0..100. Captures the PREVIOUS block's accumulated
  /// peak |bp| into peakBpPrev before resetting the accumulator for the new
  /// block, exactly matching the "previous control block" wording of
  /// DESIGN.md §5.3.
  void setControlRate(double fc, double fs, double resonancePercent) {
    peakBpPrev = peakBpAccum;
    peakBpAccum = 0.0;

    g = std::tan(kAmpPi * std::clamp(fc, 10.0, 0.45 * fs) / fs);  // [dsp] DESIGN.md §5.3/§5.4
    const double R0 = 1.0 - kR0Scale * (resonancePercent * 0.01);  // [voicing] DESIGN.md §5.3
    Reff = std::clamp(R0 + kSvfSat * peakBpPrev * peakBpPrev, -0.02, 2.0);  // [voicing]
    twoReffPlusG = 2.0 * Reff + g;
    d = 1.0 / (1.0 + 2.0 * Reff * g + g * g);  // [dsp] THE control-rate division (G4.11)
  }

  // R12/G4.11: no division, no transcendental, no atomic load anywhere in
  // this function -- see LadderFilter::process's identical note above.
  // ---- PER-SAMPLE PROCESS BEGIN ----
  inline double process(double x) {
    const double hp = (x - twoReffPlusG * ic1 - ic2) * d;
    const double bp = g * hp + ic1;
    ic1 = g * hp + bp;
    const double lp = g * bp + ic2;
    ic2 = g * bp + lp;
    const double absBp = bp < 0.0 ? -bp : bp;
    if (absBp > peakBpAccum) peakBpAccum = absBp;
    return lp;
  }
  // ---- PER-SAMPLE PROCESS END ----
};
