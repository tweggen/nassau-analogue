#pragma once

// synth_dsp.h — NassauAnalogue DSP primitives (Gate G1, DESIGN.md §2-§7).
//
// Header-only, dependency-free (R2): this file may include only <algorithm>,
// <cmath>, <cstdint>, plus the compiler's own arch-intrinsic header guarded
// on MSVC/x86 (matching nassau-zermatt/Source/DSP/amp_dsp.h's own R2
// footprint). Builds and unit-tests standalone, with NO SDK/IPlug2/framework
// dependency (DESIGN.md §0.4). Coefficient math and all internal state are
// `double`; the surrounding plugin's public I/O stays `float`.
//
// Six of the primitives below (OnePoleHP, OnePoleLP, SmoothedValue,
// ScopedNoDenormals, shapeTriodeK, shapeCubic) are lifted VERBATIM from
// nassau-zermatt/Source/DSP/amp_dsp.h -- they are already gate-proven and
// rewriting them is a defect, not an improvement (docs/GATES.md G1). Their
// original comments (including references to Zermatt's own DESIGN.md
// section numbers, e.g. "§1.5", "C2", "C3") are preserved unchanged as part
// of that verbatim lift; those section numbers belong to Zermatt's spec, not
// this one, and are kept because altering the comment text would no longer
// be a verbatim copy. Everything else in this file is new for
// NassauAnalogue (DESIGN.md §2 control-rate architecture, §5.2/§5.3 TPT
// filter structures, §3.1 polyBLEP, §3.5 noise, §6 envelope, §7 LFO).
//
// Explicitly NOT delivered here: fastTan, fastExp2 (DESIGN.md §2.1 -- a
// documented, deliberate non-goal; do not add "while we're here").
//
// [WEAK-MACHINE PATH, opt-in, DESIGN.md §12.2] `nassau_real` below is `double`
// by default (bit-identical to every gate through G11) and `float` only when
// the build is configured with `-DNASSAU_DSP_FLOAT=ON`. It is applied ONLY to
// OnePoleHP (the DC blockers) and TptOnePole (the filter primitive that backs
// LadderFilter/SvfFilter/HpfCascade, Source/DSP/synth_filter.h) -- the cost
// centre DESIGN.md §12 measured. OnePoleHP therefore is no longer a
// character-for-character verbatim copy of nassau-zermatt's amp_dsp.h (its
// field types are now `nassau_real`, not the literal token `double`); it
// remains behaviourally IDENTICAL to the verbatim original whenever
// `nassau_real` resolves to `double`, i.e. always in the default build.
// OnePoleLP/SmoothedValue/PinkFilter/AdsrEnv/Lfo/Xorshift32 -- and every
// oscillator phase accumulator in synth_osc.h -- stay plain `double`
// unconditionally: phase accumulators are excluded on principle (G2.8 needs
// 1e-9, float carries ~1e-7), and the others are not the measured cost centre
// (DESIGN.md §12.2).
#if defined(NASSAU_DSP_FLOAT)
using nassau_real = float;
#else
using nassau_real = double;
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>

// ============================================================================
// Verbatim lift from nassau-zermatt/Source/DSP/amp_dsp.h -- see this file's
// header comment above for why the section references below are unchanged.
// ============================================================================

// [dsp] standard math constant, double precision.
constexpr double kAmpPi = 3.14159265358979323846;

// ============================================================================
// OnePoleHP — DC blocker. y = x - x1 + R*y1, R = exp(-2*pi*fc/fs) (DESIGN.md
// §1.5, C2: the exp-form coefficient, NOT the naive Euler `2*pi*fc/fs`, which
// grows unstable once 2*pi*fc > fs). R is clamped to [0, 1-1e-7] — R=1 would
// be a non-decaying integrator memory (marginally unstable in floating
// point); R<0 has no physical meaning for a real fc>=0.
// ============================================================================
struct OnePoleHP {
  nassau_real R = 0.0;
  nassau_real x1 = 0.0, y1 = 0.0;

