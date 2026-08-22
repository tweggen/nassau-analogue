// NassauAnalogue DSP Unit Tests — Gate G1 (DSP primitives).
// Hand-rolled harness (plain int main + soft checks, no external framework) —
// the nassau-eq/nassau-zermatt house style (docs/GATES.md "Shared test
// harness"). Covers every G1.1-G1.19 acceptance criterion in docs/GATES.md.
//
// R6: tests measure SIGNALS (drive real samples through TptOnePole/
// PinkFilter/AdsrEnv/Lfo/shapers and look at the output), not private
// coefficient state — EXCEPT G1.1, whose whole point is an independent
// analytic reference computed fresh in this file from the TPT bilinear
// formula quoted in docs/GATES.md, deliberately NOT calling into
// TptOnePole::setFc()/process() to derive it.
//
// R8: no rand()/time(); Xorshift32 (Source/DSP/synth_dsp.h) is the only
// source of noise, always fixed-seeded.
//
// R11 (G0's lesson): every numeric constant asserted here was independently
// verified with python3 -c before being written down — see the per-group
// comments below for what was checked and how.

#include "synth_dsp.h"
#include "test_util.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#if defined(_MSC_VER) || defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#define NASSAU_TEST_HAS_MXCSR 1
#endif

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

// Log-spaced sweep of `count` positive values in [lo, hi].
std::vector<double> logSweep(double lo, double hi, int count) {
  std::vector<double> v;
  v.reserve(static_cast<size_t>(count));
  const double logLo = std::log10(lo);
  const double logHi = std::log10(hi);
  for (int i = 0; i < count; ++i) {
    const double t = (count > 1) ? static_cast<double>(i) / (count - 1) : 0.0;
    v.push_back(std::pow(10.0, logLo + t * (logHi - logLo)));
  }
  return v;
}

} // namespace

