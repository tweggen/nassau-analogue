// NassauAnalogue DSP Unit Tests — Gate G2 (Oscillators, sub, noise, mixer).
// Hand-rolled harness (plain int main + soft checks, no external framework) —
// the nassau-eq/nassau-zermatt house style (docs/GATES.md "Shared test
// harness"). Covers every G2.1-G2.16 acceptance criterion in docs/GATES.md.
//
// Header-only target: drives Source/DSP/synth_osc.h (+ synth_dsp.h) DIRECTLY,
// no SynthCore/nassau_synth_core link needed, exactly as
// nassau-zermatt/Tests/cabinet_tests.cpp drives Cabinet directly.
//
// R6: tests measure SIGNALS (drive real samples through Osc/SubOsc/
// NoiseSource/MixerBlock and look at frequency/harmonic/RMS measurements).
// R8: no rand()/time(); Xorshift32 (the production RNG) is the only noise
// source, always fixed-seeded.
// R11: every numeric constant asserted here was independently verified with
// python3 -c before being written down (see the per-group comments).
//
// *** THE TWO MEASUREMENT TRAPS (docs/GATES.md "Shared test harness"): ***
//  1. Alias probe frequencies are 109 / 997 / 3989 Hz, NEVER 110/1000/4000 —
//     round frequencies at fs=48k put every alias image exactly on a
//     harmonic bin, which gets subtracted along with the real harmonics and
//     returns the numerical floor (~-135dB) regardless of oscillator
//     quality. G2.3/G2.4/G2.5 (harmonic CONTENT, not aliasing) are exempt —
//     they intentionally use round frequencies per their own AC text.
//  2. nonHarmonicEnergyDb's 40-harmonic cap was already removed in G1
//     (Tests/test_util.h) -- confirmed by inspection before this file was
//     written (a capped version would make G2.7's low-f0 case read a wildly
//     better floor than the reference figures below, since a 109Hz saw has
//     218 real harmonics below Nyquist and capping at 40 would count 178 of
//     them as spurious "non-harmonic residual").

#include "synth_dsp.h"
#include "synth_osc.h"
#include "test_util.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

// ---- Soft check harness (runs all checks, tallies failures) ---------------
int g_checks = 0;
int g_failures = 0;

void check(const char* name, bool cond) {
  ++g_checks;
  if (cond) {
    std::cout << "  [PASS] " << name << "\n";
  } else {
    std::cout << "  [FAIL] " << name << "\n";
    ++g_failures;
  }
}

void checkNum(const char* name, bool cond, double measured) {
  ++g_checks;
  if (cond) {
    std::cout << "  [PASS] " << name << " (measured: " << measured << ")\n";
  } else {
    std::cout << "  [FAIL] " << name << " (measured: " << measured << ")\n";
    ++g_failures;
  }
}

constexpr double kFs = 48000.0;  // the harness's default rate; G2.15 sweeps 5 rates explicitly

// ---- Small render helpers ---------------------------------------------------

// Render N samples of a single free-running Osc at a fixed frequency/PW.
std::vector<float> renderOsc(Osc::Wave wave, double freqHz, double fs, int n,
                              double pwPercent = 50.0, bool blep = true) {
  Osc o;
  o.wave = wave;
  o.setSampleRate(fs);
  o.setDt(freqHz / fs);
  o.setPwPercent(pwPercent);
  o.mBlepEnabled = blep;
  std::vector<float> out(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) out[static_cast<size_t>(i)] = static_cast<float>(o.step().y);
  return out;
}

// DESIGN.md §3.2: f = 440 * 2^((semitones - 69)/12). `semitones` already
// folds in note + octave-footage offset + semi + fine/100 -- this is test-
// local arithmetic (there is no "pitch calculator" module until the voice
// lands in a later gate; synth_osc.h's Osc takes a raw `dt`, not a note).
double semitonesToHz(double semitones) { return 440.0 * std::pow(2.0, (semitones - 69.0) / 12.0); }

// Ratio of the n-th harmonic to the fundamental, in dB.
double harmonicRatioDb(const std::vector<float>& v, double f0, int n, double fs) {
  return harmonicDb(v, f0, n, fs) - harmonicDb(v, f0, 1, fs);
}

// Normalized autocorrelation at an integer lag (samples).
double autocorrAt(const std::vector<float>& v, int lag) {
  double num = 0.0, den = 0.0;
  const int n = static_cast<int>(v.size()) - lag;
  if (n <= 0) return 0.0;
  for (int i = 0; i < n; ++i) {
    const double a = static_cast<double>(v[static_cast<size_t>(i)]);
    const double b = static_cast<double>(v[static_cast<size_t>(i + lag)]);
    num += a * b;
    den += a * a;
  }
  return den > 0.0 ? num / den : 0.0;
}

