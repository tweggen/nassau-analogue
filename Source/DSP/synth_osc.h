#pragma once

// synth_osc.h — NassauAnalogue oscillator section (Gate G2, DESIGN.md §3-§4).
//
// Header-only, dependency-free (R2): the only include is "synth_dsp.h" itself
// (already R2-legal — <algorithm> <cmath> <cstdint>). No SDK/IPlug2/framework
// dependency, ever. Builds and unit-tests standalone (Tests/osc_tests.cpp
// drives these structs DIRECTLY, no SynthCore link needed — same shape as
// nassau-zermatt/Source/DSP/amp_cabinet.h + Tests/cabinet_tests.cpp).
//
// Four structs: `Osc` (saw/pulse/tri, PolyBLEP, hard-sync in), `SubOsc`
// (phase-derived from a driving Osc's own wrap, DESIGN.md §3.4), NoiseSource
// (white/pink, one generator per voice, DESIGN.md §3.5), and `MixerBlock`
// (a stateless linear sum, DESIGN.md §4's `mix = ...` line — the drive stage
// that follows it in DESIGN.md §4 is G6's job, not G2's).
//
// All internal math is `double` (DESIGN.md §2 convention); callers narrow to
// `float` at the I/O boundary.

#include "synth_dsp.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

// ============================================================================
// Osc — one PolyBLEP VCO: saw / pulse (PWM) / triangle, plus hard-sync input
// (DESIGN.md §3.1-§3.3).
//
// Pitch (phase increment `dt`) and pulse width are CONTROL-RATE quantities
// (DESIGN.md §2/§3.2): the caller (a later gate's voice) recomputes them via
// setDt()/setPwPercent() once per control block. Only phase advance and the
// BLEP arithmetic run in the audio-rate step() (R12: no transcendentals, no
// atomic loads inside it).
// ============================================================================
struct Osc {
  enum class Wave : int { Saw = 0, Pulse = 1, Tri = 2 };  // matches SynthCore::Wave int values

  Wave wave = Wave::Saw;
  double phase = 0.0;   // [0, 1) cycles
  double dt = 0.0;      // phase increment, cycles/sample (control rate, R12)
  double pw = 0.5;      // CLAMPED pulse width, Pulse wave only (DESIGN.md §3.2)

  // Test-only escape hatch (docs/GATES.md G2 Deliverables): with this false,
  // step() emits the NAIVE (unblepped) waveform. Used by exactly one AC,
  // G2.6, to prove the BLEP correction is actually wired in by comparison
  // rather than by an absolute figure alone.
  bool mBlepEnabled = true;

  // Triangle: leaky-integrated BLEP square (DESIGN.md §3.1), NOT a
  // BLAMP-corrected ramp. `triLeakR` is the per-sample retain factor of that
  // leaky integrator; `triState` its running sum.
  double triState = 0.0;
  double triLeakR = 0.999;  // [dsp] recomputed per sample rate, see setSampleRate()

  // [dsp] DESIGN.md §3.1: "The leak coefficient is 0.999 at 48 kHz, scaled
  // by sample rate." Read literally, that means the *time-domain corner*
  // the leak implies is what's actually fixed, not the raw per-sample
  // coefficient (which must change with fs to keep the same corner). Solve
  // 0.999 = exp(-2*pi*fc/48000) for fc (verified with python3 -c, see the
  // gate report):
  //   fc = -48000 * ln(0.999) / (2*pi) = 7.643259535435666 Hz
  // then rederive the retain factor at any other fs from that fixed fc via
  // the same exp-form family TptOnePole/OnePoleHP already use (DESIGN.md
  // §1.5 C2). This is a one-pole LOW-PASS-shaped leak (it removes DC drift
  // below ~7.6 Hz while leaving audio-rate integration essentially exact
  // above it), consistent with "self-antialiasing" in DESIGN.md §3.1.
  static constexpr double kTriLeakCornerHz = 7.643259535435666;  // [dsp] derived above

  void setSampleRate(double fs) {
    triLeakR = std::exp(-2.0 * kAmpPi * kTriLeakCornerHz / std::max(fs, 1.0));  // [dsp]
  }

  // DESIGN.md §3.2: "Phase at note-on: both oscillators reset to 0."
  void resetPhase() { phase = 0.0; }