int main() {
  std::cout << "=== NassauAnalogue DSP Tests (G1) ===\n\n";

  // ==========================================================================
  // G1.1: TptOnePole LP magnitude vs an INDEPENDENT analytic reference.
  //
  // Reference formula (docs/GATES.md G1.1, quoted verbatim):
  //   |g/(1+g) * (1+z^-1)/(1 - (1-g)/(1+g) z^-1)|
  // computed fresh here via direct complex arithmetic on e^{-jw} -- NOT by
  // calling TptOnePole::setFc()/process(), which would just prove the code
  // agrees with itself.
  // ==========================================================================
  std::cout << "Group: TptOnePole LP magnitude vs independent reference (G1.1)\n";
  {
    auto refTptLpMagDb = [](double fc, double fs, double freq) {
      const double g = std::tan(kTestPi * fc / fs);
      const double Gc = g / (1.0 + g);
      const double a = (1.0 - g) / (1.0 + g);
      const double w = 2.0 * kTestPi * freq / fs;
      const double numRe = Gc * (1.0 + std::cos(w));
      const double numIm = Gc * (-std::sin(w));
      const double denRe = 1.0 - a * std::cos(w);
      const double denIm = a * std::sin(w);
      const double numMag = std::sqrt(numRe * numRe + numIm * numIm);
      const double denMag = std::sqrt(denRe * denRe + denIm * denIm);
      return 20.0 * std::log10(numMag / std::max(denMag, 1e-300));
    };

    const double fcs[] = {20.0, 200.0, 2000.0, 10000.0};
    const double rates[] = {44100.0, 96000.0};
    for (double fs : rates) {
      for (double fc : fcs) {
        // Probes span a practical audio range (20Hz .. min(20kHz, 0.48*fs))
        // rather than sweeping all the way to Nyquist for every fc: at
        // fc=20Hz, probing up to ~0.48*96000=46080Hz drives the filter into
        // ~-190dB of attenuation, where the comparison is dominated by the
        // float32 precision of magDb's own test buffers (not a TptOnePole
        // defect) -- a numerically ill-conditioned dB comparison, not a
        // meaningful check of the filter's shape.
        const auto probes = logSweep(20.0, std::min(20000.0, 0.48 * fs), 24);
        double maxErr = 0.0;
        for (double freq : probes) {
          TptOnePole pole;
          pole.setFc(fc, fs);
          const double measured = magDb(
              [&](const float* in, float* out, int n) {
                for (int i = 0; i < n; ++i) out[i] = static_cast<float>(pole.process(in[i]));
              },
              freq, fs);
          const double ref = refTptLpMagDb(fc, fs, freq);
          maxErr = std::max(maxErr, std::fabs(measured - ref));
        }
        std::string label = "TptOnePole LP matches independent reference within 0.1 dB (fc=" +
                             std::to_string(static_cast<int>(fc)) +
                             "Hz, fs=" + std::to_string(static_cast<int>(fs)) + "Hz, 24 probes)";
        checkNum(label.c_str(), maxErr < 0.1, maxErr);
      }
    }
  }

  // ==========================================================================
  // G1.2: TptOnePole HP output equals x - lp exactly, and has zero DC gain.
  // ==========================================================================
  std::cout << "\nGroup: TptOnePole HP == x - lp, zero DC gain (G1.2)\n";
  {
    const double fs = 48000.0;
    TptOnePole pole;
    pole.setFc(500.0, fs);
    Xorshift32 rng(0xC0FFEEu);
    bool hpExact = true;
    for (int i = 0; i < 100000; ++i) {
      const double x = rng.nextBipolar() * 2.0;
      const double lp = pole.process(x);
      if (pole.hp() != x - lp) hpExact = false;
    }
    check("TptOnePole::hp() == x - lp() exactly, 100000 random samples", hpExact);

    // Zero DC gain: constant 1.0 for 5 s -> final |hp| < 1e-9.
    TptOnePole poleDc;
    poleDc.setFc(200.0, fs);
    const int nDc = static_cast<int>(5.0 * fs);
    double lastHp = 0.0;
    for (int i = 0; i < nDc; ++i) {
      poleDc.process(1.0);
      lastHp = poleDc.hp();
    }
    checkNum("TptOnePole HP has zero DC gain: |hp| < 1e-9 after 5 s of a constant 1.0 input",
             std::fabs(lastHp) < 1e-9, lastHp);
  }

  // ==========================================================================
  // G1.3: TptOnePole is stable and finite for fc in [10 Hz, 0.45*fs], all
  // five supported rates, 10 s of Xorshift noise.
  // ==========================================================================
  std::cout << "\nGroup: TptOnePole stability across fc/fs (G1.3)\n";
  {
    const double rates[] = {44100.0, 48000.0, 88200.0, 96000.0, 192000.0};
    bool allFinite = true;
    double maxAbs = 0.0;
    for (double fs : rates) {
      const auto fcs = logSweep(10.0, 0.45 * fs, 6);
      for (double fc : fcs) {
        TptOnePole pole;
        pole.setFc(fc, fs);
        Xorshift32 rng(static_cast<uint32_t>(fs) ^ static_cast<uint32_t>(fc * 1000.0) ^ 0xABCDu);
        const int n = static_cast<int>(10.0 * fs);
        for (int i = 0; i < n; ++i) {
          const double y = pole.process(rng.nextBipolar());
          if (!std::isfinite(y)) allFinite = false;
          maxAbs = std::max(maxAbs, std::fabs(y));
        }
      }
    }
    check("TptOnePole stays finite for fc in [10Hz, 0.45*fs] at all 5 rates, 10s of noise each",
          allFinite);
    checkNum("TptOnePole |y| < 4 across the same sweep", maxAbs < 4.0, maxAbs);
  }

  // ==========================================================================
  // G1.4/G1.5: polyBlep boundary/continuity/antisymmetry.
  // ==========================================================================
  std::cout << "\nGroup: polyBlep boundary behaviour (G1.4-G1.5)\n";
  {
    const double dt = 0.05;
    bool zeroOutside = true;
    for (int i = 0; i <= 1000; ++i) {
      const double t = dt + (1.0 - 2.0 * dt) * (static_cast<double>(i) / 1000.0);
      if (t <= dt || t >= 1.0 - dt) continue;
      if (polyBlep(t, dt) != 0.0) zeroOutside = false;
    }
    check("polyBlep is exactly 0 strictly inside [dt, 1-dt]", zeroOutside);

    double lastNearLow = 1.0, lastNearHigh = 1.0;
    for (double eps : {1e-2, 1e-3, 1e-4, 1e-6, 1e-9}) {
      lastNearLow = std::fabs(polyBlep(dt - eps, dt));
      lastNearHigh = std::fabs(polyBlep(1.0 - dt + eps, dt));
    }
    checkNum("polyBlep(dt-eps, dt) -> 0 as eps -> 0", lastNearLow < 1e-8, lastNearLow);
    checkNum("polyBlep(1-dt+eps, dt) -> 0 as eps -> 0", lastNearHigh < 1e-8, lastNearHigh);

    bool antisym = true;
    double maxAntisymErr = 0.0;
    for (int i = 1; i <= 200; ++i) {
      const double t = dt * static_cast<double>(i) / 201.0;
      const double a = polyBlep(t, dt);
      const double b = polyBlep(1.0 - t, dt);
      const double err = std::fabs(a - (-b));
      maxAntisymErr = std::max(maxAntisymErr, err);
      if (err > 1e-12) antisym = false;
    }
    checkNum("polyBlep(1-t,dt) == -polyBlep(t,dt) for 200 t in (0,dt)", antisym, maxAntisymErr);
  }

  // ==========================================================================
  // G1.6/G1.7: Xorshift32 statistics and reproducibility.
  // ==========================================================================
  std::cout << "\nGroup: Xorshift32 (G1.6-G1.7)\n";
  {
    Xorshift32 rng(0x9E3779B9u);
    const long long N = 10000000;
    double sum = 0.0, sumSq = 0.0;
    uint32_t prevRaw = 0;
    bool everRepeated = false;
    bool first = true;
    for (long long i = 0; i < N; ++i) {
      const uint32_t raw = rng.next();
      if (!first && raw == prevRaw) everRepeated = true;
      first = false;
      prevRaw = raw;
    }
    // nextBipolar() statistics on a fresh, identically-seeded stream (next()
    // above already consumed the state; re-seed for a clean pass).
    Xorshift32 rng2(0x9E3779B9u);
    for (long long i = 0; i < N; ++i) {
      const double v = rng2.nextBipolar();
      sum += v;
      sumSq += v * v;
    }
    const double mean = sum / static_cast<double>(N);
    const double var = sumSq / static_cast<double>(N) - mean * mean;
    checkNum("Xorshift32 mean over 1e7 draws in [-0.01, 0.01]", mean >= -0.01 && mean <= 0.01,
             mean);
    checkNum("Xorshift32 variance over 1e7 draws in [0.32, 0.34] (uniform[-1,1) = 1/3)",
             var >= 0.32 && var <= 0.34, var);
    check("Xorshift32 never repeats its immediate predecessor over 1e7 draws", !everRepeated);

    Xorshift32 a(0x1357u), b(0x1357u);
    bool identical = true;
    for (long long i = 0; i < 1000000; ++i) {
      if (a.next() != b.next()) identical = false;
    }
    check("Two Xorshift32 with the same seed are bit-identical for 1e6 draws (R13)", identical);
  }

  // ==========================================================================
  // G1.8: PinkFilter slope, 3.0 +/- 0.7 dB per octave, 8 octave-pair probes
  // 40 Hz -> 10240 Hz at fs = 48 kHz.
  //
  // Measured via sine-wave magnitude response (PinkFilter is LTI -- three
  // one-poles plus a direct path -- so its "spectrum" is exactly its
  // magnitude response; a literal noise-driven periodogram measurement adds
  // substantial statistical variance ON TOP of the filter's own ~0.5 dB
  // published approximation error and was verified by hand (python3
  // simulation, see the G1 gate note) to occasionally exceed the 0.7 dB
  // tolerance on a CORRECT filter -- the sine-response measurement below is
  // both noise-free and exactly what "spectrum falls N dB/octave" means for
  // an LTI filter).
  // ==========================================================================
  std::cout << "\nGroup: PinkFilter slope (G1.8)\n";
  {
    const double fs = 48000.0;
    auto pinkMagDb = [&](double freq) {
      PinkFilter pf;
      return magDb(
          [&](const float* in, float* out, int n) {
            for (int i = 0; i < n; ++i) out[i] = static_cast<float>(pf.process(in[i]));
          },
          freq, fs);
    };
    const double probes[] = {40.0, 80.0, 160.0, 320.0, 640.0, 1280.0, 2560.0, 5120.0};
    bool allInRange = true;
    double worstDev = 0.0;
    for (double f : probes) {
      const double delta = pinkMagDb(2.0 * f) - pinkMagDb(f);
      const double dev = std::fabs(delta - (-3.0));
      worstDev = std::max(worstDev, dev);
      std::string label = "PinkFilter octave-pair " + std::to_string(static_cast<int>(f)) + "Hz->" +
                           std::to_string(static_cast<int>(2.0 * f)) +
                           "Hz within 3.0+/-0.7 dB (measured delta)";
      checkNum(label.c_str(), dev <= 0.7, delta);
      if (dev > 0.7) allInRange = false;
    }
    check("all 8 PinkFilter octave-pair probes within tolerance", allInRange);
    (void)worstDev;
  }

  // ==========================================================================
  // AdsrEnv (G1.9-G1.14). Times are derived divisors, computed via std::log
  // inside AdsrEnv itself (not hand-transcribed) -- see synth_dsp.h.
  // Independently verified with python3 before writing any tolerance below:
  //   ln(1.15/0.15) = 2.03688192726104   (attack)
  //   ln(100)       = 4.605170185988092  (decay)
  //   ln(1.05/0.05) = 3.044522437723423  (release)
  //   t(50%)/t(99%) = 0.28927257631518993 during attack
  // ==========================================================================
  std::cout << "\nGroup: AdsrEnv attack timing (G1.9, G1.11)\n";
  {
    struct Case { double fs; double ms; };
    const Case cases[] = {
        {44100.0, 10.0}, {44100.0, 100.0}, {44100.0, 1000.0},
        {96000.0, 10.0}, {96000.0, 100.0}, {96000.0, 1000.0},
    };
    for (const auto& c : cases) {
      const double fsControl = c.fs / 32.0;  // DESIGN.md §2 kControlBlock = 32
      AdsrEnv env;
      env.setAttackMs(c.ms, fsControl);
      env.setDecayMs(1000.0, fsControl);
      env.setSustainPercent(50.0);
      env.setReleaseMs(1000.0, fsControl);
      env.noteOn();
      long long steps = 0;
      while (env.y < 0.99 && steps < 10000000) {
        env.step();
        ++steps;
      }
      const double actualMs = static_cast<double>(steps) / fsControl * 1000.0;
      const double controlStepMs = 1000.0 / fsControl;
      const double relTol = 0.15 * c.ms;
      const double tol = (c.ms < 10.0) ? std::max(relTol, 1.5 * controlStepMs) : relTol;
      std::string label = "AdsrEnv attack " + std::to_string(static_cast<int>(c.ms)) +
                           "ms @ fs=" + std::to_string(static_cast<int>(c.fs)) +
                           " reaches 0.99 within tolerance";
      checkNum(label.c_str(), std::fabs(actualMs - c.ms) <= tol, actualMs);
    }

    // G1.11: explicit short-time (<10ms) cases at fs=44100 -- tolerance is
    // 15% OR 1.5 control blocks, whichever is larger. 1 control block at
    // fs=44100/32 is 1000/(44100/32) = 0.7256 ms (verified: 32/44100*1000 =
    // 0.7256235827664399 -- matches docs/GATES.md's own "0.726 ms").
    const double shortMs[] = {1.0, 2.0, 5.0};
    const double fsControl44k = 44100.0 / 32.0;
    for (double ms : shortMs) {
      AdsrEnv env;
      env.setAttackMs(ms, fsControl44k);
      env.noteOn();
      long long steps = 0;
      while (env.y < 0.99 && steps < 1000000) {
        env.step();
        ++steps;
      }
      const double actualMs = static_cast<double>(steps) / fsControl44k * 1000.0;
      const double controlStepMs = 1000.0 / fsControl44k;
      const double tol = std::max(0.15 * ms, 1.5 * controlStepMs);
      std::string label = "AdsrEnv G1.11 short attack " + std::to_string(ms) +
                           "ms: quantisation-aware tolerance " + std::to_string(tol) + "ms";
      checkNum(label.c_str(), std::fabs(actualMs - ms) <= tol, actualMs);
    }
  }

  std::cout << "\nGroup: AdsrEnv decay/release timing (G1.10)\n";
  {
    const double fsControl = 48000.0 / 32.0;
    AdsrEnv env;
    env.setAttackMs(1.0, fsControl);  // fast attack: get to Decay quickly
    env.setDecayMs(200.0, fsControl);
    env.setSustainPercent(30.0);
    env.setReleaseMs(300.0, fsControl);
    env.noteOn();
    long long steps = 0;
    while (env.state == AdsrEnv::State::Attack && steps < 1000000) {
      env.step();
      ++steps;
    }
    long long decaySteps = 0;
    while (env.state == AdsrEnv::State::Decay && decaySteps < 1000000) {
      env.step();
      ++decaySteps;
    }
    const double decayMs = static_cast<double>(decaySteps) / fsControl * 1000.0;
    checkNum("AdsrEnv decay reaches within 1% of sustain in the requested 200ms",
             std::fabs(decayMs - 200.0) <= 0.15 * 200.0, decayMs);

    // Release from a full-scale sustain (100%) reaches 0 in the requested time.
    AdsrEnv env2;
    env2.setAttackMs(1.0, fsControl);
    env2.setDecayMs(1.0, fsControl);
    env2.setSustainPercent(100.0);  // full-scale sustain
    env2.setReleaseMs(250.0, fsControl);
    env2.noteOn();
    long long warm = 0;
    while (env2.state != AdsrEnv::State::Sustain && warm < 1000000) {
      env2.step();
      ++warm;
    }
    env2.noteOff();
    long long relSteps = 0;
    while (env2.state == AdsrEnv::State::Release && relSteps < 1000000) {
      env2.step();
      ++relSteps;
    }
    const double relMs = static_cast<double>(relSteps) / fsControl * 1000.0;
    checkNum("AdsrEnv release from full-scale sustain reaches 0 in the requested 250ms",
             std::fabs(relMs - 250.0) <= 0.15 * 250.0, relMs);
  }

  std::cout << "\nGroup: AdsrEnv is exponential (G1.12)\n";
  {
    const double fsControl = 48000.0 / 32.0;
    AdsrEnv env;
    env.setAttackMs(100.0, fsControl);
    env.noteOn();
    long long steps = 0, t50 = -1, t99 = -1;
    while (steps < 1000000 && (t50 < 0 || t99 < 0)) {
      env.step();
      ++steps;
      if (t50 < 0 && env.y >= 0.5) t50 = steps;
      if (t99 < 0 && env.y >= 0.99) t99 = steps;
    }
    const double ratio = static_cast<double>(t50) / static_cast<double>(t99);
    checkNum("AdsrEnv t(50%)/t(99%) = 0.289 +/- 0.03 (exponential, not linear -- linear gives 0.505)",
             std::fabs(ratio - 0.28927257631518993) <= 0.03, ratio);
  }

  std::cout << "\nGroup: AdsrEnv bounds and Idle exactness (G1.13)\n";
  {
    const double fsControl = 44100.0 / 32.0;
    AdsrEnv env;
    env.setAttackMs(20.0, fsControl);
    env.setDecayMs(30.0, fsControl);
    env.setSustainPercent(70.0);
    env.setReleaseMs(40.0, fsControl);
    env.noteOn();
    bool inBounds = true;
    for (int i = 0; i < 20000; ++i) {
      env.step();
      if (env.y < 0.0 || env.y > 1.0) inBounds = false;
      if (i == 15000) env.noteOff();
    }
    check("AdsrEnv never leaves [0, 1] across a full attack/decay/sustain/release cycle", inBounds);

    // Run well past release completion -> must be exactly 0.0 in Idle.
    bool idleExactZero = true;
    for (int i = 0; i < 5000; ++i) {
      env.step();
      if (env.state == AdsrEnv::State::Idle && env.y != 0.0) idleExactZero = false;
    }
    check("AdsrEnv is Idle and y == 0.0 exactly well after release completes",
          env.isIdle() && env.y == 0.0 && idleExactZero);
  }

  std::cout << "\nGroup: AdsrEnv jump-free transitions (G1.14)\n";
  {
    const double fsControl = 48000.0 / 32.0;
    AdsrEnv env;
    env.setAttackMs(500.0, fsControl);  // slow, so we can catch it mid-attack
    env.setReleaseMs(200.0, fsControl);
    env.noteOn();
    for (int i = 0; i < 5; ++i) env.step();  // partway into Attack, y well below 1.0
    const double yBeforeOff = env.y;
    env.noteOff();
    const double yAfterOff = env.y;
    checkNum("note-off during Attack: y unchanged by the state transition itself (no jump)",
             std::fabs(yAfterOff - yBeforeOff) <= 1e-6, std::fabs(yAfterOff - yBeforeOff));
    check("note-off during Attack transitions straight to Release", env.state == AdsrEnv::State::Release);

    for (int i = 0; i < 5; ++i) env.step();  // partway into Release, y > 0
    const double yBeforeOn = env.y;
    env.noteOn();
    const double yAfterOn = env.y;
    checkNum("note-on during Release: y unchanged by the state transition itself (no jump)",
             std::fabs(yAfterOn - yBeforeOn) <= 1e-6, std::fabs(yAfterOn - yBeforeOn));
    check("note-on during Release transitions straight to Attack", env.state == AdsrEnv::State::Attack);
  }

  // ==========================================================================
  // Lfo (G1.15-G1.17).
  // ==========================================================================
  std::cout << "\nGroup: Lfo frequency, range, zero mean (G1.15)\n";
  {
    const double fsControl = 1500.0;  // e.g. 48000/32
    const double rateHzList[] = {0.05, 1.0, 5.0, 30.0};
    for (double rateHz : rateHzList) {
      Lfo lfo;
      lfo.init(0xF00Du);
      lfo.wave = Lfo::Wave::Triangle;
      lfo.setRateHz(rateHz, fsControl);
      // Enough duration for >= 10 cycles (or >= 60s for the slowest rate).
      const double durationS = std::max(60.0, 10.0 / rateHz);
      const int n = static_cast<int>(durationS * fsControl);
      std::vector<float> samples(static_cast<size_t>(n));
      for (int i = 0; i < n; ++i) samples[static_cast<size_t>(i)] = static_cast<float>(lfo.step(fsControl));
      const double measured = measuredF0(samples, fsControl);
      const double relErr = std::fabs(measured - rateHz) / rateHz;
      std::string label = "Lfo Triangle frequency accurate to 1% at " + std::to_string(rateHz) + " Hz";
      checkNum(label.c_str(), relErr <= 0.01, measured);
    }

    // All five waves stay in [-1, 1]; Tri/Saw/Ramp/Square are zero-mean over
    // a whole number of cycles (SampleHold is excluded per docs/GATES.md).
    //
    // The cycle boundary is detected DYNAMICALLY (phase wrapping below its
    // previous value), not assumed from a fixed step count: `increment`
    // (e.g. 1/300) is not exactly representable in binary floating point,
    // so repeated += accumulation drifts, and a cycle actually contains the
    // wrap's real step count (verified: summing 1.0/300.0 three hundred
    // times in double gives 0.9999999999999961, one ULP short of 1.0, so
    // the 300th step does NOT wrap -- it happens on the 301st). Averaging
    // over a window that assumes a fixed 300 steps/cycle therefore samples
    // a slightly-off-boundary window and reintroduces exactly the kind of
    // bias this check exists to rule out. Summing whole, dynamically
    // detected cycles sidesteps that entirely.
    const Lfo::Wave waves[] = {Lfo::Wave::Triangle, Lfo::Wave::Saw, Lfo::Wave::Ramp,
                               Lfo::Wave::Square, Lfo::Wave::SampleHold};
    const char* waveNames[] = {"Triangle", "Saw", "Ramp", "Square", "SampleHold"};
    for (int w = 0; w < 5; ++w) {
      Lfo lfo;
      lfo.init(0xBEEFu + static_cast<uint32_t>(w));
      lfo.wave = waves[w];
      lfo.setRateHz(5.0, fsControl);  // ~300 steps/cycle

      double sum = 0.0;
      long long count = 0;
      int wrapsAfterStart = 0;  // completed whole cycles included in sum/count
      bool started = false;     // true once the leading partial cycle has been discarded
      bool inRange = true;
      double prevPhase = lfo.phase;
      for (int i = 0; i < 200000 && wrapsAfterStart < 10; ++i) {
        const double v = lfo.step(fsControl);
        if (v < -1.0 - 1e-12 || v > 1.0 + 1e-12) inRange = false;
        const bool wrapped = lfo.phase < prevPhase;
        prevPhase = lfo.phase;
        if (!started) {
          // Discard samples up to and including the first wrap (a partial
          // leading cycle -- its length depends on the arbitrary starting
          // phase, not on the waveform, so it is not part of "a whole
          // number of cycles").
          if (wrapped) started = true;
          continue;
        }
        sum += v;
        ++count;
        if (wrapped) ++wrapsAfterStart;  // this sample completed a whole cycle
      }
      std::string rangeLabel = std::string("Lfo ") + waveNames[w] + " stays within [-1,1]";
      check(rangeLabel.c_str(), inRange);
      if (waves[w] != Lfo::Wave::SampleHold) {
        const double mean = (count > 0) ? sum / static_cast<double>(count) : 1.0;
        std::string meanLabel = std::string("Lfo ") + waveNames[w] + " zero mean over " +
                                 std::to_string(wrapsAfterStart) +
                                 " dynamically-detected whole cycles (" + std::to_string(count) +
                                 " samples)";
        checkNum(meanLabel.c_str(), std::fabs(mean) < 1e-6, mean);
      }
    }
  }

  std::cout << "\nGroup: Lfo sample & hold (G1.16)\n";
  {
    // Cycle boundaries are detected DYNAMICALLY (phase wrap), not assumed
    // from a fixed step count -- see the G1.15 zero-mean group's comment
    // above for why a fixed "300 steps/cycle" assumption does not exactly
    // match the real wrap timing under floating-point phase accumulation.
    const double fsControl = 1500.0;
    Lfo lfo;
    lfo.init(0x5EEDu);
    lfo.wave = Lfo::Wave::SampleHold;
    lfo.setRateHz(5.0, fsControl);  // ~300 steps/cycle

    // A wrap detected on step N draws the new shValue for use starting on
    // step N+1 (synth_dsp.h's step() computes `raw` from the OLD shValue,
    // THEN advances phase and redraws) -- so the value change is expected
    // on the step AFTER a detected wrap, not on the wrap step itself.
    bool constantBetweenWraps = true;  // never changes except right after a wrap
    int changeAfterWrapCount = 0;
    int wrapCount = 0;
    bool prevWrapped = false;
    double prevValue = lfo.step(fsControl);
    double prevPhase = lfo.phase;
    const int kTotalSteps = 300 * 25;  // ~25 cycles' worth
    for (int i = 1; i < kTotalSteps; ++i) {
      const double v = lfo.step(fsControl);
      const bool wrapped = lfo.phase < prevPhase;
      prevPhase = lfo.phase;
      if (prevWrapped) {
        ++wrapCount;
        if (v != prevValue) ++changeAfterWrapCount;
      } else {
        if (v != prevValue) constantBetweenWraps = false;
      }
      prevWrapped = wrapped;
      prevValue = v;
    }
    check("Lfo SampleHold holds an exactly constant value strictly between wraps",
          constantBetweenWraps);
    checkNum("Lfo SampleHold changes value at (almost) every actual cycle wrap",
             changeAfterWrapCount >= wrapCount - 1, static_cast<double>(changeAfterWrapCount));
  }

  std::cout << "\nGroup: Lfo delay and ramp-in (G1.17)\n";
  {
    const double fsControl = 1500.0;
    Lfo lfo;
    lfo.init(0xD00Du);
    lfo.wave = Lfo::Wave::Square;
    lfo.setRateHz(0.0, fsControl);  // frozen phase at 0 -> raw output pinned to +1.0
    lfo.setDelayMs(500.0);
    lfo.reset();
    lfo.noteOnEdge();

    const int stepsBeforeDelay = static_cast<int>(0.5 * fsControl);  // 500ms
    bool exactZero = true;
    for (int i = 0; i < stepsBeforeDelay; ++i) {
      if (lfo.step(fsControl) != 0.0) exactZero = false;
    }
    check("Lfo output is exactly 0 for the first 500ms of the delay", exactZero);

    // Now step through the 200ms ramp and confirm it reaches ~1.0 by the end,
    // roughly linearly (5% tolerance, matching the AC).
    const int stepsRamp = static_cast<int>(0.200 * fsControl);
    double midVal = 0.0, endVal = 0.0;
    for (int i = 0; i < stepsRamp; ++i) {
      const double v = lfo.step(fsControl);
      if (i == stepsRamp / 2) midVal = v;
      if (i == stepsRamp - 1) endVal = v;
    }
    checkNum("Lfo ramps to ~0.5 depth at the ramp midpoint (+/-5%)",
             std::fabs(midVal - 0.5) <= 0.05, midVal);
    checkNum("Lfo reaches full depth (~1.0) by the end of the 200ms ramp (+/-5%)",
             std::fabs(endVal - 1.0) <= 0.05, endVal);
  }

  // ==========================================================================
  // G1.18: ScopedNoDenormals actually flushes denormals on x86-64 (not a
  // silent no-op -- Zermatt's R2 amendment exists precisely because this
  // degraded to one on MSVC), and restores the FP control word exactly.
  // ==========================================================================
  std::cout << "\nGroup: ScopedNoDenormals (G1.18)\n";
  {
#ifdef NASSAU_TEST_HAS_MXCSR
    const unsigned int before = _mm_getcsr();
    {
      ScopedNoDenormals guard;
      (void)guard;
    }
    const unsigned int after = _mm_getcsr();
    check("ScopedNoDenormals restores MXCSR exactly on scope exit", before == after);

    // Functional check: a product that underflows to a denormal float must
    // actually flush to zero WHILE the guard is active, and must NOT flush
    // once the guard has exited -- i.e. FTZ/DAZ are genuinely toggled, not
    // just the MXCSR bit pattern. `volatile` prevents the optimizer from
    // constant-folding the multiply at compile time.
    volatile float tinyA = std::numeric_limits<float>::min();  // smallest normal float
    volatile float halfB = 0.5f;
    const float outsideGuard = tinyA * halfB;
    const bool subnormalOutside = (std::fpclassify(outsideGuard) == FP_SUBNORMAL);

    float insideGuard = 0.0f;
    {
      ScopedNoDenormals guard;
      (void)guard;
      volatile float a2 = std::numeric_limits<float>::min();
      volatile float b2 = 0.5f;
      insideGuard = a2 * b2;
    }
    const bool flushedInside = (std::fpclassify(insideGuard) == FP_ZERO);

    checkNum("without the guard, a denormal product is a genuine subnormal (sanity check)",
             subnormalOutside, static_cast<double>(outsideGuard));
    check("ScopedNoDenormals actually flushes denormals to zero while active (not a no-op)",
          flushedInside);
#else
    check("ScopedNoDenormals MXCSR check (skipped: no MXCSR access on this platform)", true);
#endif
    // Construct/destruct doesn't crash even where it is a portable no-op
    // fallback (e.g. a non-x86/non-aarch64 target).
    {
      ScopedNoDenormals guard;
      (void)guard;
    }
    check("ScopedNoDenormals RAII construct/destruct completes without crashing", true);
  }

  // ==========================================================================
  // G1.19: shapeTriodeK(x,0) == x bit-exactly; shapeCubic(0.25, 2.0) ==
  // 0.251922607421875 exactly (BOTH the cubic and quintic terms, not just
  // the cubic -- independently verified with python3:
  //   x=0.25; L=2.0
  //   x + x**3/(2*L**2) - x**5/(2*L**4) == 0.251922607421875
  //   20*log10(0.251922607421875/0.25) == 0.06654267934... dB
  // -- 0.251953125 (cubic term alone) is the WRONG number this AC exists to
  // catch.)
  // ==========================================================================
  std::cout << "\nGroup: shapeTriodeK / shapeCubic (G1.19)\n";
  {
    Xorshift32 rng(0x5A5A5A5Au);
    bool exactAtZero = true;
    for (int i = 0; i < 4096; ++i) {
      const double x = rng.nextBipolar() * 10.0;
      if (shapeTriodeK(x, 0.0) != x) exactAtZero = false;
    }
    check("shapeTriodeK(x,0) == x bit-exactly, 4096 random x", exactAtZero);

    const double y = shapeCubic(0.25, 2.0);
    checkNum("shapeCubic(0.25, 2.0) == 0.251922607421875 exactly (both cubic AND quintic terms)",
             std::fabs(y - 0.251922607421875) < 1e-12, y);
    const double gainDb = 20.0 * std::log10(y / 0.25);
    checkNum("shapeCubic(0.25,2.0) corresponds to +0.06654 dB", std::fabs(gainDb - 0.06654267934) < 1e-4,
             gainDb);
    checkNum("shapeCubic(0.25,2.0) is NOT the cubic-only value 0.251953125 (the bug this AC exists to catch)",
              std::fabs(y - 0.251953125) > 1e-9, y);
  }

  // ==========================================================================
  // Bonus: light self-checks of the new test_util.h helpers themselves
  // (measuredF0, minus3dBPoint) against known signals -- not a numbered G1
  // AC, but these tools are relied on by every later gate, so a basic sanity
  // check belongs here rather than being taken on faith.
  // ==========================================================================
  std::cout << "\nGroup: test_util.h harness self-checks (not a numbered AC)\n";
  {
    const double fs = 48000.0;
    const int n = static_cast<int>(2.0 * fs);
    std::vector<float> sine(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
      sine[static_cast<size_t>(i)] = static_cast<float>(std::sin(2.0 * kTestPi * 440.0 * i / fs));
    const double f0 = measuredF0(sine, fs);
    checkNum("measuredF0 recovers a known 440 Hz sine to within 0.1 Hz", std::fabs(f0 - 440.0) < 0.1, f0);

    TptOnePole pole;
    pole.setFc(1000.0, fs);
    auto poleBlock = [&](const float* in, float* out, int nn) {
      for (int i = 0; i < nn; ++i) out[i] = static_cast<float>(pole.process(in[i]));
    };
    const double corner = minus3dBPoint(poleBlock, fs, 10.0, 100.0, 10000.0);
    checkNum("minus3dBPoint locates TptOnePole's corner within 2% of fc=1000Hz",
             std::fabs(corner - 1000.0) / 1000.0 < 0.02, corner);
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