  // fc/fs stay `double` (control-rate/init-time only, R12 does not apply
  // here) -- only the coefficient's STORAGE and the per-sample process()
  // arithmetic below narrow to nassau_real (DESIGN.md §12.2).
  void setFc(double fc, double fs) {
    fc = std::clamp(fc, 0.0, 0.49 * fs);
    R = static_cast<nassau_real>(
        std::clamp(std::exp(-2.0 * kAmpPi * fc / fs), 0.0, 1.0 - 1e-7));  // [dsp] DESIGN.md C2
  }

  void reset() {
    x1 = 0.0;
    y1 = 0.0;
  }

  inline nassau_real process(nassau_real x) {
    const nassau_real y = x - x1 + R * y1;
    x1 = x;
    y1 = y;
    return y;
  }
};

// ============================================================================
// OnePoleLP — y += a*(x - y), a = 1 - exp(-2*pi*fc/fs) (DESIGN.md §1.5, same
// exp-form family as OnePoleHP/EnvFollower/SmoothedValue — C2/C3).
// ============================================================================
struct OnePoleLP {
  double a = 1.0;
  double y1 = 0.0;

  void setFc(double fc, double fs) {
    fc = std::clamp(fc, 0.0, 0.49 * fs);
    a = std::clamp(1.0 - std::exp(-2.0 * kAmpPi * fc / fs), 0.0, 1.0);  // [dsp]
  }

  void reset() { y1 = 0.0; }

  inline double process(double x) {
    y1 += a * (x - y1);
    return y1;
  }
};

// ============================================================================
// SmoothedValue — one-pole parameter ramp, default 10 ms (DESIGN.md §1.5,
// C6: "all gain-like params run through a one-pole SmoothedValue"). Same
// exp-form coefficient family as EnvFollower (C3).
//
// Per-sample step bound (G1.16): step[n] = value[n] - value[n-1]
//                                         = coeff * (target - value[n-1]).
// Since |target - value[n]| is strictly decreasing toward 0 as value[n]
// converges monotonically on target (0 < coeff < 1), the LARGEST step is the
// very first one: step[1] = coeff * (target - start). So the per-sample step
// never exceeds `abs(target - start) * coeff` — the AC as written in
// docs/GATES.md is correct; verified algebraically here, not just asserted.
// ============================================================================
struct SmoothedValue {
  static constexpr double kDefaultTimeMs = 10.0;  // [dsp] DESIGN.md §1.5 default

  double value = 0.0;
  double target = 0.0;
  double coeff = 1.0;

  /// Snap immediately to v (used by reset(), DESIGN.md §9 "Reset semantics":
  /// smoothed-value ramps snap to target, not ramping from zero).
  void reset(double v) {
    value = v;
    target = v;
  }

  void setTimeMs(double ms, double fs) {
    ms = std::max(ms, 0.01);  // [dsp] DESIGN.md C3
    coeff = 1.0 - std::exp(-1.0 / (fs * ms * 1e-3));
  }

  /// Convenience: apply the DESIGN.md-default 10 ms ramp at this sample rate.
  void prepare(double fs) { setTimeMs(kDefaultTimeMs, fs); }

  void setTarget(double t) { target = t; }

  inline double next() {
    value += coeff * (target - value);
    return value;
  }
};

// ============================================================================
// ScopedNoDenormals — RAII flush-to-zero/denormals-are-zero for the block
// (DESIGN.md §1.5; ported from nassau-eq M3's EqCore denormal guard, same
// asm, same bit patterns). Denormal (subnormal) values arise as impulse/IIR
// tail energy decays toward zero; on many CPUs each denormal op stalls the
// FPU. This only affects magnitudes below the normal-float floor (~1e-38),
// far under any of this plugin's audible or golden-parity tolerances.
//
// GCC/Clang use inline asm. MSVC understands neither the GCC target macros nor
// GCC inline-asm syntax, and supports no inline asm at all on x64, so it gets
// the <xmmintrin.h> intrinsic path instead. R2 was amended after G1 to permit
// arch-intrinsic headers precisely because forbidding them left this a silent
// no-op on Windows — see the R2 note in docs/GATES.md.
// ============================================================================
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <xmmintrin.h>
#endif