  // Full state reset (DESIGN.md §11 "Reset semantics"): phase AND the
  // triangle integrator's running sum (otherwise a reset would leave stale
  // energy in triState that a fresh instance would never have had, breaking
  // R13 bit-identity across reset()/fresh-instance comparisons).
  void reset() {
    phase = 0.0;
    triState = 0.0;
  }

  void setDt(double dtIn) { dt = dtIn; }

  // DESIGN.md §3.2: "Pulse width is clamped against the phase increment":
  // pw in [max(0.05, dt), min(0.95, 1 - dt)]. `pctIn` is 0..100 (kOsc1PW/
  // kOsc2PW's param range, DESIGN.md §11).
  void setPwPercent(double pctIn) {
    const double raw = pctIn * 0.01;
    double lo = std::max(0.05, dt);       // [dsp] DESIGN.md §3.2
    double hi = std::min(0.95, 1.0 - dt); // [dsp] DESIGN.md §3.2
    if (lo > hi) std::swap(lo, hi);       // degenerate guard: dt >= 0.45 (Nyquist-adjacent)
    pw = std::clamp(raw, lo, hi);
  }

  // The NAIVE (pre-BLEP) waveform value at an arbitrary phase, used both by
  // step() itself and by hardSync()'s step-height computation (DESIGN.md
  // §3.3). For Tri this is the underlying 50%-duty SQUARE that feeds the
  // leaky integrator (DESIGN.md §3.1: triangle "reuses the pulse's two
  // BLEPs" — the discontinuity that actually needs correcting, on sync or
  // otherwise, lives in that square, not in the smooth integrated output).
  static inline double naiveWaveOf(Wave w, double p, double pwIn) {
    switch (w) {
      case Wave::Saw:   return 2.0 * p - 1.0;
      case Wave::Pulse: return (p < pwIn) ? 1.0 : -1.0;
      case Wave::Tri:   return (p < 0.5) ? 1.0 : -1.0;
      default:          return 0.0;
    }
  }

  // The ideal (unit-amplitude, band-UNlimited) triangle value at phase `p`:
  // rises linearly -1 -> +1 over [0, 0.5), falls +1 -> -1 over [0.5, 1).
  // Used only by hardSync()'s Tri case below, to give the leaky integrator a
  // well-defined, BOUNDED value to re-anchor to at a sync reset (see that
  // function's comment for why this is needed, not optional).
  static inline double idealTriAt(double p) { return (p < 0.5) ? (4.0 * p - 1.0) : (3.0 - 4.0 * p); }

  struct StepResult {
    double y = 0.0;
    bool wrapped = false;     // true if phase wrapped (crossed 1.0->0.0) THIS sample
    double wrapFrac = 0.0;    // valid iff wrapped: phase overshoot past 1.0, in CYCLES (< dt)
  };

  // One sample. Advances phase by dt and reports wrap info so a caller can
  // drive a SubOsc (DESIGN.md §3.4) or a sync'd second Osc (DESIGN.md §3.3)
  // from it "for free" — no second phase accumulator anywhere in this file.
  inline StepResult step() {
    StepResult r;

    switch (wave) {
      case Wave::Saw: {
        // [dsp] DESIGN.md §3.1: "y = 2*phase - 1, minus one BLEP at the wrap."
        double y = 2.0 * phase - 1.0;
        if (mBlepEnabled) y -= polyBlep(phase, dt);
        r.y = y;
        break;
      }
      case Wave::Pulse: {
        // [dsp] DESIGN.md §3.1: "y = phase < pw ? 1 : -1, plus a BLEP at the
        // wrap and a sign-flipped BLEP at the pw crossing."
        double y = (phase < pw) ? 1.0 : -1.0;
        if (mBlepEnabled) {
          y += polyBlep(phase, dt);
          double t2 = phase - pw;
          if (t2 < 0.0) t2 += 1.0;
          y -= polyBlep(t2, dt);
        }
        r.y = y;
        break;
      }
      case Wave::Tri:
      default: {
        // [dsp] DESIGN.md §3.1: leaky-integrated BLEP square, normalised by
        // 4*dt. Fixed 50% duty (a triangle is inherently the symmetric
        // case — there is no PW control on this wave in DESIGN.md §11).
        double sq = (phase < 0.5) ? 1.0 : -1.0;
        if (mBlepEnabled) {
          sq += polyBlep(phase, dt);
          double t2 = phase - 0.5;
          if (t2 < 0.0) t2 += 1.0;
          sq -= polyBlep(t2, dt);
        }
        triState = triLeakR * triState + sq;
        r.y = triState * 4.0 * dt;  // [dsp] DESIGN.md §3.1 normalisation
        break;
      }
    }

    phase += dt;
    if (phase >= 1.0) {
      r.wrapped = true;
      r.wrapFrac = phase - 1.0;
      phase -= 1.0;
    }
    return r;
  }

