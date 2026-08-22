#pragma once

// synth_simd.h — NassauAnalogue SIMD-over-voices, DESIGN.md §12.3 (the
// third and last of §12's escape hatches; §12.1 logs the first two --
// fastTan/fastExp2 and a wider control block -- as measured/rejected).
//
// Header-only, dependency-free (R2, amended to permit arch intrinsics --
// see synth_dsp.h's own ScopedNoDenormals for the precedent this follows):
// this file may include only <algorithm> <cmath>, this directory's own
// sibling headers synth_dsp.h/synth_filter.h (both R2-legal), and the
// compiler's own SSE2 intrinsic header on x86/x64. No SDK/IPlug2/framework
// dependency, ever.
//
// SCOPE (deliberately narrow): x86-64's baseline ISA already includes SSE2,
// so this needs no -march flag (R7 forbids -march=native/-ffast-math, not a
// compile-time #ifdef'd arch path with a portable fallback). Two backends:
//
//   * SSE2, 2-wide `double` (__m128d) -- the shipped path, x86-64.
//   * A plain two-`double` scalar fallback -- used whenever SSE2 is not
//     available OR the build defines NASSAU_NO_SIMD (an explicit escape
//     hatch, verified by its own CMake option/CI leg) OR the build defines
//     NASSAU_DSP_FLOAT (§12.2's opt-in weak-machine float path: `nassau_real`
//     is `float` there, and this file's Vec2d is deliberately always
//     `double` -- see the "why not fold into NASSAU_DSP_FLOAT" note below).
//
// AArch64/NEON is NOT implemented here (a deliberate scope cut mid-gate,
// see docs/DESIGN.md §12.3's own note): there is no ARM hardware in this
// project's dev environment, so a NEON kernel would be unverifiable,
// unbenchmarked and unexercised by any test here -- exactly the class of
// speculative code this project has been strict about (§12.2's float path
// is the cautionary precedent: written for ARM, measured a REGRESSION on
// the only hardware available, and its actual ARM benefit is still
// unmeasured). Instead, the vector operations below are behind the small,
// named Vec2d abstraction so a `float64x2_t` backend is a THIRD branch to
// fill in later, not a restructure -- see Vec2d's own comment for exactly
// which operations that branch would need to implement.
//
// BIT-EXACTNESS (the load-bearing property this whole file exists to
// preserve, DESIGN.md §12.3): every function below is a hand-transliteration
// of an ALREADY-PROVEN scalar recursion (LadderFilter/SvfFilter/HpfCascade/
// OnePoleHP, Source/DSP/synth_filter.h + synth_dsp.h), term for term, in the
// SAME left-to-right operator order, with NO horizontal reduction and NO
// fused multiply-add:
//   - SSE2 addpd/subpd/mulpd/divpd perform per-lane IEEE-754 round-to-nearest
//     arithmetic, bit-identical to the equivalent SSE2 SCALAR instruction
//     (addsd/mulsd/...) on the same operands -- there is exactly one
//     IEEE-754-double adder/multiplier/divider in the FPU pipeline either
//     way, packed or scalar, so there is nothing here that could round
//     differently between the two.
//   - SSE2 has no FMA instruction at all, so there is no contraction risk
//     from the intrinsics themselves; the surrounding build also never
//     passes -mfma/-march=native (R7), so the compiler cannot synthesize one
//     either. Two-operand mul-then-add (never a single fused op) is used
//     throughout, exactly mirroring the scalar originals' own `a*b + c`
//     shape.
// This is verified positively, not just argued: both golden batteries
// (Tests/synth_golden.cpp, docs/GATES.md G5.7/G11.1) verify at EXACTLY
// 0.000e+00 max-abs-error with this path live -- not a tolerance, an exact
// match against the pre-SIMD scalar rendering.
//
// WHAT IS VECTORIZED, AND WHAT DELIBERATELY IS NOT (DESIGN.md §12's own
// per-component cost table): LadderFilter (19.3 ns/voice, the cost centre),
// SvfFilter (7.7 ns, only one slope runs at a time), HpfCascade (4.2-5.6 ns)
// and both OnePoleHP DC blockers (4.3 ns combined) -- together ~27.8 of the
// measured 56.1 ns/voice marginal cost. Oscillators, the sub, the noise
// source and the drive stage stay scalar: PolyBLEP is branchy and a poor
// SIMD fit, and DESIGN.md §12 already measured all four at ~2 ns each --
// not worth the complexity here.
//
// PAIRING (Source/DSP/synth_core.cpp): coefficients differ per lane (key
// follow, velocity->filter and ENV-F make cutoff per-voice, DESIGN.md §5.4),
// so every Vec2d this file builds from two chains' filters carries genuinely
// independent per-lane values -- nothing here is a broadcast-and-hope.
//
// Why this stays a SEPARATE type from `nassau_real` (synth_dsp.h) rather
// than folding into it: `nassau_real` is a per-BUILD width switch (double
// normally, float only under NASSAU_DSP_FLOAT, DESIGN.md §12.2) baked into
// every filter struct's OWN fields. Vec2d is a per-CALL SIMD-over-TWO-
// VOICES facility that reads/writes those same `nassau_real` fields from
// the outside, two chains at a time -- it only has a well-defined 2-wide
// `double` intrinsic backend, so it is compiled and used ONLY when
// `nassau_real == double` (i.e. NASSAU_DSP_FLOAT is off): see
// Source/DSP/synth_core.cpp's own `#if !defined(NASSAU_DSP_FLOAT)` guard
// around every call site in this file. Under NASSAU_DSP_FLOAT the ARM/weak-
// machine float path already has its own (different, 4-wide-capable, still
// unmeasured on real hardware) reason to exist per DESIGN.md §12.2 and is
// explicitly out of this gate's scope -- that build renders every voice via
// the ordinary scalar per-chain path, unchanged, so it cannot bit-rot.