struct ScopedNoDenormals {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
  unsigned int saved;
  ScopedNoDenormals() {
    saved = _mm_getcsr();
    _mm_setcsr(saved | 0x8040u);  // [dsp] MXCSR FTZ (bit 15) | DAZ (bit 6)
  }
  ~ScopedNoDenormals() { _mm_setcsr(saved); }
#elif defined(__aarch64__)
  unsigned long long saved;
  ScopedNoDenormals() {
    unsigned long long fpcr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    saved = fpcr;
    __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr | (1ull << 24)));  // FPCR.FZ
  }
  ~ScopedNoDenormals() { __asm__ __volatile__("msr fpcr, %0" : : "r"(saved)); }
#elif defined(__x86_64__) || defined(__i386__)
  unsigned int saved;
  ScopedNoDenormals() {
    unsigned int csr;
    __asm__ __volatile__("stmxcsr %0" : "=m"(csr));
    saved = csr;
    unsigned int modified = csr | 0x8040u;  // MXCSR FTZ (bit 15) | DAZ (bit 6)
    __asm__ __volatile__("ldmxcsr %0" : : "m"(modified));
  }
  ~ScopedNoDenormals() { __asm__ __volatile__("ldmxcsr %0" : : "m"(saved)); }
#else
  ScopedNoDenormals() {}  // Portable fallback: no denormal-mode control.
#endif
  ScopedNoDenormals(const ScopedNoDenormals&) = delete;
  ScopedNoDenormals& operator=(const ScopedNoDenormals&) = delete;
};

// ============================================================================
// Shapers (DESIGN.md §1.5, free functions, `double` in and out).
// ============================================================================

// Triode with an explicit knee k. k = 0 reduces the expression to
// `x / (1.0 + 0.0)` = `x / 1.0`, which IEEE-754 division returns bit-exactly
// as x for every finite x — so shapeTriodeK(x, 0) == x is not an
// approximation, it is a property of the formula (G1.6). This is what makes
// a saturation "amount" control built on this primitive continuous at zero
// with no special-case branch (the G4 output-transformer control, §6.4,
// depends on exactly this). [dsp] DESIGN.md §1.5.
inline double shapeTriodeK(double x, double k) {
  return x / (1.0 + k * std::fabs(x));
}

// Classic cubic soft-clip: exact hard limit past |x| = L, C1-continuous at
// the knee, unity small-signal slope (f'(0) = 1, so f(x) ~= x for tiny x).
//
// A plain cubic f(x) = x + b*x^3 cannot satisfy all three of {f'(0)=1,
// f(L)=L, f'(L)=0} at once — that is 3 constraints on 1 free coefficient (b;
// the leading term is fixed to 1 by the unity-slope requirement), which is
// over-determined (the classic *unscaled* cubic soft-clip, f(x)=x-x^3/3,
// only satisfies f'(0)=1 and f'(1)=0; its plateau lands at 2/3, not at the
// requested ceiling). This uses the minimal one-higher-degree (quintic)
// polynomial that hits the ceiling exactly:
//   f(x) = x + x^3/(2L^2) - x^5/(2L^4),   |x| < L
//   f(x) = sign(x) * L,                   |x| >= L
// Solving f'(0)=1, f(L)=L, f'(L)=0 for the two free coefficients of
// x + b*x^3 + c*x^5 gives b = 1/(2L^2), c = -1/(2L^4) (worked by hand, see
// the G1 gate note for the derivation). f' stays >= 0 on [0, L] (monotone,
// no overshoot before the knee is reached). [dsp] DESIGN.md §1.5.
inline double shapeCubic(double x, double L) {
  L = std::fabs(L) < 1e-12 ? 1e-12 : std::fabs(L);  // [dsp] guard against L==0
  if (x >= L) return L;
  if (x <= -L) return -L;
  const double L2 = L * L;
  const double x2 = x * x;
  return x * (1.0 + x2 / (2.0 * L2) - (x2 * x2) / (2.0 * L2 * L2));
}

// ============================================================================
// End of the verbatim lift from nassau-zermatt/Source/DSP/amp_dsp.h.
// Everything below is new for NassauAnalogue.
// ============================================================================