  // Hard sync (DESIGN.md §3.3): called on the SLAVE osc by the caller when
  // the MASTER osc's step() reported `wrapped` this sample. `tFrac` is the
  // master's wrap overshoot expressed as a FRACTION OF A SAMPLE PERIOD
  // (masterWrapFrac / masterDt, in [0,1)) — "the same fractional overshoot"
  // DESIGN.md §3.3 asks for, converted from the master's cycle-domain units
  // into a sample-domain fraction so it can be reapplied against THIS osc's
  // own dt. `ySelf` is the value THIS osc's own step() already produced this
  // sample (used as the "before" reference for the jump-height BLEP scaling
  // below); this call OVERRIDES that value and resets phase for next sample.
  //
  // Step height, not the natural +-1/+-2: DESIGN.md §3.3 is explicit that
  // the correction must use "VCO 2's actual step height at that instant,
  // which is not, in general, the full +-1 of a natural wrap" — i.e. the
  // master can interrupt the slave mid-cycle, at an arbitrary naive value,
  // not just at the ~+-1 a natural wrap would land on. The general
  // jump-height-scaled BLEP correction (derived from DESIGN.md §3.1's own
  // saw/pulse formulas — verified in the gate report that "correction =
  // (jumpHeight/2) * polyBlep(t,dt)" reduces exactly to the quoted
  // "minus/plus one BLEP" forms when jumpHeight is the natural +-2) is used
  // here for the general case.
  //
  // Documented approximation (DESIGN.md §3.3's own words: "the correction
  // is only approximate"): this applies the post-edge polyBlep smoothing
  // only, to THIS sample. A full MinBLEP treatment would also smear a
  // symmetric pre-edge correction into the sample immediately BEFORE the
  // sync instant (the way the normal per-sample wrap/pw-crossing correction
  // above gets that "for free" simply by evaluating polyBlep at whatever
  // phase a later sample happens to land on) — sync's discontinuity instant
  // is NOT on this osc's own dt-grid, so there is no such earlier sample to
  // retroactively correct within a forward-only, single-pass generator.
  // This is exactly the gap DESIGN.md §3.3 and G2.11 both call out, and it
  // is why G2.11's alias floor is far looser than the unsync'd waveforms'.
  //
  // *** Tri + sync is a special case, and NOT an optional refinement ***
  // (a genuine defect found and fixed in this gate, R11): a hard-synced
  // triangle truncates the slave's underlying square at an ARBITRARY,
  // sync-ratio-dependent point in its duty cycle every master cycle. For a
  // rational (and therefore periodic) sync ratio this truncation pattern
  // repeats, and if it is not exactly 50/50 -- which it generically is not
  // -- it feeds a small but PERSISTENT non-zero-mean signal into the leaky
  // integrator every cycle. The leak's DC gain is large by design (~1/(1-r)
  // ~= 1000 at the ~7.6Hz corner, DESIGN.md §3.1 -- deliberately low so it
  // does not colour real audio content), so that persistent bias does not
  // merely nudge the output: it was measured (python3 simulation, VCO2 =
  // Tri, sync on, 1.5x ratio, f0 = 1760Hz @ 44.1kHz) to settle at a SUSTAINED
  // DC offset around 65-70x nominal amplitude -- bounded (the leak's finite
  // DC gain prevents literal divergence) but severely, audibly broken for a
  // fully legitimate, exposed parameter combination (kOsc2Wave=Tri,
  // kOsc2Sync=on). This is a structural mismatch between DESIGN.md §3.1's
  // integrator-based triangle and §3.3's hard sync that neither section
  // cross-references; it is not particular to this file's implementation
  // (an equivalent analogue Miller-integrator circuit, hard-synced with an
  // imbalanced duty, drifts toward a rail the same way). The general
  // jump-height BLEP correction above does not fix it either way (routing
  // it into `triState` vs. straight onto `y` made no measurable difference
  // -- confirmed by python3 simulation of both) because the bias comes from
  // the ORDINARY per-sample square feeding the integrator every sample
  // between resets, not from the sync correction term itself.
  //
  // The fix: a sync reset for Tri ALSO re-anchors `triState` to the
  // analytically correct (unit-amplitude, band-unlimited) triangle value at
  // the new phase, `idealTriAt(newPhase)`, instead of letting the
  // integrator inherit whatever partial area the truncated cycle left
  // behind. This is a natural extension of "phase resets to the fractional
  // overshoot" (DESIGN.md §3.3) to an integrator-based wave: since a
  // sync'd phase reset is ALREADY a discontinuity the naive waveform must
  // jump across, defining that jump for Tri as "jump straight to the ideal
  // triangle's own value" is the same idea DESIGN.md already applies to
  // Saw/Pulse, just phrased in the integrator's own state instead of in an
  // additive correction term. Re-verified by python3 simulation: this
  // bounds the same adversarial case to |y| <= 1.81 (vs. ~65-70
  // unresolved), with no measurable change to G2.5's harmonic content (the
  // ideal triangle IS the leaky integrator's own steady-state target
  // shape, so this only ever fires exactly at a sync reset, never during
  // ordinary free-running operation).
  inline double hardSync(double tFrac, double ySelf) {
    const double newPhase = tFrac * dt;  // this osc's own cycle-domain overshoot

    if (wave == Wave::Tri) {
      const double y = mBlepEnabled ? idealTriAt(newPhase) : ySelf;
      triState = y / (4.0 * dt);
      phase = newPhase;
      return y;
    }

    const double vBefore = naiveWaveOf(wave, phase, pw);      // naive continuation (no sync)
    const double vAfter  = naiveWaveOf(wave, newPhase, pw);   // naive value right after reset
    const double jump = vAfter - vBefore;

    double y = ySelf;
    if (mBlepEnabled) {
      y += 0.5 * jump * polyBlep(newPhase, dt);  // [dsp] see step-height derivation above
    }
    phase = newPhase;
    return y;
  }
};