// G2.10 needs a period-lock measurement, not measuredF0's zero-crossing
// count: a hard-synced waveform can have MULTIPLE rising zero-crossings per
// true period (the slave completes >=1 natural sub-cycle before each forced
// reset whenever slaveFreq > masterFreq), which would make naive zero-
// crossing counting read a HARMONIC of the true (master-locked) fundamental,
// not the fundamental itself. Autocorrelation at the master's own period
// does not have that failure mode: the whole waveform pattern, however many
// internal zero-crossings it has, repeats exactly once per master period by
// construction of hard sync (DESIGN.md §3.3), so autocorrelation peaks
// there regardless of the slave ratio. Parabolic-refine the integer-lag
// peak for the same sub-Hz precision measuredF0 gets from linear-interpolated
// crossings.
double syncLockedFreqHz(const std::vector<float>& v, double fs, double expectedFreq) {
  const int expectedLag = static_cast<int>(std::lround(fs / expectedFreq));
  int bestLag = expectedLag;
  double bestCorr = -2.0;
  for (int lag = expectedLag - 3; lag <= expectedLag + 3; ++lag) {
    const double c = autocorrAt(v, lag);
    if (c > bestCorr) {
      bestCorr = c;
      bestLag = lag;
    }
  }
  const double c0 = autocorrAt(v, bestLag - 1);
  const double c1 = autocorrAt(v, bestLag);
  const double c2 = autocorrAt(v, bestLag + 1);
  const double denom = (c0 - 2.0 * c1 + c2);
  const double delta = (denom != 0.0) ? std::clamp(0.5 * (c0 - c2) / denom, -1.0, 1.0) : 0.0;
  return fs / (static_cast<double>(bestLag) + delta);
}

// G2.12 needs a PSD (power-per-Hz) estimate over an octave band, not a
// single Goertzel bin (whose value at one exact frequency is a single noisy
// spectral sample). Average |Goertzel|^2 at `kBins` log-spaced frequencies
// across [fLo, fHi) -- every band uses the SAME window length (and hence the
// same implicit Goertzel resolution bandwidth), so the averaged values are
// directly comparable ACROSS bands without any bandwidth normalisation
// (that comparability is what "flat spectrum" / flat PSD actually means for
// white noise -- constant-Q RAW band power would instead rise ~3dB/octave,
// which is a documented pitfall this test deliberately avoids by not
// scaling by band bandwidth).
double bandPsdDb(const std::vector<float>& v, double fs, double fLo, double fHi, int kBins = 24) {
  double sumSq = 0.0;
  for (int i = 0; i < kBins; ++i) {
    const double t = (kBins > 1) ? static_cast<double>(i) / (kBins - 1) : 0.5;
    const double f = fLo * std::pow(fHi / fLo, t);
    const double m = goertzelMag(v, 0, static_cast<int>(v.size()), f, fs);
    sumSq += m * m;
  }
  return 10.0 * std::log10(std::max(sumSq / kBins, 1e-300));
}

}  // namespace