// ============================================================================
// TptOnePole — Zavalishin "Topology-Preserving Transform" one-pole,
// DESIGN.md §5.2/§5.3 (the same per-pole recursion the ZDF ladder (G4's 4
// poles) and the HPF cascade (§5.5, DESIGN.md, G6) both reuse verbatim):
//
//   g = tan(pi*fc/fs) ;  G = g/(1+g)
//   v = (x - s) * G ;  lp = v + s ;  s = lp + v
//   hp = x - lp                          // exact, zero DC gain (G1.2)
//
// `g` is recomputed on setFc() -- the "once per control block, interpolated
// per sample" scheduling of DESIGN.md §2 is the CALLER's job (later gates);
// this primitive itself is a plain single-pole recursion driven one sample
// at a time. [dsp] Zavalishin, "The Art of VA Filter Design".
// ============================================================================
// [WEAK-MACHINE PATH, DESIGN.md §12.2]: `g`/`s`/`curLp`/`curHp` are
// `nassau_real` -- this is the one primitive every filter structure in
// synth_filter.h (LadderFilter/SvfFilter/HpfCascade, the measured cost
// centre) is built from, so it is the actual target of the opt-in
// single-precision path, not phase accumulation (which stays double
// everywhere, unconditionally). `setFc()` still computes `g` in `double`
// (control-rate only, R12 does not apply there) and narrows just once when
// storing it, so the coefficient itself loses no more precision than a
// single final rounding.
struct TptOnePole {
  nassau_real g = 0.0;   // [dsp] tan(pi*fc/fs)
  nassau_real s = 0.0;   // integrator state
  nassau_real curLp = 0.0;
  nassau_real curHp = 0.0;

  // fc is clamped just inside (0, 0.5*fs) so the tan() argument never
  // reaches +-pi/2, where tan() diverges (a robustness clamp; callers apply
  // DESIGN.md's musical/architectural clamps -- e.g. the 0.45*fs ceiling of
  // §5.4 -- themselves). [dsp]
  void setFc(double fc, double fs) {
    fc = std::clamp(fc, 1.0, 0.49 * fs);
    g = static_cast<nassau_real>(std::tan(kAmpPi * fc / fs));
  }

  void reset() {
    s = 0.0;
    curLp = 0.0;
    curHp = 0.0;
  }

  // One sample in, both outputs available afterward via lp()/hp() (or the
  // return value, which is lp). Every intermediate here is `nassau_real`
  // (not `double` narrowed only at the end) -- this is the per-sample loop
  // DESIGN.md §12.2 targets, so the arithmetic itself must run at the
  // reduced width for a weak-ARM build to see any benefit from it.
  inline nassau_real process(nassau_real x) {
    const nassau_real G = g / (nassau_real(1.0) + g);
    const nassau_real v = (x - s) * G;
    const nassau_real lp = v + s;
    s = lp + v;
    curLp = lp;
    curHp = x - lp;  // [dsp] DESIGN.md §5: HP = x - LP exactly, zero DC gain
    return lp;
  }

  nassau_real lp() const { return curLp; }
  nassau_real hp() const { return curHp; }
};

// ============================================================================
// Xorshift32 — the plugin's ONE deterministic PRNG (R8/R13): all noise (the
// white-noise oscillator, DESIGN.md §3.5; the LFO's sample-and-hold source,
// §7) is this generator, fixed-seeded per instance. [dsp] Marsaglia,
// "Xorshift RNGs" (2003), the (13,17,5) parameterisation.
// ============================================================================
struct Xorshift32 {
  uint32_t state;

  explicit Xorshift32(uint32_t seed = 1u) : state(seed ? seed : 0x1234567u) {}

  inline uint32_t next() {
    uint32_t x = state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state = x;
    return x;
  }

  // Uniform in [-1, 1) (24-bit mantissa -> [0,2) then shifted). `double` to
  // match this file's "all double internally" convention (DESIGN.md §2);
  // callers needing `float` (DESIGN.md §3.5) narrow at the call site.
  inline double nextBipolar() {
    return (static_cast<double>(next() >> 8) / 8388608.0) - 1.0;  // [dsp]
  }

  void reseed(uint32_t seed) { state = seed ? seed : 0x1234567u; }
};