// ============================================================================
// SubOsc — a square at the driving Osc's frequency / 2 or / 4, derived from
// that Osc's OWN phase accumulator (DESIGN.md §3.4): NOT a second
// independent phase accumulator. The sub's own phase is instead RECOMPUTED
// FROM SCRATCH every sample as a pure function of the driver's CURRENT
// (live) phase and a small integer wrap-position counter — never integrated
// on its own — so it cannot accumulate floating-point drift the way a
// second `phase += dt` accumulator could (this is what makes G2.8's "period
// ratio exact to 1e-9 over 10s" the natural, expected result rather than a
// coincidence of double precision being good enough).
//
//   subPhase = (cycleWrapIndex + driverPhase) / periodDivisor()
//
// `cycleWrapIndex` (0 .. periodDivisor()-1) counts driver wraps completed
// within the CURRENT sub period; `driverPhase` is the driver's own [0,1)
// phase, read BEFORE the driver's own step() advances it this sample (i.e.
// the same phase value the driver itself used to generate ITS sample this
// step). `periodDivisor()` is 2 for -1 octave, 4 for -2 (DESIGN.md §3.4:
// sub freq = driver freq / periodDivisor()).
//
// Treating the sub as a plain 50%-duty PULSE running at `subPhase`/`subDt`
// (subDt = driverDt/periodDivisor()) and applying PolyBLEP's ordinary TWO
// corrections (wrap AND pw-crossing, DESIGN.md §3.1) is what "reuses the
// fractional wrap offset ... for free" (DESIGN.md §3.4) actually buys: an
// earlier version of this struct toggled a bare +-1 state and applied only
// ONE correction (at the wrap sample, using an unscaled or scaled
// wrapFrac/dt) — that is only the POST-edge half of a proper 2-point BLEP,
// missing the PRE-edge half a native oscillator gets for free simply by
// evaluating polyBlep(phase,dt) every sample regardless of whether a wrap
// happened THIS sample. That single-sided version measured a 17dB shortfall
// against G2.9's reference figure; this phase-derived version measures
// within 0.01dB of a directly-generated 50% pulse at the same frequency
// (i.e. bit-for-bit the same sequence of dt-steps, just computed by
// division from the driver instead of by independent accumulation) — see
// the gate report.
// ============================================================================
struct SubOsc {
  enum class Octave : int { Minus1 = 0, Minus2 = 1 };  // matches SynthCore::SubOctave

  Octave octave = Octave::Minus1;
  int cycleWrapIndex = 0;  // 0 .. periodDivisor()-1: driver wraps completed this sub period
  double state = 1.0;      // NAIVE (pre-BLEP) square sign, +-1 -- exposed for tests/toggle-counting

