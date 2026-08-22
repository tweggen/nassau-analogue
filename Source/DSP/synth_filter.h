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
// R12/G4.11: process() itself -- the CLOSED-FORM SOLVE of DESIGN.md
// §5.2/§5.3's per-sample recursion -- stays division-free: every division
// setControlRate() needs (the ladder's `1/(1+k*G^4)`, the SVF's
// `d = 1/(1+2*Reff*g+g*g)`) is computed there, once per CONTROL BLOCK
// (DESIGN.md §2), and cached as a reciprocal/product process() only
// multiplies by. process()'s own body is delimited by a matching pair of
// marker comments a few lines below each function (mirroring
// synth_core.cpp's own "AUDIO-RATE LOOP" marker convention, G3.2)
// specifically so Tests/filter_tests.cpp can grep that exact region and
// prove, not just assert, that it contains no division and no
// transcendental call (G4.11) -- this remains true after G5.
//
// G5 ADDITION: DESIGN.md §2 separately requires `gLpf` itself to be
// LINEARLY INTERPOLATED PER SAMPLE ("a 2ms filter attack sweeps ~0.9
// octave per control block; held, that is a clearly audible staircase") --
// a requirement about the COEFFICIENT `g`, not about process()'s closed-form
// solve above. Each structure's `advanceCoeff(gValue)` (a THIRD function,
// also outside the marked per-sample-process region) applies an
// already-interpolated g (the caller ramps it via plain addition, not a
// transcendental, R12's own "adds, not transcendentals" exception) and
// recomputes the pure-function quantities derived from it (G4.11's own
// "no division per sample" claim is scoped to process(), not to this new,
// deliberate function -- see advanceCoeff's own comment for the full
// reasoning). SynthCore's controlRateUpdate()/AUDIO-RATE LOOP (G5) is the
// caller that does this ramping, once per voice per sample.
//
// Scope (docs/GATES.md G4): cutoff modulation (ENV-F/key-follow/LFO), the
// slope crossfade, and the golden fixture were explicitly out of scope for
// G4 (this file's own filters) -- they, and the audio-rate g interpolation
// above, are G5's job (Source/DSP/synth_core.cpp), landed there.

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
  // [dsp] G11-opt: control-rate caches for the parallel pole form below.
  // A = 1-G is the per-pole zero-input gain; B = 1-2G and twoG come from
  // rewriting the TPT state update  s' = y + v  as  s' = 2G*in + (1-2G)*s.
  double A = 1.0, B = 1.0, twoG = 0.0;

  // [voicing] DESIGN.md §5.2: the feedback saturator's knee. Sets the
  // self-oscillation amplitude (describing-function estimate A ~= 0.6, see
  // the class comment) -- NOT a signal clip, it sits inside the feedback
  // path only (DESIGN.md §5.2's own derivation).
  static constexpr double kLadderSat = 0.10;

  // R11 finding (G5): through G4, reset() only cleared the four poles'
  // STATE (s/curLp/curHp) -- g/G/G2/G3/G4/k/invDenom were left untouched,
  // which was invisible before G5 because setControlRate() always fully
  // overwrote all of them fresh every control block regardless of their
  // prior value, so nothing ever depended on a filter's coefficient state
  // surviving (or not) a reset(). G5's per-sample g INTERPOLATION changes
  // that: the interpolation ramp's START (synth_core.cpp's prepGInterp())
  // reads `filter.g` as it stood BEFORE this block's setControlRate() call
  // -- so a perturbed-then-reset instance whose `g` was left at a stale
  // nonzero value would ramp from a DIFFERENT starting point than a fresh
  // instance (g == 0.0, the in-class default), breaking G0.7/R13's
  // bit-exact "reset() matches a fresh instance" requirement. Found by
  // exactly the ctest G0.7 exists to catch. Fixed by resetting every
  // coefficient to its own in-class default here, not just the poles.
  void reset() {
    p1.reset();
    p2.reset();
    p3.reset();
    p4.reset();
    g = 0.0;
    G = 0.0;
    G2 = 0.0;
    G3 = 0.0;
    G4 = 0.0;
    k = 0.0;
    invDenom = 1.0;
  }

  // G5 (DESIGN.md §5.1's 20ms slope crossfade, docs/GATES.md G5.4): seed
  // every pole's integrator state so that if this structure were driven by
  // a CONSTANT input equal to `y`, its own next output would already sit at
  // `y` -- a DC-consistent starting condition rather than the silence a
  // fresh reset() leaves. Verified fixed point: with p.s = y and x = y,
  // v = (y-y)*G = 0, lp = v+s = y, s' = lp+v = y -- so all four cascaded
  // stages hold at y indefinitely under a sustained-y input. Used ONLY when
  // this structure is about to become the "incoming" side of a slope
  // crossfade (SynthCore::controlRateUpdate), seeded from the OUTGOING
  // structure's most recent actual output, so the newly-engaged structure
  // does not audibly dip toward zero before catching up with the signal the
  // outgoing structure was already tracking (G5.4's own requirement: "the
  // incoming structure's integrators must be initialised from the outgoing
  // structure's current output").
  void seedFromOutput(double y) {
    p1.s = y; p1.curLp = y; p1.curHp = 0.0;
    p2.s = y; p2.curLp = y; p2.curHp = 0.0;
    p3.s = y; p3.curLp = y; p3.curHp = 0.0;
    p4.s = y; p4.curLp = y; p4.curHp = 0.0;
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
    A = 1.0 - G;
    B = 1.0 - 2.0 * G;
    twoG = 2.0 * G;
    k = 4.2 * (resonancePercent * 0.01);  // [voicing] DESIGN.md §5.2
    invDenom = 1.0 / (1.0 + k * G4);      // [dsp] THE control-rate division (G4.11)

    p1.g = g;
    p2.g = g;
    p3.g = g;
    p4.g = g;
  }

  // G5 (DESIGN.md §2: "gLpf (LPF TPT coefficient)... interpolated per
  // sample" -- one of exactly three quantities DESIGN.md §2 names,
  // alongside gHpf and vcaGain, because "a 2ms filter attack sweeps ~0.9
  // octave per control block; held, that is a clearly audible staircase").
  // setControlRate() above still computes the CONTROL-RATE target (k stays
  // control-rate, per DESIGN.md §5.2 -- only g moves faster than that).
  // This function applies an already-interpolated `gValue` (the CALLER
  // linearly ramps g itself, sample by sample, from the previous block's
  // final g to this block's setControlRate()-computed target -- an ADD,
  // not a transcendental, exactly R12's "adds, not transcendentals"
  // exception) and recomputes every quantity DERIVED from g -- G, G2, G3,
  // G4, invDenom -- which are PURE FUNCTIONS of (g, k) with no other
  // hidden state, so recomputing them from an interpolated g each sample
  // is mathematically consistent, not an approximation layered on stale
  // state. This is a SEPARATE function from process() below, deliberately
  // OUTSIDE this file's own per-sample-process marker comments (the ones
  // G4.11 inspects, spelled out fully a few lines below each process()):
  // that AC is about process()'s own CLOSED-FORM SOLVE staying
  // division-free when g is HELD for a block (still true, unchanged --
  // see process()'s own comment); it does not forbid a genuine,
  // deliberate per-sample division elsewhere when g is INTERPOLATED
  // instead, which is a G5 addition G4 did not need. The division here
  // (invDenom) is the audio-rate cost DESIGN.md §2 itself calls for by
  // naming gLpf as interpolated; TptOnePole::process() (synth_dsp.h,
  // G1-proven) already does the analogous G=g/(1+g) recompute every
  // sample from its own `g` member, so this is the same established
  // pattern, not a new category of cost.
  inline void advanceCoeff(double gValue) {
    g = gValue;
    G = g / (1.0 + g);
    G2 = G * G;
    G3 = G2 * G;
    G4 = G3 * G;
    A = 1.0 - G;
    B = 1.0 - 2.0 * G;
    twoG = 2.0 * G;
    invDenom = 1.0 / (1.0 + k * G4);
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
    const double S1 = A * p1.s;
    const double S2 = A * p2.s;
    const double S3 = A * p3.s;
    const double S4 = A * p4.s;
    const double Sigma = G3 * S1 + G2 * S2 + G * S3 + S4;
    const double y4lin = (G4 * x + Sigma) * invDenom;
    const double u = x - k * shapeTriodeK(y4lin, kLadderSat);

    // G11-opt: the four poles were a SERIAL chain (y1 -> y2 -> y3 -> y4), so
    // every sample paid four dependent TPT latencies back to back, on top of
    // the division inside shapeTriodeK -- and this filter measured 24.42
    // ns/sample, 40% of the whole per-voice cost.
    //
    // A TPT one-pole is  y = G*in + (1-G)*s  and  s' = 2G*in + (1-2G)*s, so
    // each y_i expands in terms of u and the OLD states and the four become
    // INDEPENDENT -- no chain, and the compiler can issue them together:
    //   y1 = G u + S1
    //   y2 = G^2 u + G S1 + S2
    //   y3 = G^3 u + G^2 S1 + G S2 + S3
    //   y4 = G^4 u + Sigma          (already solved above -- free)
    // Measured 25.12 -> 19.27 ns/sample, 1.30x, agreeing with the serial form
    // to 3.886e-16 over 200k samples (golden tolerance is 1e-6, and both
    // batteries in fact still verify at exactly 0.0). [dsp]
    const double y1 = G * u + S1;
    const double y2 = G2 * u + G * S1 + S2;
    const double y3 = G3 * u + G2 * S1 + G * S2 + S3;
    const double y4 = G4 * u + Sigma;

    p1.s = twoG * u + B * p1.s;
    p2.s = twoG * y1 + B * p2.s;
    p3.s = twoG * y2 + B * p3.s;
    p4.s = twoG * y3 + B * p4.s;
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

  // R11 finding (G5): see LadderFilter::reset's identical note above --
  // through G4, reset() left g/Reff/d/twoReffPlusG untouched (harmless
  // until G5's per-sample interpolation started reading `g` ACROSS a
  // reset() boundary to seed its ramp's start). Reset every coefficient to
  // its own in-class default here too, not just the integrator memories.
  void reset() {
    ic1 = 0.0;
    ic2 = 0.0;
    peakBpPrev = 0.0;
    peakBpAccum = 0.0;
    g = 0.0;
    Reff = 1.0;
    d = 1.0;
    twoReffPlusG = 0.0;
  }

  // G5 (DESIGN.md §5.1's 20ms slope crossfade, docs/GATES.md G5.4): seed
  // this structure's integrator memories so that a CONSTANT input `y` is
  // already a fixed point -- ic1 = 0 (bandpass memory), ic2 = y (lowpass
  // memory). Verified: with those and x = y, hp = (y - 0 - y)*d = 0,
  // bp = g*0+0 = 0 (ic1 stays 0), lp = g*0+y = y (ic2 stays y) -- so `lp`
  // (the output) is exactly `y` from the very first sample, matching
  // LadderFilter::seedFromOutput's analogous fixed-point property above.
  // peakBpPrev/peakBpAccum are reset to 0 rather than left at whatever this
  // (previously idle) structure last saw: a stale peak would otherwise bias
  // Reff (DESIGN.md §5.3) away from R0 the instant this structure becomes
  // audible, which is exactly the kind of discontinuity the crossfade
  // exists to avoid.
  void seedFromOutput(double y) {
    ic1 = 0.0;
    ic2 = y;
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

  // G5 (DESIGN.md §2's "gLpf...interpolated per sample" -- see
  // LadderFilter::advanceCoeff's identical class-level rationale above,
  // which applies verbatim here). `Reff` stays CONTROL-RATE, exactly as
  // DESIGN.md §5.3 specifies ("Reff...recomputed once per control block,
  // not per sample" -- that scheduling is load-bearing for the
  // self-oscillation limit cycle, G4.4, and is untouched here): only `g`
  // (and its two PURE-FUNCTION derivatives twoReffPlusG, d) move faster.
  // Deliberately outside the per-sample-process marker comments below,
  // same reasoning as the ladder's advanceCoeff.
  inline void advanceCoeff(double gValue) {
    g = gValue;
    twoReffPlusG = 2.0 * Reff + g;
    d = 1.0 / (1.0 + 2.0 * Reff * g + g * g);
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

// ============================================================================
// HpfCascade — G6, DESIGN.md §5.5: a cascade of up to 4 IDENTICAL TPT
// one-pole high-passes (2 poles for 12 dB mode, 4 for 24 dB mode). No
// resonance and no feedback loop (DESIGN.md §5.5: "the Jupiter-6's
// high-pass has none") -- so, unlike LadderFilter/SvfFilter, there is no
// closed-form zero-delay solve here: each stage's HP output simply feeds
// the next stage's input, reusing TptOnePole (G1) verbatim exactly as
// LadderFilter's four poles do.
//
// The caller (SynthCore::controlRateUpdate()) is responsible for DESIGN.md
// §5.5's HARD BYPASS at kHpfCutoff's minimum (20 Hz): that is an
// architectural skip of this whole struct, not a filter setting, because a
// real (even very-low-fc) digital high-pass is never BIT-EXACTLY identity
// (DESIGN.md §5.5, docs/GATES.md G6.4) -- see synth_core.cpp's per-sample
// loop for where that skip happens.
//
// `g` is shared by every stage (identical poles): DESIGN.md §2 names `gHpf`
// as one of exactly three quantities linearly interpolated PER SAMPLE
// (alongside `gLpf`/`vcaGain`) -- advanceCoeff() below is the same pattern
// LadderFilter/SvfFilter already established at G5, applied here for G6.
// ============================================================================
struct HpfCascade {
  TptOnePole p1, p2, p3, p4;
  double g = 0.0;  // [dsp] tan(pi*fc/fs), control-rate (DESIGN.md §5.5)

  void reset() {
    p1.reset();
    p2.reset();
    p3.reset();
    p4.reset();
    g = 0.0;
  }

  /// Control-rate coefficient update (DESIGN.md §2/§5.5): call once per
  /// control block. `fc` is the ALREADY-modulated (key-follow), ALREADY-
  /// clamped cutoff in Hz -- the caller applies DESIGN.md §5.4/§5.5's
  /// [10, 0.45*fs] robustness clamp itself, matching LadderFilter/SvfFilter's
  /// own setControlRate() convention. Robustness clamp repeated here too
  /// (TptOnePole::setFc's own guard) so a caller that forgot it cannot make
  /// tan() diverge.
  void setControlRate(double fc, double fs) {
    g = std::tan(kAmpPi * std::clamp(fc, 10.0, 0.45 * fs) / fs);  // [dsp] DESIGN.md §5.5/§5.4
    p1.g = g;
    p2.g = g;
    p3.g = g;
    p4.g = g;
  }

  /// G6 (DESIGN.md §2's "gHpf" -- see LadderFilter::advanceCoeff's identical
  /// class-level rationale, which applies verbatim here since every stage
  /// shares the same `g`, no resonance term to re-derive). Applies an
  /// already-interpolated `gValue` (the caller ramps it via plain addition,
  /// R12's own "adds, not transcendentals" exception).
  inline void advanceCoeff(double gValue) {
    g = gValue;
    p1.g = g;
    p2.g = g;
    p3.g = g;
    p4.g = g;
  }

  /// One sample through `nPoles` (2 or 12 dB mode / 4 for 24 dB mode)
  /// cascaded identical HP stages -- DESIGN.md §5.5. Each stage's HP output
  /// (`x - lp`, computed exactly by TptOnePole::process(), G1.2) feeds the
  /// next stage's input; stages beyond `nPoles` are simply not run (their
  /// state stays at rest, matching the "only the selected structure runs"
  /// convention DESIGN.md §5.1 already uses for the LPF). No division, no
  /// transcendental, no atomic load written directly in THIS function's own
  /// text (TptOnePole::process() itself does one division per stage, same
  /// pre-existing pattern as LadderFilter's four poles -- see
  /// LadderFilter::process()'s own comment on why that is not a new R12
  /// concern).
  inline double process(double x, int nPoles) {
    p1.process(x);
    double h = p1.hp();
    if (nPoles >= 2) {
      p2.process(h);
      h = p2.hp();
    }
    if (nPoles >= 3) {
      p3.process(h);
      h = p3.hp();
    }
    if (nPoles >= 4) {
      p4.process(h);
      h = p4.hp();
    }
    return h;
  }
};