// ============================================================================
// PinkFilter — Kellet 3-pole ("economy") pink-noise approximation
// (DESIGN.md §3.5): three one-poles summed with a direct path, giving
// ~-3 dB/octave. Coefficients are Paul Kellet's widely published "economy"
// method (musicdsp.org "Filters/pink noise"), NOT re-derived here -- but
// per R11's lesson from G0 (a hand-transcribed constant shipped wrong by one
// character), they were independently checked rather than trusted from
// memory: feeding a sine at 40/80/160/320/640/1280/2560/5120 Hz and its
// octave partner through the exact recursion below and comparing output
// magnitude gives octave-pair deltas of -2.42 .. -3.69 dB (max deviation
// from the nominal -3 dB: 0.69 dB) at fs = 48 kHz -- inside G1.8's
// 3.0 +/- 0.7 dB tolerance, consistent with DESIGN.md's own "worst-case
// error is about +/-0.5 dB" for this filter family. [dsp]
// ============================================================================
struct PinkFilter {
  double b0 = 0.0, b1 = 0.0, b2 = 0.0;

  void reset() { b0 = b1 = b2 = 0.0; }

  inline double process(double white) {
    b0 = 0.99765 * b0 + white * 0.0990460;   // [dsp] Kellet economy pole 1
    b1 = 0.96300 * b1 + white * 0.2965164;   // [dsp] Kellet economy pole 2
    b2 = 0.57000 * b2 + white * 1.0526913;   // [dsp] Kellet economy pole 3
    return b0 + b1 + b2 + white * 0.1848;    // [dsp] Kellet economy direct path
  }
};

// ============================================================================
// polyBlep — DESIGN.md §3.1, quoted VERBATIM from the design document
// (Valimaki/Leary 2-sample polynomial BLEP). t is the fractional distance,
// in samples, from the discontinuity; dt is the phase increment
// (cycles/sample). [dsp]
// ============================================================================
inline double polyBlep(double t, double dt) {
  if (t < dt)        { t /= dt;      return t + t - t*t - 1.0; }
  if (t > 1.0 - dt)  { t = (t - 1.0)/dt; return t*t + t + t + 1.0; }
  return 0.0;
}

// ============================================================================
// AdsrEnv — the analogue-style one-pole ADSR of DESIGN.md §6: a one-pole
// running toward a target it never reaches (overshooting past 1.0 on
// attack, undershooting past 0.0 on release), so the curve is exponential
// rather than linear and crosses the "nominal" endpoint at exactly the
// requested time.
//
// Runs at CONTROL rate (DESIGN.md §2): the caller advances it once per
// control block via step(fsControl), where fsControl = fs / kControlBlock.
//
// The three time-constant divisors are DERIVED, not tuned (DESIGN.md §6),
// and are computed here via std::log rather than hand-transcribed as
// decimal literals -- R11's lesson from G0 (a hand-typed 32-hex constant
// shipped wrong by one character, uncaught by any test): a computed
// std::log(...) call cannot suffer a transcription typo the way a literal
// "2.037" could.
//   Attack:  target 1.15, crosses 1.0 at t_A  => divisor = ln(1.15/0.15)
//   Decay:   reaches within 1% of the (1.0 -> S) span in t_D
//                                             => divisor = ln(100)
//   Release: target -0.05, from 1.0 crosses 0 at t_R
//                                             => divisor = ln(1.05/0.05)
// [dsp] DESIGN.md §6.
// ============================================================================
struct AdsrEnv {
  enum class State { Idle, Attack, Decay, Sustain, Release };

  double y = 0.0;
  State state = State::Idle;

  double aCoeff = 1.0, dCoeff = 1.0, rCoeff = 1.0;
  double sustainLevel = 1.0;

  static constexpr double kAttackTarget = 1.15;    // [dsp] DESIGN.md §6
  static constexpr double kReleaseTarget = -0.05;  // [dsp] DESIGN.md §6

  // [dsp] DESIGN.md §6 derivation, computed (not transcribed) -- see the
  // struct comment above.
  static double attackDivisor()  { return std::log(1.15 / 0.15); }   // ~2.037
  static double decayDivisor()   { return std::log(100.0); }         // ~4.605
  static double releaseDivisor() { return std::log(1.05 / 0.05); }   // ~3.045