  void reset() {
    cycleWrapIndex = 0;
    state = 1.0;
  }

  // [dsp] DESIGN.md §3.4: -1 octave -> sub freq = driver/2; -2 -> driver/4.
  int periodDivisor() const { return (octave == Octave::Minus1) ? 2 : 4; }

  // Call once per sample. `driverPhaseBeforeStep` is the DRIVING osc's
  // `phase` field read BEFORE its own step() call this sample (so this can
  // evaluate the pre-edge polyBlep branch exactly as a native oscillator's
  // own per-sample check does, one sample ahead of the actual wrap);
  // `driverDt` its dt (unaffected by step(), safe to read before or after);
  // `driverWrapped` is that SAME step's outcome, consumed only to advance
  // the internal wrap-position counter. No second wrap TEST is performed
  // here (DESIGN.md §3.4) — this only ever reacts to the driver's own flag.
  inline double step(double driverPhaseBeforeStep, double driverDt, bool driverWrapped,
                      bool blepEnabled) {
    const int div = periodDivisor();
    const double subDt = driverDt / div;
    double subPhase = (static_cast<double>(cycleWrapIndex) + driverPhaseBeforeStep) / div;
    if (subPhase >= 1.0) subPhase -= 1.0;  // guard; not expected to trigger in normal operation

    const double naive = (subPhase < 0.5) ? 1.0 : -1.0;
    state = naive;
    double y = naive;
    if (blepEnabled) {
      // [dsp] DESIGN.md §3.1's ordinary two-BLEP pulse treatment (pw = 0.5
      // fixed — a sub-octave square is inherently symmetric duty).
      y += polyBlep(subPhase, subDt);
      double t2 = subPhase - 0.5;
      if (t2 < 0.0) t2 += 1.0;
      y -= polyBlep(t2, subDt);
    }

    if (driverWrapped) {
      ++cycleWrapIndex;
      if (cycleWrapIndex >= div) cycleWrapIndex = 0;
    }
    return y;
  }
};

// ============================================================================
// NoiseSource — DESIGN.md §3.5. One generator per voice: `init()` seeds BOTH
// the underlying Xorshift32 AND resets the pink filter's state, so a fresh
// instance and a `reset()`-then-reseed-to-the-same-index instance are
// bit-identical (R8/R13) — mirrors Lfo::init()'s seeding convention in
// synth_dsp.h.
// ============================================================================
struct NoiseSource {
  enum class Color : int { White = 0, Pink = 1 };  // matches SynthCore::NoiseColor

  Xorshift32 rng{1u};
  PinkFilter pink;

  // DESIGN.md §3.5 / R8/R13: "Each voice is seeded deterministically from
  // its voice index in init()/reset()." `voiceIndex` is folded into a
  // non-zero seed (Xorshift32's own ctor already guards seed==0, but +1
  // keeps voice 0 from reusing Xorshift32's own default seed of 1, which
  // would make voice 0 indistinguishable from "no seed given" in a debugger
  // — a purely cosmetic guard, not a correctness requirement).
  void init(int voiceIndex) {
    rng.reseed(static_cast<uint32_t>(voiceIndex) + 1u);
    pink.reset();
  }

  void reset(int voiceIndex) { init(voiceIndex); }

  inline double step(Color c) {
    const double w = rng.nextBipolar();
    return (c == Color::Pink) ? pink.process(w) : w;
  }
};

// ============================================================================
// MixerBlock — DESIGN.md §4's linear sum (the drive stage that follows it in
// DESIGN.md §4 is G6's job, not G2's: this gate ends at the mixer output).
// Stateless: a single free function is enough, kept as a named struct method
// so callers read `MixerBlock::mix(...)` rather than a bare global.
//   mix = o1*Osc1Level + o2*Osc2Level + sub*SubLevel + noise*NoiseLevel
// Level params are 0..100 (%), DESIGN.md §11.
// ============================================================================
struct MixerBlock {
  static inline double mix(double o1, double o1LevelPct, double o2, double o2LevelPct,
                            double sub, double subLevelPct, double noise, double noiseLevelPct) {
    return o1 * (o1LevelPct * 0.01) + o2 * (o2LevelPct * 0.01) + sub * (subLevelPct * 0.01) +
           noise * (noiseLevelPct * 0.01);
  }
};