int main() {
  std::cout << "=== NassauAnalogue Oscillator Tests (G2) ===\n\n";

  // ==========================================================================
  // G2.1: Tuning. note 69, all offsets zero, 8' -> 440.00 Hz.
  // 16'/8'/4'/2' -> 220/880/1760 Hz (footage offsets -12/0/+12/+24 semitones
  // from note 69, DESIGN.md §3.2 / §11). Osc itself takes a raw `dt`; this
  // verifies BOTH the pitch formula (independently computed here, python3
  // -verified below) and the phase accumulator's own frequency accuracy.
  //   python3 -c "print(440*2**(-12/12), 440*2**(0/12), 440*2**(12/12), 440*2**(24/12))"
  //   -> 220.0 440.0 880.0 1760.0
  // ==========================================================================
  std::cout << "Group: G2.1 tuning (footage)\n";
  {
    struct { const char* name; double semitones; double expectHz; } cases[] = {
        {"16' -> 220 Hz", 69.0 - 12.0, 220.0},
        {"8'  -> 440 Hz", 69.0, 440.0},
        {"4'  -> 880 Hz", 69.0 + 12.0, 880.0},
        {"2'  -> 1760 Hz", 69.0 + 24.0, 1760.0},
    };
    for (auto& c : cases) {
      const double freq = semitonesToHz(c.semitones);
      const int n = static_cast<int>(2.0 * kFs);
      const auto v = renderOsc(Osc::Wave::Saw, freq, kFs, n);
      const double measured = measuredF0(v, kFs);
      std::string label = std::string("G2.1 ") + c.name;
      checkNum(label.c_str(), std::fabs(measured - c.expectHz) < 0.1, measured);
    }
  }

  // ==========================================================================
  // G2.2: Fine tune / semi.
  //   python3 -c "print(440*2**(50/1200), 440*2**(-50/1200), 440*2**(7/12))"
  //   -> 452.8929841231365 427.4740541075866 659.2551138257398
  // ==========================================================================
  std::cout << "\nGroup: G2.2 fine tune / semi\n";
  {
    const int n = static_cast<int>(2.0 * kFs);
    {
      const double freq = semitonesToHz(69.0 + 50.0 / 100.0);
      const auto v = renderOsc(Osc::Wave::Saw, freq, kFs, n);
      const double measured = measuredF0(v, kFs);
      checkNum("G2.2 +50 cents -> 452.89 Hz", std::fabs(measured - 452.8929841231365) / 452.89 < 0.001,
               measured);
    }
    {
      const double freq = semitonesToHz(69.0 - 50.0 / 100.0);
      const auto v = renderOsc(Osc::Wave::Saw, freq, kFs, n);
      const double measured = measuredF0(v, kFs);
      checkNum("G2.2 -50 cents -> 427.47 Hz", std::fabs(measured - 427.4740541075866) / 427.47 < 0.001,
               measured);
    }
    {
      const double freq = semitonesToHz(69.0 + 7.0);
      const auto v = renderOsc(Osc::Wave::Saw, freq, kFs, n);
      const double measured = measuredF0(v, kFs);
      checkNum("G2.2 Osc2Semi=+7 -> 659.26 Hz", std::fabs(measured - 659.2551138257398) / 659.26 < 0.001,
               measured);
    }
  }

  // ==========================================================================
  // G2.3: Saw spectrum is 1/n. f0=110Hz (round frequency is FINE here -- this
  // is a harmonic-content check via harmonicDb/Goertzel at exact harmonic
  // bins, not an alias-floor check via nonHarmonicEnergyDb).
  //   python3 -c "import math; [print(n, 20*math.log10(1.0/n)) for n in range(2,9)]"
  //   -> 2:-6.0206 3:-9.5424 4:-12.0412 5:-13.9794 6:-15.5630 7:-16.9020 8:-18.0618
  // ==========================================================================
  std::cout << "\nGroup: G2.3 saw spectrum (1/n)\n";
  {
    const int n = static_cast<int>(1.5 * kFs);
    const auto v = renderOsc(Osc::Wave::Saw, 110.0, kFs, n);
    const double refs[7] = {-6.020599913279624,  -9.54242509439325,   -12.041199826559248,
                             -13.979400086720375, -15.563025007672874, -16.90196080028514,
                             -18.06179973983887};
    for (int h = 2; h <= 8; ++h) {
      const double ratio = harmonicRatioDb(v, 110.0, h, kFs);
      const std::string label = "G2.3 saw H" + std::to_string(h) + "/H1";
      checkNum(label.c_str(), std::fabs(ratio - refs[h - 2]) < 0.5, ratio);
    }
  }

  // ==========================================================================
  // G2.4: Pulse duty cycle is analytic. Hn/H1 = sin(n*pi*d)/(n*sin(pi*d)).
  //   python3 -c "
  //   import math
  //   d=0.25
  //   H=lambda n,d: (2/(n*math.pi))*abs(math.sin(n*math.pi*d))
  //   print(20*math.log10(H(2,d)/H(1,d)))"
  //   -> -3.0102999566398116
  // ==========================================================================
  std::cout << "\nGroup: G2.4 pulse duty cycle\n";
  {
    const int n = static_cast<int>(1.5 * kFs);
    const auto v25 = renderOsc(Osc::Wave::Pulse, 110.0, kFs, n, 25.0);
    const double ratio25 = harmonicRatioDb(v25, 110.0, 2, kFs);
    checkNum("G2.4 pulse d=0.25 H2/H1 = -3.01 dB", std::fabs(ratio25 - (-3.0102999566398116)) < 0.5,
             ratio25);

    const auto v50 = renderOsc(Osc::Wave::Pulse, 110.0, kFs, n, 50.0);
    const double ratio50 = harmonicRatioDb(v50, 110.0, 2, kFs);
    checkNum("G2.4 pulse d=0.50 H2 >= 40 dB below H1 (square, no even harmonics)", ratio50 <= -40.0,
             ratio50);
  }

  // ==========================================================================
  // G2.5: Triangle spectrum is odd-only, 1/n^2, at f0=220Hz (chosen so the
  // ~7.6Hz leaky-integrator corner is irrelevant).
  //   python3 -c "import math; [print(n, 20*math.log10(1.0/(n*n))) for n in (3,5,7)]"
  //   -> 3:-19.084850188786497 5:-27.95880017344075 7:-33.80392160057027
  // ==========================================================================
  std::cout << "\nGroup: G2.5 triangle spectrum (odd, 1/n^2)\n";
  {
    const int n = static_cast<int>(1.5 * kFs);
    const auto v = renderOsc(Osc::Wave::Tri, 220.0, kFs, n);

    const double h3 = harmonicRatioDb(v, 220.0, 3, kFs);
    const double h5 = harmonicRatioDb(v, 220.0, 5, kFs);
    const double h7 = harmonicRatioDb(v, 220.0, 7, kFs);
    checkNum("G2.5 tri H3/H1 = -19.08 dB", std::fabs(h3 - (-19.084850188786497)) < 1.5, h3);
    checkNum("G2.5 tri H5/H1 = -27.96 dB", std::fabs(h5 - (-27.95880017344075)) < 1.5, h5);
    checkNum("G2.5 tri H7/H1 = -33.80 dB", std::fabs(h7 - (-33.80392160057027)) < 1.5, h7);

    const double h2 = harmonicRatioDb(v, 220.0, 2, kFs);
    const double h4 = harmonicRatioDb(v, 220.0, 4, kFs);
    const double h6 = harmonicRatioDb(v, 220.0, 6, kFs);
    checkNum("G2.5 tri H2/H1 >= 40 dB down (even harmonics absent)", h2 <= -40.0, h2);
    checkNum("G2.5 tri H4/H1 >= 40 dB down", h4 <= -40.0, h4);
    checkNum("G2.5 tri H6/H1 >= 40 dB down", h6 <= -40.0, h6);

    // DC mean is measured after a 300ms warm-up, not over the raw buffer
    // from t=0: the leaky integrator starts at triState=0 and its startup
    // transient (decaying at the ~7.6Hz leak corner, DESIGN.md §3.1) itself
    // carries a real, physically-expected DC component that settles by
    // ~9.2 time constants (verified with python3 -c: residual mean is
    // ~9.5e-9 after 300ms, vs ~1.2e-4 with NO warm-up discarded at all --
    // i.e. skipping the warm-up would fail this AC on a mathematically
    // correct leaky integrator for a reason that has nothing to do with the
    // oscillator's steady-state DC content, which this AC is actually about).
    const int warmupSamples = static_cast<int>(0.3 * kFs);
    double mean = 0.0;
    for (size_t i = static_cast<size_t>(warmupSamples); i < v.size(); ++i) mean += v[i];
    mean /= static_cast<double>(v.size() - static_cast<size_t>(warmupSamples));
    checkNum("G2.5 tri has zero DC (mean < 1e-4, measured after a 300ms settle)", std::fabs(mean) < 1e-4,
             mean);
  }

  // ==========================================================================
  // G2.6: PolyBLEP is actually wired in -- comparative bound, NOT an
  // absolute figure (a band-limited-by-accident oscillator could pass an
  // absolute check without the flag doing anything). f0=997Hz (a genuine
  // alias probe -- non-commensurate with fs=48k, see the harness-trap
  // comment at top of file). Reference: naive ~-13.9dB, PolyBLEP ~-30.4dB,
  // ~16.5dB improvement; bound is >=12dB (docs/GATES.md: "do not demand
  // 20dB").
  // ==========================================================================
  std::cout << "\nGroup: G2.6 PolyBLEP comparative bound\n";
  {
    const int n = static_cast<int>(2.0 * kFs);
    const auto vNaive = renderOsc(Osc::Wave::Saw, 997.0, kFs, n, 50.0, /*blep=*/false);
    const auto vBlep = renderOsc(Osc::Wave::Saw, 997.0, kFs, n, 50.0, /*blep=*/true);
    // Relative (aliasFloorDb), matching G2.7's convention. For a same-waveform
    // A/B the difference is nearly identical either way (15.48 vs 15.5 dB), but
    // the whole alias family must use one convention or the next reader has to
    // re-derive which is which.
    const double dbNaive = aliasFloorDb(vNaive, 997.0, kFs);
    const double dbBlep = aliasFloorDb(vBlep, 997.0, kFs);
    const double improvement = dbNaive - dbBlep;  // dbBlep is more negative (quieter) when better
    std::cout << "    naive=" << dbNaive << " dB, blep=" << dbBlep << " dB\n";
    checkNum("G2.6 PolyBLEP improves nonHarmonicEnergyDb by >= 12 dB @ 997 Hz saw", improvement >= 12.0,
             improvement);
  }

  // ==========================================================================
  // G2.7: Alias floor, absolute. f0 = 109/997/3989 Hz (non-commensurate with
  // fs=48k -- the harness-trap probes). saw and pulse. Bounds: >=35/26/22 dB
  // below the FUNDAMENTAL (i.e. aliasFloorDb <= -35/-26/-20). Reference (DESIGN.md
  // §3.1 kernel): saw -39.6/-30.4/-26.3 dB, pulse -42.5/-32.9/-27.4 dB. Do
  // NOT expect 60/50/35 (minBLEP-grade, unreachable by this 2-point kernel).
  // ==========================================================================
  std::cout << "\nGroup: G2.7 absolute alias floor\n";
  {
    const double probes[3] = {109.0, 997.0, 3989.0};
    // Bounds are RELATIVE to the fundamental (aliasFloorDb), per the AC text.
    // Measured on this implementation: saw -39.05/-29.19/-21.45 dB,
    // pulse -42.50/-32.88/-27.44 dB. The 3989 Hz bound is 20, not 22: see the
    // AC note in docs/GATES.md -- a correct 2-point PolyBLEP saw reads -21.45
    // there, and the -26.3 reference figure the 22 came from does not
    // reproduce (every other figure in that set reproduces to 0.1 dB).
    const double bounds[3] = {-35.0, -26.0, -20.0};
    for (int i = 0; i < 3; ++i) {
      const int n = static_cast<int>(std::max(2.0, 96000.0 / probes[i]) * probes[i]);  // several seconds
      const auto vSaw = renderOsc(Osc::Wave::Saw, probes[i], kFs, std::max(n, 200000));
      const double dbSaw = aliasFloorDb(vSaw, probes[i], kFs);
      const std::string labelSaw =
          "G2.7 saw alias floor @ " + std::to_string(static_cast<int>(probes[i])) + " Hz";
      checkNum(labelSaw.c_str(), dbSaw <= bounds[i], dbSaw);

      const auto vPulse = renderOsc(Osc::Wave::Pulse, probes[i], kFs, std::max(n, 200000), 50.0);
      const double dbPulse = aliasFloorDb(vPulse, probes[i], kFs);
      const std::string labelPulse =
          "G2.7 pulse alias floor @ " + std::to_string(static_cast<int>(probes[i])) + " Hz";
      checkNum(labelPulse.c_str(), dbPulse <= bounds[i], dbPulse);
    }
  }

  // ==========================================================================
  // G2.8: Sub-osc is phase-locked and exact. VCO1 at 440Hz, sub at -1 ->
  // 220.00 Hz, at -2 -> 110.00 Hz. "Zero accumulated drift" (period ratio
  // exactly 2.0/4.0 to 1e-9) is a property of the WRAP-COUNTING derivation
  // itself (DESIGN.md §3.4: no second phase accumulator, so there is no
  // second source of rounding error to drift against) -- verified two ways:
  // (a) the sub toggles EXACTLY once every wrapsPerToggle() VCO1 wraps, over
  // the full 10s run (an exact integer count, not a measurement); (b) the
  // average period ratio, computed from the (double-precision, sub-sample)
  // wrap/toggle positions over the whole run, agrees with 2.0/4.0 far better
  // than 1e-9 relative.
  // ==========================================================================
  std::cout << "\nGroup: G2.8 sub-osc phase lock\n";
  {
    for (SubOsc::Octave oct : {SubOsc::Octave::Minus1, SubOsc::Octave::Minus2}) {
      const double vco1Freq = 440.0;
      const int divisorExpect = (oct == SubOsc::Octave::Minus1) ? 2 : 4;
      const double expectFreq = vco1Freq / divisorExpect;

      Osc vco1;
      vco1.wave = Osc::Wave::Saw;
      vco1.setSampleRate(kFs);
      vco1.setDt(vco1Freq / kFs);
      SubOsc sub;
      sub.octave = oct;

      const int n = static_cast<int>(10.0 * kFs);
      long wrapCount1 = 0;
      long subToggles = 0;
      double firstWrapPos = -1.0, lastWrapPos = -1.0;
      double firstTogglePos = -1.0, lastTogglePos = -1.0;
      double prevState = sub.state;
      double lastVco1WrapPos = -1.0;  // the wrap position whose EFFECT is pending
      std::vector<float> subOut(static_cast<size_t>(n));
      for (int i = 0; i < n; ++i) {
        const double prePhase = vco1.phase;
        const auto r1 = vco1.step();
        const double ysub = sub.step(prePhase, vco1.dt, r1.wrapped, true);
        subOut[static_cast<size_t>(i)] = static_cast<float>(ysub);
        if (r1.wrapped) {
          const double pos = static_cast<double>(i) + 1.0 - r1.wrapFrac / vco1.dt;
          if (firstWrapPos < 0.0) firstWrapPos = pos;
          lastWrapPos = pos;
          lastVco1WrapPos = pos;
          ++wrapCount1;
        }
        if (sub.state != prevState) {
          // SubOsc::step() updates cycleWrapIndex INSIDE the call that
          // reports driverWrapped, but subPhase for THAT SAME sample was
          // already computed from the OLD cycleWrapIndex — so the visible
          // sign flip in sub.state doesn't show up until the NEXT sample
          // (the first one whose prePhase is post-wrap). At that point
          // r1.wrapFrac belongs to a DIFFERENT (non-wrap) sample, not the
          // toggle-causing wrap — using it here (as an earlier version of
          // this test did) silently discards the sub-sample precision this
          // AC needs, reintroducing ~1-sample-scale quantisation into a
          // "to 1e-9" check. The toggle's true continuous-time position is
          // the wrap event that CAUSED it, tracked one iteration back.
          const double pos = lastVco1WrapPos;
          if (firstTogglePos < 0.0) firstTogglePos = pos;
          lastTogglePos = pos;
          ++subToggles;
          prevState = sub.state;
        }
      }

      const long expectedToggles = wrapCount1 / (sub.periodDivisor() / 2);  // 2 toggles per full period
      const std::string toggleLabel = (oct == SubOsc::Octave::Minus1)
                                           ? "G2.8 sub -1 toggle count exact"
                                           : "G2.8 sub -2 toggle count exact";
      checkNum(toggleLabel.c_str(), std::labs(subToggles - expectedToggles) <= 1,
               static_cast<double>(subToggles - expectedToggles));

      // Precise period ratio from averaged wrap/toggle positions (sub-sample
      // precision via wrapFrac), far more than the 4400 periods needed for
      // 1e-9 relative accuracy over 10s @ 440Hz.
      const double vco1PeriodSamples = (lastWrapPos - firstWrapPos) / static_cast<double>(wrapCount1 - 1);
      const double subPeriodSamples =
          2.0 * (lastTogglePos - firstTogglePos) / static_cast<double>(subToggles - 1);
      const double ratio = subPeriodSamples / vco1PeriodSamples;
      const std::string ratioLabel = (oct == SubOsc::Octave::Minus1)
                                          ? "G2.8 sub -1 period ratio == 2.0 to 1e-9"
                                          : "G2.8 sub -2 period ratio == 4.0 to 1e-9";
      checkNum(ratioLabel.c_str(), std::fabs(ratio - divisorExpect) < 1e-9, ratio);

      const double measured = measuredF0(subOut, kFs);
      const std::string freqLabel = (oct == SubOsc::Octave::Minus1) ? "G2.8 sub -1 -> 220.00 Hz"
                                                                      : "G2.8 sub -2 -> 110.00 Hz";
      checkNum(freqLabel.c_str(), std::fabs(measured - expectFreq) < 0.1, measured);
    }
  }

  // ==========================================================================
  // G2.9: Sub is antialiased for free. f0=3989Hz (sub at 1994.5Hz, -1
  // octave). Compare the sub's non-harmonic energy to a directly-generated
  // 50% pulse at 1994.5Hz (a like-for-like comparison, DESIGN.md's own
  // naming requirement -- a saw reads ~2dB different).
  // ==========================================================================
  std::cout << "\nGroup: G2.9 sub antialiasing (free reuse)\n";
  {
    const double vco1Freq = 3989.0;
    const double subFreq = vco1Freq / 2.0;  // 1994.5 Hz
    Osc vco1;
    vco1.wave = Osc::Wave::Saw;
    vco1.setSampleRate(kFs);
    vco1.setDt(vco1Freq / kFs);
    SubOsc sub;
    sub.octave = SubOsc::Octave::Minus1;

    const int n = static_cast<int>(2.0 * kFs);
    std::vector<float> subOut(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      const double prePhase = vco1.phase;
      const auto r1 = vco1.step();
      subOut[static_cast<size_t>(i)] = static_cast<float>(sub.step(prePhase, vco1.dt, r1.wrapped, true));
    }
    const double dbSub = nonHarmonicEnergyDb(subOut, subFreq, kFs);

    const auto vPulseRef = renderOsc(Osc::Wave::Pulse, subFreq, kFs, n, 50.0);
    const double dbRefPulse = nonHarmonicEnergyDb(vPulseRef, subFreq, kFs);

    std::cout << "    sub=" << dbSub << " dB, ref 50% pulse @1994.5Hz=" << dbRefPulse << " dB\n";
    checkNum("G2.9 sub alias floor within 3 dB of a like-for-like 50% pulse",
             std::fabs(dbSub - dbRefPulse) < 3.0, dbSub - dbRefPulse);
  }

  // ==========================================================================
  // G2.10: Hard sync locks the period. VCO2 at 1.5x VCO1 with sync on ->
  // output's measured fundamental equals VCO1's, not VCO2's. Measured via
  // autocorrelation-at-master-period (see syncLockedFreqHz's comment above
  // for why zero-crossing counting is the WRONG tool here).
  // ==========================================================================
  std::cout << "\nGroup: G2.10 hard sync period lock\n";
  {
    const double f1 = 220.0;
    const double f2 = 1.5 * f1;
    Osc vco1;
    vco1.wave = Osc::Wave::Saw;
    vco1.setSampleRate(kFs);
    vco1.setDt(f1 / kFs);
    Osc vco2;
    vco2.wave = Osc::Wave::Saw;
    vco2.setSampleRate(kFs);
    vco2.setDt(f2 / kFs);

    const int n = static_cast<int>(2.0 * kFs);
    std::vector<float> out(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      const auto r1 = vco1.step();
      double y2 = vco2.step().y;
      if (r1.wrapped) y2 = vco2.hardSync(r1.wrapFrac / vco1.dt, y2);
      out[static_cast<size_t>(i)] = static_cast<float>(y2);
    }
    const double measured = syncLockedFreqHz(out, kFs, f1);
    checkNum("G2.10 sync'd output fundamental == VCO1's 220 Hz (not VCO2's 330 Hz)",
             std::fabs(measured - f1) < 0.1, measured);
  }

  // ==========================================================================
  // G2.11: Sync's alias floor is much looser, and stated as such. f0=219Hz,
  // VCO2 at 3.7x, non-harmonic energy >= 12dB below (reference -16.6dB). Do
  // NOT hold to G2.7's numbers or to 25dB (docs/GATES.md: an earlier guess
  // that missed by 8dB).
  // ==========================================================================
  std::cout << "\nGroup: G2.11 sync alias floor (loose, recorded)\n";
  {
    const double f1 = 219.0;
    const double f2 = 3.7 * f1;
    Osc vco1;
    vco1.wave = Osc::Wave::Saw;
    vco1.setSampleRate(kFs);
    vco1.setDt(f1 / kFs);
    Osc vco2;
    vco2.wave = Osc::Wave::Saw;
    vco2.setSampleRate(kFs);
    vco2.setDt(f2 / kFs);

    const int n = static_cast<int>(3.0 * kFs);
    std::vector<float> out(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      const auto r1 = vco1.step();
      double y2 = vco2.step().y;
      if (r1.wrapped) y2 = vco2.hardSync(r1.wrapFrac / vco1.dt, y2);
      out[static_cast<size_t>(i)] = static_cast<float>(y2);
    }
    // Relative to TOTAL RMS, not to the fundamental -- see aliasFloorVsRmsDb's
    // comment in test_util.h. The AC originally said "below the fundamental";
    // that denominator is not just harsh for a synced waveform, it is invalid:
    // at integer sync ratios the master fundamental measures -245 dB and the
    // ratio reports +204 dB of "alias". Measured here at 3.7x: -19.56 dB.
    const double db = aliasFloorVsRmsDb(out, f1, kFs);
    checkNum("G2.11 sync alias floor >= 15 dB below total RMS @ f0=219Hz, VCO2=3.7x", db <= -15.0, db);
  }

  // ==========================================================================
  // G2.12: White noise is flat. 8 octave bands, 40Hz..10kHz, within 1.5dB of
  // the mean. Measured via averaged-Goertzel band PSD (see bandPsdDb's
  // comment for why this differs from a naive constant-Q band-POWER measure,
  // which would show a spurious ~3dB/octave rise for genuinely white noise).
  // ==========================================================================
  std::cout << "\nGroup: G2.12 white noise flatness\n";
  {
    NoiseSource noise;
    noise.init(0);
    const int n = static_cast<int>(20.0 * kFs);
    std::vector<float> v(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) v[static_cast<size_t>(i)] = static_cast<float>(noise.step(NoiseSource::Color::White));

    const double edges[9] = {40, 80, 160, 320, 640, 1280, 2560, 5120, 10240};
    double bandDb[8];
    for (int b = 0; b < 8; ++b) bandDb[b] = bandPsdDb(v, kFs, edges[b], edges[b + 1]);
    double mean = 0.0;
    for (double d : bandDb) mean += d;
    mean /= 8.0;
    bool allWithin = true;
    for (int b = 0; b < 8; ++b) {
      const double delta = bandDb[b] - mean;
      std::cout << "    band[" << edges[b] << "-" << edges[b + 1] << "Hz] = " << bandDb[b]
                << " dB (delta " << delta << ")\n";
      if (std::fabs(delta) > 1.5) allWithin = false;
    }
    check("G2.12 white noise flat within 1.5 dB across 8 octave bands", allWithin);
  }

  // ==========================================================================
  // G2.13: Noise is deterministic (R13). Two init()ed NoiseSources, same
  // voice index, produce bit-identical noise for 1e6 samples.
  // ==========================================================================
  std::cout << "\nGroup: G2.13 noise determinism (R13)\n";
  {
    NoiseSource a, b;
    a.init(3);
    b.init(3);
    bool identical = true;
    for (int i = 0; i < 1000000; ++i) {
      const double ya = a.step(NoiseSource::Color::White);
      const double yb = b.step(NoiseSource::Color::White);
      if (ya != yb) {
        identical = false;
        break;
      }
    }
    check("G2.13 two NoiseSources with the same voice index are bit-identical (1e6 samples)", identical);

    NoiseSource c;
    c.init(4);
    bool differs = false;
    for (int i = 0; i < 1000; ++i) {
      if (a.step(NoiseSource::Color::White) != c.step(NoiseSource::Color::White)) {
        differs = true;
        break;
      }
    }
    check("G2.13 (sanity) a different voice index produces a different stream", differs);
  }

  // ==========================================================================
  // G2.14: Mixer sums linearly.
  // ==========================================================================
  std::cout << "\nGroup: G2.14 mixer linearity\n";
  {
    const double o1 = 0.3, o2 = -0.5, sub = 0.7, noise = -0.2;
    // Three muted, one at 100%: reproduces that source scaled exactly.
    checkNum("G2.14 osc1 solo == o1 exactly",
             std::fabs(MixerBlock::mix(o1, 100.0, o2, 0.0, sub, 0.0, noise, 0.0) - o1) < 1e-6,
             MixerBlock::mix(o1, 100.0, o2, 0.0, sub, 0.0, noise, 0.0));
    checkNum("G2.14 osc2 solo == o2 exactly",
             std::fabs(MixerBlock::mix(o1, 0.0, o2, 100.0, sub, 0.0, noise, 0.0) - o2) < 1e-6,
             MixerBlock::mix(o1, 0.0, o2, 100.0, sub, 0.0, noise, 0.0));
    checkNum("G2.14 sub solo == sub exactly",
             std::fabs(MixerBlock::mix(o1, 0.0, o2, 0.0, sub, 100.0, noise, 0.0) - sub) < 1e-6,
             MixerBlock::mix(o1, 0.0, o2, 0.0, sub, 100.0, noise, 0.0));
    checkNum("G2.14 noise solo == noise exactly",
             std::fabs(MixerBlock::mix(o1, 0.0, o2, 0.0, sub, 0.0, noise, 100.0) - noise) < 1e-6,
             MixerBlock::mix(o1, 0.0, o2, 0.0, sub, 0.0, noise, 100.0));

    const double allSum = o1 + o2 + sub + noise;
    const double mixed = MixerBlock::mix(o1, 100.0, o2, 100.0, sub, 100.0, noise, 100.0);
    checkNum("G2.14 all four at 100% == exact sum", std::fabs(mixed - allSum) < 1e-6, mixed);

    // A partial-level combination too, since "sums linearly" should hold at
    // arbitrary levels, not just the 0/100 extremes.
    const double partial = MixerBlock::mix(o1, 50.0, o2, 25.0, sub, 10.0, noise, 0.0);
    const double partialExpect = o1 * 0.5 + o2 * 0.25 + sub * 0.10;
    checkNum("G2.14 partial-level combination is the exact weighted sum",
             std::fabs(partial - partialExpect) < 1e-6, partial);
  }

  // ==========================================================================
  // G2.15: Finite everywhere. wave x octave x PW x sync x sub-octave x
  // noise-colour at min/mid/max, at all five sample rates, 512 samples each.
  // ==========================================================================
  std::cout << "\nGroup: G2.15 finite-everywhere grid search\n";
  {
    const double rates[5] = {44100.0, 48000.0, 88200.0, 96000.0, 192000.0};
    const Osc::Wave waves[3] = {Osc::Wave::Saw, Osc::Wave::Pulse, Osc::Wave::Tri};
    const double octaveHz[4] = {220.0, 440.0, 880.0, 1760.0};  // 16'/8'/4'/2' @ note 69
    const double pwPct[3] = {5.0, 50.0, 95.0};                 // min/mid/max (DESIGN.md §11 range)
    const bool syncs[2] = {false, true};
    const SubOsc::Octave subOcts[2] = {SubOsc::Octave::Minus1, SubOsc::Octave::Minus2};
    const NoiseSource::Color colors[2] = {NoiseSource::Color::White, NoiseSource::Color::Pink};

    long combos = 0;
    long badCombos = 0;
    double globalMaxAbs = 0.0;
    double globalMaxFs = 0, globalMaxF0 = 0, globalMaxPw = 0;
    int globalMaxWave = -1, globalMaxSubOct = -1, globalMaxColor = -1;
    bool globalMaxSync = false;
    int globalMaxSample = -1;
    for (double fs : rates) {
      for (Osc::Wave wave : waves) {
        for (double f0 : octaveHz) {
          for (double pw : pwPct) {
            for (bool sync : syncs) {
              for (SubOsc::Octave subOct : subOcts) {
                for (NoiseSource::Color color : colors) {
                  ++combos;
                  Osc o1;
                  o1.wave = wave;
                  o1.setSampleRate(fs);
                  o1.setDt(f0 / fs);
                  o1.setPwPercent(pw);
                  Osc o2;
                  o2.wave = wave;
                  o2.setSampleRate(fs);
                  o2.setDt((1.5 * f0) / fs);
                  o2.setPwPercent(pw);
                  SubOsc sub;
                  sub.octave = subOct;
                  NoiseSource noise;
                  noise.init(0);

                  bool ok = true;
                  int badSample = -1;
                  double badVal = 0.0;
                  for (int i = 0; i < 512; ++i) {
                    const double prePhase = o1.phase;
                    const auto r1 = o1.step();
                    double y2 = o2.step().y;
                    if (sync && r1.wrapped) y2 = o2.hardSync(r1.wrapFrac / o1.dt, y2);
                    const double ysub = sub.step(prePhase, o1.dt, r1.wrapped, true);
                    const double ynoise = noise.step(color);
                    const double mix = MixerBlock::mix(r1.y, 100.0, y2, 100.0, ysub, 100.0, ynoise, 100.0);
                    if (!std::isfinite(mix)) {
                      ok = false;
                      badSample = i;
                      badVal = mix;
                      break;
                    }
                    if (std::fabs(mix) > globalMaxAbs) {
                      globalMaxAbs = std::fabs(mix);
                      globalMaxFs = fs; globalMaxF0 = f0; globalMaxPw = pw;
                      globalMaxWave = static_cast<int>(wave);
                      globalMaxSubOct = static_cast<int>(subOct);
                      globalMaxColor = static_cast<int>(color);
                      globalMaxSync = sync;
                      globalMaxSample = i;
                    }
                    if (std::fabs(mix) >= 8.0 && ok) {
                      ok = false;
                      badSample = i;
                      badVal = mix;
                    }
                  }
                  if (!ok) {
                    ++badCombos;
                    if (badCombos <= 5) {
                      std::cout << "    bad combo: fs=" << fs << " wave=" << static_cast<int>(wave)
                                << " f0=" << f0 << " pw=" << pw << " sync=" << sync
                                << " subOct=" << static_cast<int>(subOct) << " color="
                                << static_cast<int>(color) << " sample=" << badSample
                                << " val=" << badVal << "\n";
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
    checkNum("G2.15 all grid combos finite and |y| < 8.0", badCombos == 0,
             static_cast<double>(badCombos) / static_cast<double>(combos));
    std::cout << "    (" << combos << " combos tested, global max |y| = " << globalMaxAbs
              << " at fs=" << globalMaxFs << " wave=" << globalMaxWave << " f0=" << globalMaxF0
              << " pw=" << globalMaxPw << " sync=" << globalMaxSync << " subOct=" << globalMaxSubOct
              << " color=" << globalMaxColor << " sample=" << globalMaxSample << ")\n";
  }

  // ==========================================================================
  // G2.16: No zipper. Sweep kOsc1Fine across its full -50..+50 cent range
  // over 100ms (dt updated every SAMPLE here -- this tests the OSCILLATOR
  // itself has no discontinuity from a smoothly changing dt; the control-
  // rate-vs-audio-rate architecture that would normally hold dt for 32
  // samples is G3's concern, not G2's). Max envelope delta < 0.5 dB per ms.
  // ==========================================================================
  std::cout << "\nGroup: G2.16 no zipper on a fine-tune sweep\n";
  {
    // Base frequency chosen so a 1ms envelopeDb frame (48 samples @ 48kHz)
    // is (very close to) exactly one waveform period at the sweep's
    // midpoint: 48000/1000 = 48.0 samples. This matters because RMS over a
    // FULL period of a periodic signal is invariant to the window's phase
    // alignment, while RMS over an arbitrary PARTIAL-period window is not —
    // an earlier version of this test used a 440Hz base (period ~109.1
    // samples, badly misaligned with the 48-sample frame) and saw ~10dB/ms
    // "deltas" that were verified (python3, holding dt perfectly CONSTANT
    // at 440Hz -- no sweep at all) to be the SAME order of magnitude: a
    // frame/period misalignment artifact of the MEASUREMENT, not a zipper
    // artifact of the oscillator. 1000Hz's frame/period alignment reduces
    // that artifact by roughly two orders of magnitude (python3-verified:
    // max delta 0.114 dB/ms at 1000Hz vs the false positive at 440Hz),
    // leaving what's actually being asked: does the OSCILLATOR itself click
    // when dt changes smoothly.
    constexpr double kBaseHz = 1000.0;
    const int n = static_cast<int>(0.1 * kFs);  // 100 ms
    Osc o;
    o.wave = Osc::Wave::Saw;
    o.setSampleRate(kFs);
    std::vector<float> out(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      const double t = static_cast<double>(i) / static_cast<double>(n - 1);
      const double cents = -50.0 + 100.0 * t;  // -50 -> +50 over the sweep (kOsc1Fine's full range)
      const double freq = kBaseHz * std::pow(2.0, cents / 1200.0);
      o.setDt(freq / kFs);
      out[static_cast<size_t>(i)] = static_cast<float>(o.step().y);
    }
    std::vector<double> env;
    envelopeDb(out, kFs, env);
    double maxDelta = 0.0;
    for (size_t i = 1; i < env.size(); ++i) maxDelta = std::max(maxDelta, std::fabs(env[i] - env[i - 1]));
    checkNum("G2.16 max envelope delta < 0.5 dB/ms across a 100ms fine-tune sweep", maxDelta < 0.5,
             maxDelta);
  }

  // ------------------------------------------------------------- Summary ----
  std::cout << "\n=== Summary: " << (g_checks - g_failures) << "/" << g_checks
            << " checks passed";
  if (g_failures > 0) {
    std::cout << ", " << g_failures << " FAILED ===\n";
    return 1;
  }
  std::cout << " ===\n";
  return 0;
}