  // coefficient for a one-pole that reaches its DESIGN.md-defined "done"
  // point in `ms` milliseconds at control rate `fsControl` (steps/sec),
  // given the stage's time-constant divisor.
  static double coeffForMs(double ms, double divisor, double fsControl) {
    ms = std::max(ms, 1e-3);
    fsControl = std::max(fsControl, 1e-6);
    const double tau = (ms * 1e-3) / std::max(divisor, 1e-12);
    return 1.0 - std::exp(-1.0 / (std::max(tau, 1e-12) * fsControl));
  }

  void setAttackMs(double ms, double fsControl)  { aCoeff = coeffForMs(ms, attackDivisor(),  fsControl); }
  void setDecayMs(double ms, double fsControl)   { dCoeff = coeffForMs(ms, decayDivisor(),   fsControl); }
  void setReleaseMs(double ms, double fsControl) { rCoeff = coeffForMs(ms, releaseDivisor(), fsControl); }
  void setSustainPercent(double pct) { sustainLevel = std::clamp(pct * 0.01, 0.0, 1.0); }

  // Note-on: (re)start Attack from the CURRENT level, no reset of y -- this
  // is what makes a retrigger during Release jump-free (G1.14).
  void noteOn() { state = State::Attack; }

  // Note-off: go straight to Release from the current level, no reset of y
  // -- jump-free during Attack/Decay/Sustain (G1.14). A no-op from Idle.
  void noteOff() {
    if (state != State::Idle) state = State::Release;
  }

  void reset() {
    y = 0.0;
    state = State::Idle;
  }

  bool isIdle() const { return state == State::Idle; }

  // Advance exactly one control step; returns the new value. Never negative,
  // never exceeds 1.0, exactly 0.0 in Idle (G1.13).
  inline double step() {
    switch (state) {
      case State::Attack: {
        y += aCoeff * (kAttackTarget - y);
        if (y >= 1.0) {
          y = 1.0;
          state = State::Decay;
        }
        break;
      }
      case State::Decay: {
        y += dCoeff * (sustainLevel - y);
        // "within 1%" is 1% of the (1.0 -> S) span the decay is traversing
        // (DESIGN.md §6's own derivation: t_D = tau*ln(100) comes from
        // exp(-t/tau) == 0.01 of the INITIAL gap, not an absolute 1%).
        const double span = std::fabs(1.0 - sustainLevel);
        if (std::fabs(y - sustainLevel) <= std::max(0.01 * span, 1e-9)) {
          y = sustainLevel;
          state = State::Sustain;
        }
        break;
      }
      case State::Sustain:
        y = sustainLevel;  // held (DESIGN.md §6)
        break;
      case State::Release: {
        y += rCoeff * (kReleaseTarget - y);
        if (y <= 0.0) {
          y = 0.0;
          state = State::Idle;
        }
        break;
      }
      case State::Idle:
      default:
        y = 0.0;
        break;
    }
    return y;
  }
};

// ============================================================================
// Lfo — DESIGN.md §7: one global LFO, advanced once per control block.
// Waves: Triangle, Saw (falling), Ramp (rising), Square, Sample & Hold
// (a dedicated Xorshift32, seeded in init(), R8/R13). Delay: the output is
// gated to exactly 0 until `delayMs` has elapsed since the last
// noteOnEdge() call (DESIGN.md §7: "per note-on of the FIRST held note"; the
// caller is responsible for invoking noteOnEdge() only on the 0->1 held-
// voice-count transition), then ramps linearly to full depth over 200 ms.
// ============================================================================
struct Lfo {
  enum class Wave { Triangle, Saw, Ramp, Square, SampleHold };

  Wave wave = Wave::Triangle;
  double phase = 0.0;       // [0, 1)
  double increment = 0.0;   // set via setRateHz()

  double delaySeconds = 0.0;
  bool delayActive = false;
  double delayElapsed = 0.0;
  double depthGain = 1.0;

  Xorshift32 sh{1u};
  double shValue = 0.0;

  static constexpr double kRampSeconds = 0.200;  // [ref] DESIGN.md §7: 200 ms linear ramp-in

  // Seeds the sample-and-hold generator (R8/R13: fixed per instance) and
  // resets phase/gating.
  void init(uint32_t seed) {
    sh = Xorshift32(seed);
    reset();
  }