#include "synth_dsp.h"
#include "synth_filter.h"

#include <algorithm>
#include <cmath>

// ---- Backend selection ----------------------------------------------------
// NASSAU_NO_SIMD is the R7 "portable fallback" escape hatch (a CMake option,
// root CMakeLists.txt): with it defined, Vec2d compiles to two plain
// `double`s and plain scalar arithmetic -- no intrinsics, no arch header --
// so it also doubles as the fallback for any non-x86 target this project
// does not have an intrinsic backend for yet (see the NEON note above).
//
// MSVC never defines __SSE2__ even when SSE2 is the compile target (it is
// baseline on x64, and /arch:SSE2 sets _M_IX86_FP==2 on x86) -- so the
// detection below checks the MSVC-specific macros too, matching
// synth_dsp.h's ScopedNoDenormals precedent for "how this project detects
// x86 SIMD availability across GCC/Clang and MSVC".
#if defined(NASSAU_NO_SIMD)
#define NASSAU_SIMD_SSE2 0
#elif defined(__SSE2__) || defined(_M_X64) || defined(__x86_64__) || \
    (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define NASSAU_SIMD_SSE2 1
#else
#define NASSAU_SIMD_SSE2 0
#endif

#if NASSAU_SIMD_SSE2
#include <emmintrin.h>
#endif

// ============================================================================
// Vec2d — exactly two IEEE-754 doubles, lane0/lane1, processed together.
// The ENTIRE surface a future NEON (`float64x2_t`) backend would need to
// fill in is: a 2-arg constructor (lane0, lane1), set1(), lane0()/lane1()
// extraction, absv(), maxv(), and operator+ - * /. Nothing outside this
// struct's body (LadderFilter/SvfFilter/HpfCascade/OnePoleHP pair functions
// below) would need to change -- they are written entirely in terms of this
// surface.
// ============================================================================
struct Vec2d {
#if NASSAU_SIMD_SSE2
  __m128d v;

  Vec2d() : v(_mm_setzero_pd()) {}
  Vec2d(double lane0In, double lane1In) : v(_mm_set_pd(lane1In, lane0In)) {}  // _mm_set_pd(hi, lo)
  explicit Vec2d(__m128d raw) : v(raw) {}

  static Vec2d set1(double x) { return Vec2d(_mm_set1_pd(x)); }

  double lane0() const { return _mm_cvtsd_f64(v); }
  double lane1() const { return _mm_cvtsd_f64(_mm_unpackhi_pd(v, v)); }

  // Sign-bit clear, exactly what std::fabs does for every finite/inf/nan
  // double per IEEE-754 (no rounding involved) -- bit-identical to the
  // scalar shapeTriodeK's own std::fabs(x) call.
  Vec2d absv() const {
    const __m128d signMask = _mm_set1_pd(-0.0);
    return Vec2d(_mm_andnot_pd(signMask, v));
  }

  static Vec2d maxv(Vec2d a, Vec2d b) { return Vec2d(_mm_max_pd(a.v, b.v)); }

  friend Vec2d operator+(Vec2d a, Vec2d b) { return Vec2d(_mm_add_pd(a.v, b.v)); }
  friend Vec2d operator-(Vec2d a, Vec2d b) { return Vec2d(_mm_sub_pd(a.v, b.v)); }
  friend Vec2d operator*(Vec2d a, Vec2d b) { return Vec2d(_mm_mul_pd(a.v, b.v)); }
  friend Vec2d operator/(Vec2d a, Vec2d b) { return Vec2d(_mm_div_pd(a.v, b.v)); }
#else
  // Portable fallback (R7): two independent doubles, plain scalar ops. This
  // is not an approximation of the SSE2 path -- it is LITERALLY what running
  // the original scalar filter code twice (once per lane) computes, so it is
  // bit-exact by construction, just without the actual SIMD instructions.
  double x0 = 0.0, x1 = 0.0;

  Vec2d() = default;
  Vec2d(double lane0In, double lane1In) : x0(lane0In), x1(lane1In) {}

  static Vec2d set1(double x) { return Vec2d(x, x); }

  double lane0() const { return x0; }
  double lane1() const { return x1; }

  Vec2d absv() const { return Vec2d(std::fabs(x0), std::fabs(x1)); }

  static Vec2d maxv(Vec2d a, Vec2d b) { return Vec2d(std::max(a.x0, b.x0), std::max(a.x1, b.x1)); }

  friend Vec2d operator+(Vec2d a, Vec2d b) { return Vec2d(a.x0 + b.x0, a.x1 + b.x1); }
  friend Vec2d operator-(Vec2d a, Vec2d b) { return Vec2d(a.x0 - b.x0, a.x1 - b.x1); }
  friend Vec2d operator*(Vec2d a, Vec2d b) { return Vec2d(a.x0 * b.x0, a.x1 * b.x1); }
  friend Vec2d operator/(Vec2d a, Vec2d b) { return Vec2d(a.x0 / b.x0, a.x1 / b.x1); }
#endif
};

// shapeTriodeK (synth_dsp.h), lane-wise: x / (1 + k*|x|). Mirrors the scalar
// free function term for term, including its operator order.
inline Vec2d shapeTriodeKVec(Vec2d x, Vec2d k) { return x / (Vec2d::set1(1.0) + k * x.absv()); }

// ============================================================================
// onePoleHpPairProcess — two OnePoleHP instances (synth_dsp.h), one sample
// each, in lockstep. Mirrors OnePoleHP::process() exactly:
//   y = x - x1 + R*y1 ;  x1 = x ;  y1 = y
// `R` is a fixed per-instance coefficient (both DC blockers are configured
// once, at init()/reset(), to the same 5 Hz corner at the same fs -- see
// Voice::Chain::dcBlock/postLpfDcBlock's own field comments -- so reading it
// per-lane here rather than assuming it is shared costs nothing and stays
// correct even if that ever changes).
// ============================================================================
inline void onePoleHpPairProcess(OnePoleHP& a, OnePoleHP& b, double xA, double xB, double& yA,
                                  double& yB) {
  const Vec2d R(a.R, b.R);
  const Vec2d x1(a.x1, b.x1);
  const Vec2d y1(a.y1, b.y1);
  const Vec2d x(xA, xB);
  const Vec2d y = x - x1 + R * y1;

  a.x1 = xA;
  b.x1 = xB;
  a.y1 = y.lane0();
  b.y1 = y.lane1();
  yA = y.lane0();
  yB = y.lane1();
}

// ============================================================================
// hpfCascadePairProcess — two HpfCascade instances (synth_filter.h), one
// sample each. Mirrors HpfCascade::advanceCoeff() + process(x, nPoles)
// exactly, stage by stage, feeding each stage's `x - lp` output into the
// next (TptOnePole::process()'s own recursion, synth_dsp.h):
//   G = g/(1+g) ;  v = (x-s)*G ;  lp = v+s ;  s = lp+v ;  hp = x-lp
// `G` depends only on the shared `g` (identical across all 4 poles of a
// cascade, HpfCascade::setControlRate()/advanceCoeff()'s own invariant), so
// it is computed ONCE per pair-call and reused across stages -- this is not
// an approximation, it is the SAME redundant-but-identical division the
// scalar TptOnePole::process() already performs once per stage on the
// bit-identical `g`, deduplicated (division is a deterministic function of
// its bit-pattern inputs; computing it once instead of four times on the
// same inputs cannot change the result). Only `nPoles` (2 or 4, DESIGN.md
// §5.5) stages actually run, mirroring HpfCascade::process()'s own "stages
// beyond nPoles are simply not run".
// ============================================================================
inline void hpfCascadePairProcess(HpfCascade& a, HpfCascade& b, double gValueA, double gValueB,
                                   double xA, double xB, int nPoles, double& hOutA, double& hOutB) {
  const Vec2d g(gValueA, gValueB);
  const Vec2d G = g / (Vec2d::set1(1.0) + g);

  a.g = gValueA;
  b.g = gValueB;

  Vec2d h(xA, xB);

  {
    Vec2d s(a.p1.s, b.p1.s);
    const Vec2d vv = (h - s) * G;
    const Vec2d lp = vv + s;
    s = lp + vv;
    h = h - lp;
    a.p1.s = s.lane0();
    b.p1.s = s.lane1();
    a.p1.g = gValueA;
    a.p1.setG();
    b.p1.g = gValueB;
    b.p1.setG();
  }
  if (nPoles >= 2) {
    Vec2d s(a.p2.s, b.p2.s);
    const Vec2d vv = (h - s) * G;
    const Vec2d lp = vv + s;
    s = lp + vv;
    h = h - lp;
    a.p2.s = s.lane0();
    b.p2.s = s.lane1();
    a.p2.g = gValueA;
    a.p2.setG();
    b.p2.g = gValueB;
    b.p2.setG();
  }
  if (nPoles >= 3) {
    Vec2d s(a.p3.s, b.p3.s);
    const Vec2d vv = (h - s) * G;
    const Vec2d lp = vv + s;
    s = lp + vv;
    h = h - lp;
    a.p3.s = s.lane0();
    b.p3.s = s.lane1();
    a.p3.g = gValueA;
    a.p3.setG();
    b.p3.g = gValueB;
    b.p3.setG();
  }
  if (nPoles >= 4) {
    Vec2d s(a.p4.s, b.p4.s);
    const Vec2d vv = (h - s) * G;
    const Vec2d lp = vv + s;
    s = lp + vv;
    h = h - lp;
    a.p4.s = s.lane0();
    b.p4.s = s.lane1();
    a.p4.g = gValueA;
    a.p4.setG();
    b.p4.g = gValueB;
    b.p4.setG();
  }

  hOutA = h.lane0();
  hOutB = h.lane1();
}

// ============================================================================
// ladderPairProcess — two LadderFilter instances (synth_filter.h), one
// sample each. Mirrors LadderFilter::advanceCoeff() + process() EXACTLY,
// term for term, same left-to-right order:
//
//   G = g/(1+g) ; G2=G*G ; G3=G2*G ; G4=G3*G
//   A = 1-G ; B = 1-2*G ; twoG = 2*G ; invDenom = 1/(1+k*G4)
//   S1=A*p1.s ; S2=A*p2.s ; S3=A*p3.s ; S4=A*p4.s
//   Sigma = G3*S1 + G2*S2 + G*S3 + S4
//   y4lin = (G4*x + Sigma) * invDenom
//   u = x - k*shapeTriodeK(y4lin, kLadderSat)     -- kLadderSat, NOT k, is
//                                                     the shaper's own knee
//   y1 = G*u+S1 ; y2 = G2*u+G*S1+S2 ; y3 = G3*u+G2*S1+G*S2+S3 ; y4 = G4*u+Sigma
//   p1.s = twoG*u+B*p1.s ; p2.s = twoG*y1+B*p2.s
//   p3.s = twoG*y2+B*p3.s ; p4.s = twoG*y3+B*p4.s
//
// Only `g` and the four pole states (`p1..p4.s`) are written back: every
// other quantity here (G/G2/G3/G4/A/B/twoG/invDenom) is a PURE function of
// (g, k) recomputed fresh by the very next call -- scalar advanceCoeff() or
// this function -- regardless of what it was left at, and nothing outside
// LadderFilter::process()/advanceCoeff() itself ever reads them (verified:
// no read site outside synth_filter.h). `k` (resonance) is read per-lane
// from each instance's own field, not assumed shared, matching this file's
// "coefficients are vectors, not broadcasts" scope note.
// ============================================================================
inline void ladderPairProcess(LadderFilter& a, LadderFilter& b, double gValueA, double gValueB,
                               double xA, double xB, double& yA, double& yB) {
  const Vec2d one = Vec2d::set1(1.0);
  const Vec2d two = Vec2d::set1(2.0);
  const Vec2d g(gValueA, gValueB);
  const Vec2d G = g / (one + g);
  const Vec2d G2 = G * G;
  const Vec2d G3 = G2 * G;
  const Vec2d G4 = G3 * G;
  const Vec2d A = one - G;
  const Vec2d B = one - two * G;
  const Vec2d twoG = two * G;
  const Vec2d k(a.k, b.k);
  const Vec2d invDenom = one / (one + k * G4);

  a.g = gValueA;
  b.g = gValueB;

  const Vec2d p1s(a.p1.s, b.p1.s);
  const Vec2d p2s(a.p2.s, b.p2.s);
  const Vec2d p3s(a.p3.s, b.p3.s);
  const Vec2d p4s(a.p4.s, b.p4.s);

  const Vec2d S1 = A * p1s;
  const Vec2d S2 = A * p2s;
  const Vec2d S3 = A * p3s;
  const Vec2d S4 = A * p4s;
  const Vec2d Sigma = G3 * S1 + G2 * S2 + G * S3 + S4;

  const Vec2d x(xA, xB);
  const Vec2d y4lin = (G4 * x + Sigma) * invDenom;
  const Vec2d sat = shapeTriodeKVec(y4lin, Vec2d::set1(LadderFilter::kLadderSat));
  const Vec2d u = x - k * sat;

  const Vec2d y1 = G * u + S1;
  const Vec2d y2 = G2 * u + G * S1 + S2;
  const Vec2d y3 = G3 * u + G2 * S1 + G * S2 + S3;
  const Vec2d y4 = G4 * u + Sigma;

  const Vec2d np1s = twoG * u + B * p1s;
  const Vec2d np2s = twoG * y1 + B * p2s;
  const Vec2d np3s = twoG * y2 + B * p3s;
  const Vec2d np4s = twoG * y3 + B * p4s;

  a.p1.s = np1s.lane0();
  b.p1.s = np1s.lane1();
  a.p2.s = np2s.lane0();
  b.p2.s = np2s.lane1();
  a.p3.s = np3s.lane0();
  b.p3.s = np3s.lane1();
  a.p4.s = np4s.lane0();
  b.p4.s = np4s.lane1();

  yA = y4.lane0();
  yB = y4.lane1();
}

// ============================================================================
// svfPairProcess — two SvfFilter instances (synth_filter.h), one sample
// each. Mirrors SvfFilter::advanceCoeff() + process() exactly:
//
//   twoReffPlusG = 2*Reff + g ; d = 1/(1 + 2*Reff*g + g*g)
//   hp = (x - twoReffPlusG*ic1 - ic2) * d
//   bp = g*hp + ic1 ; ic1 = g*hp + bp
//   lp = g*bp + ic2 ; ic2 = g*bp + lp
//   peakBpAccum = max(peakBpAccum, |bp|)          -- running max, DESIGN.md §5.3
//
// `Reff` is control-rate (recomputed once per control block by
// setControlRate(), unchanged, scalar -- see Source/DSP/synth_core.cpp), so
// it is only READ here, per lane, never written. `ic1`/`ic2` (integrator
// memory) and `peakBpAccum` (the running max the NEXT control block's
// setControlRate() reads as "the previous block's peak") are the three
// quantities that must persist sample to sample, and are written back every
// call, exactly like the scalar process()'s own `ic1 = ...; ic2 = ...;
// peakBpAccum = ...` side effects.
// ============================================================================
inline void svfPairProcess(SvfFilter& a, SvfFilter& b, double gValueA, double gValueB, double xA,
                            double xB, double& yA, double& yB) {
  const Vec2d one = Vec2d::set1(1.0);
  const Vec2d two = Vec2d::set1(2.0);
  const Vec2d g(gValueA, gValueB);
  const Vec2d Reff(a.Reff, b.Reff);
  const Vec2d twoReffPlusG = two * Reff + g;
  const Vec2d d = one / (one + two * Reff * g + g * g);

  a.g = gValueA;
  b.g = gValueB;

  const Vec2d ic1(a.ic1, b.ic1);
  const Vec2d ic2(a.ic2, b.ic2);
  const Vec2d x(xA, xB);

  const Vec2d hp = (x - twoReffPlusG * ic1 - ic2) * d;
  const Vec2d bp = g * hp + ic1;
  const Vec2d nic1 = g * hp + bp;
  const Vec2d lp = g * bp + ic2;
  const Vec2d nic2 = g * bp + lp;

  a.ic1 = nic1.lane0();
  b.ic1 = nic1.lane1();
  a.ic2 = nic2.lane0();
  b.ic2 = nic2.lane1();

  const Vec2d absBp = bp.absv();
  const Vec2d peakPrev(a.peakBpAccum, b.peakBpAccum);
  const Vec2d newPeak = Vec2d::maxv(peakPrev, absBp);
  a.peakBpAccum = newPeak.lane0();
  b.peakBpAccum = newPeak.lane1();

  yA = lp.lane0();
  yB = lp.lane1();
}