  // DESIGN.md §7 is explicit about delayMs > 0 ("depth ramps in linearly
  // AFTER the delay, over 200ms") but does not say whether the 200ms ramp
  // itself still applies at the default delayMs == 0. This implementation
  // takes delayMs == 0 to mean "off": full depth immediately, no ramp --
  // the conventional reading of a delay control at its zero/off position,
  // and the one that keeps the (very common) default setting free of an
  // unrequested 200ms fade-in on every note. This is a genuine reading of
  // an underspecified corner of DESIGN.md §7 (docs/GATES.md G1 gate note:
  // "a decision the plan did not cover"), not an obvious fact -- flagged
  // for confirmation when this primitive is wired into SynthCore (G3).
  void reset() {
    phase = 0.0;
    delayElapsed = 0.0;
    delayActive = (delaySeconds > 0.0);
    depthGain = delayActive ? 0.0 : 1.0;
    shValue = sh.nextBipolar();
  }

  void setRateHz(double hz, double fsControl) {
    fsControl = std::max(fsControl, 1e-6);
    increment = hz / fsControl;
  }

  void setDelayMs(double ms) { delaySeconds = std::max(ms, 0.0) * 0.001; }

  // Call exactly when the held-voice count transitions 0 -> 1 (DESIGN.md
  // §7's "resets only... under a held chord" -- the caller does not call
  // this on every note-on, only the first one of a newly-non-empty chord).
  void noteOnEdge() {
    delayElapsed = 0.0;
    delayActive = (delaySeconds > 0.0);
    depthGain = delayActive ? 0.0 : 1.0;
  }

  static double waveAt(Wave w, double p, double heldShValue) {
    switch (w) {
      case Wave::Triangle:
        // Starts at 0, rises to +1 at quarter-cycle, back to 0 at half,
        // down to -1 at three-quarter, back to 0 at wrap. Zero mean.
        if (p < 0.25) return 4.0 * p;
        if (p < 0.75) return 2.0 - 4.0 * p;
        return 4.0 * p - 4.0;
      case Wave::Saw:
        // "Saw (falling)": starts at +1, falls linearly to -1.
        return 1.0 - 2.0 * p;
      case Wave::Ramp:
        // "Ramp (rising)": starts at -1, rises linearly to +1.
        return -1.0 + 2.0 * p;
      case Wave::Square:
        return (p < 0.5) ? 1.0 : -1.0;
      case Wave::SampleHold:
      default:
        return heldShValue;
    }
  }

  // Advance one control step (dt = 1/fsControl seconds). Returns the
  // depth-gated output, in [-1, 1].
  inline double step(double fsControl) {
    if (delayActive) {
      fsControl = std::max(fsControl, 1e-6);
      delayElapsed += 1.0 / fsControl;
      if (delayElapsed >= delaySeconds) {
        const double rampElapsed = delayElapsed - delaySeconds;
        depthGain = (kRampSeconds <= 0.0) ? 1.0 : std::min(1.0, rampElapsed / kRampSeconds);
        if (depthGain >= 1.0) {
          depthGain = 1.0;
          delayActive = false;
        }
      }
    }

    // Evaluate at the MIDPOINT of the current step's phase interval
    // (phase + increment/2, wrapped into [0,1)) rather than at its raw
    // left edge. For a piecewise-LINEAR wave (Saw/Ramp; Triangle is
    // piecewise-linear too) this makes the discrete average of any whole
    // number of consecutive samples match the continuous integral (zero
    // mean) exactly, rather than carrying the systematic O(1/N) bias a
    // left-edge sample of a ramp has (a straightforward midpoint/trapezoid
    // quadrature identity). Square and SampleHold are insensitive to a
    // sub-increment phase shift (Square's edges just move by less than one
    // sample; SampleHold ignores phase entirely). [dsp]
    double samplePhase = phase + 0.5 * increment;
    samplePhase -= std::floor(samplePhase);
    const double raw = waveAt(wave, samplePhase, shValue);

    phase += increment;
    if (phase >= 1.0) {
      phase -= 1.0;
      shValue = sh.nextBipolar();  // exactly one draw per cycle (G1.16)
    }

    return raw * depthGain;
  }
};
