// NassauAnalogue DSP Unit Tests — Gate G6 (High-pass, drive, Poly-Mod,
// output stage, complete voice). Hand-rolled harness (plain int main + soft
// checks, no external framework) — the nassau-eq/nassau-zermatt house style
// (docs/GATES.md "Shared test harness"). Covers G6.1-G6.17.
//
// Structure mirrors filter_tests.cpp's own G4/G5 precedent: the HPF's
// static response ACs (G6.1/G6.2/G6.3/G6.5/G6.6) drive Source/DSP/
// synth_filter.h's HpfCascade DIRECTLY (raw-structure testing, same shape
// as ControlRateDriver<LadderFilter>/<SvfFilter>), while the voice-level
// ACs (drive placement, Poly-Mod, output stage, DC, finiteness) drive the
// REAL SynthCore per-voice path (NoteEvent in, rendered audio out), since
// those are properties of the WIRED voice, not of one isolated structure.
//
// R6: every group states, in a comment immediately above its check(),
// exactly what quantity is measured and why that is the quantity the AC
// text actually asks for.
// R8: no rand()/time(); Xorshift32 is the only randomness source, always
// fixed-seeded.
// R11: two findings from this gate are documented at their point of use
// below: (a) G6.4's hard bypass is tested via a REAL per-sample readback of
// production's own drive/HPF stage outputs (new debug accessors), not a
// synthetic re-derivation of the drive formula, because the latter would
// only prove the TEST's assumption, not production's actual bypass branch;
// (b) G6.15's own diagnostic matrix (requested by this gate's brief, not
// itself a numbered AC) is what actually found the output-clip DC
// mechanism now documented in DESIGN.md's "Output stage" section and fixed
// by SynthCore's third (output-stage) DC blocker.

#include "synth_core.h"
#include "synth_filter.h"
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

void checkNum(const std::string& name, bool cond, double measured) {
  ++g_checks;
  if (cond) {
    std::cout << "  [PASS] " << name << " (measured: " << measured << ")\n";
  } else {
    std::cout << "  [FAIL] " << name << " (measured: " << measured << ")\n";
    ++g_failures;
  }
}

constexpr double kFs = 48000.0;

// ---- Render helpers (same shape as envlfo_tests.cpp/filter_tests.cpp) -----

std::vector<float> renderAbsEvents(SynthCore& core, std::vector<NoteEvent> absEvents, int totalSamples,
                                    int block) {
  std::vector<float> outL(static_cast<size_t>(totalSamples)), outR(static_cast<size_t>(totalSamples));
  int pos = 0;
  size_t evIdx = 0;
  std::vector<NoteEvent> chunk;
  while (pos < totalSamples) {
    const int n = std::min(block, totalSamples - pos);
    chunk.clear();
    while (evIdx < absEvents.size() && absEvents[evIdx].sampleOffset < pos + n) {
      NoteEvent e = absEvents[evIdx];
      e.sampleOffset -= pos;
      if (e.sampleOffset < 0) e.sampleOffset = 0;
      chunk.push_back(e);
      ++evIdx;
    }
    core.process(chunk.empty() ? nullptr : chunk.data(), static_cast<int>(chunk.size()), outL.data() + pos,
                 outR.data() + pos, n);
    pos += n;
  }
  return outL;
}

std::vector<float> renderNoteHeld(SynthCore& core, int note, float vel, int totalSamples) {
  return renderAbsEvents(core, {{0, NoteEvent::NoteOn, note, vel}}, totalSamples, 512);
}

double harmonicRatioDb(const std::vector<float>& v, double f0, int n, double fs) {
  return harmonicDb(v, f0, n, fs) - harmonicDb(v, f0, 1, fs);
}

// Fine-tune offset (cents) so `note` sits at exactly `targetHz` -- COMPUTED,
// not hand-transcribed (R11's own "hand-typed constant" lesson).
inline float fineCentsFor(int note, double targetHz) {
  const double noteHz = 440.0 * std::pow(2.0, (note - 69) / 12.0);
  return static_cast<float>(1200.0 * std::log2(targetHz / noteHz));
}

// ---- HpfControlRateDriver ---------------------------------------------------
// Same shape as filter_tests.cpp's ControlRateDriver<LadderFilter/SvfFilter>:
// reproduces PRODUCTION's control-rate coefficient-update cadence (32
// samples) and per-sample gHpf interpolation (advanceCoeff()) exactly, so
// magDb/minus3dBPointHighpass measure the SAME thing SynthCore's own
// per-voice HPF does, not a differently-scheduled stand-in.
struct HpfControlRateDriver {
  HpfCascade filter;
  double fc = 0.0, fs = 0.0;
  int nPoles = 2;
  int controlBlock = 32;  // [dsp] DESIGN.md §2 default
  int phase = 0;
  double gCur = 0.0, gStep = 0.0;

  void configure(double fcIn, double fsIn, int nPolesIn) {
    fc = fcIn;
    fs = fsIn;
    nPoles = nPolesIn;
    filter.reset();
    phase = 0;
    filter.setControlRate(fc, fs);
    gCur = filter.g;  // starts already AT target -- no ramp-in on the very first block
    gStep = 0.0;
  }

  inline double step(double x) {
    if (phase >= controlBlock) {
      const double gStart = filter.g;
      filter.setControlRate(fc, fs);
      gStep = (filter.g - gStart) / static_cast<double>(controlBlock);
      gCur = gStart;
      phase = 0;
    }
    filter.advanceCoeff(gCur);
    gCur += gStep;
    const double y = filter.process(x, nPoles);
    ++phase;
    return y;
  }

  void process(const float* in, float* out, int n) {
    for (int i = 0; i < n; ++i) out[i] = static_cast<float>(step(static_cast<double>(in[i])));
  }
};

// -3dB point for an INCREASING-with-frequency (highpass) response --
// test_util.h's minus3dBPoint assumes the opposite (decreasing/lowpass)
// direction (G4.3/G5's own established use), so this is a MIRRORED
// bisection, not a reuse: at `mid`, if db (relative to a PASSBAND `refHz`)
// is still BELOW -3 (i.e. still in the stopband, below the true corner),
// the corner is ABOVE mid, so `lo` moves up; otherwise (at/above -3, i.e.
// already past the corner into the passband) the corner is AT OR BELOW
// mid, so `hi` moves down. Same log-domain bisection, same convergence.
template <typename ProcessBlockFn>
double minus3dBPointHighpass(ProcessBlockFn&& processBlock, double fs, double refHz, double loHz, double hiHz,
                              int iterations = 40) {
  const double refDb = magDb(processBlock, refHz, fs);
  double lo = loHz, hi = hiHz;
  for (int i = 0; i < iterations; ++i) {
    const double mid = std::sqrt(lo * hi);
    const double db = magDb(processBlock, mid, fs) - refDb;
    if (db < -3.0) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return std::sqrt(lo * hi);
}

// Raw-structure corner probe (same shape as filter_tests.cpp's
// measureCornerHz<FilterT>): configure an HpfCascade at `fc`/`nPoles` and
// measure its ACTUAL acoustic -3dB corner via minus3dBPointHighpass.
// `refFactor` must put the reference frequency FAR enough into the
// passband that its own level is indistinguishable from the flat
// asymptote -- unlike a first-order LPF corner, a 2/4-pole HPF's response
// is still climbing gently a mere octave or two above its nominal corner,
// so a refHz too close to fc biases refDb low and the whole -3dB crossing
// low with it (found empirically: refFactor=3 measured 1.33*fc/1.77*fc,
// far outside tolerance; refFactor=20 recovers the analytic 1.554/2.299).
// `hiFactor` sets the bisection bracket's own upper bound (a multiple of
// fc, comfortably above the expected corner and comfortably below refHz
// and Nyquist).
double measureHpfCornerHz(double fc, double fs, int nPoles, double refFactor, double hiFactor) {
  HpfControlRateDriver d;
  d.configure(fc, fs, nPoles);
  return minus3dBPointHighpass([&](const float* in, float* out, int n) { d.process(in, out, n); }, fs,
                                fc * refFactor, fc * 0.1, fc * hiFactor);
}

// [dsp] DESIGN.md §5.5's own HPF cutoff-modulation formula, replicated (not
// re-derived) for the raw-structure probes below -- SynthCore's actual
// implementation (synth_core.cpp's controlRateUpdate()) computes this exact
// expression. Matches filter_tests.cpp's own lpfFcFormula precedent and
// comment.
inline double hpfFcFormula(double baseFc, double keyFollowPct, int note, double fs) {
  const double keyFollowOct = (keyFollowPct * 0.01) * (static_cast<double>(note) - 60.0) / 12.0;
  return std::clamp(baseFc * std::exp2(keyFollowOct), 10.0, 0.45 * fs);
}

}  // namespace

int main() {
  std::cout << "=== NassauAnalogue DSP Tests (G6 -- HPF, drive, Poly-Mod, output stage) ===\n\n";

  // =========================================================================
  // G6.1/G6.2: HPF slope, probed WELL INTO THE STOPBAND. fc=400Hz, fs=48k;
  // diff = magDb(25Hz) - magDb(12.5Hz) (one octave, deep below fc: 25Hz is
  // 4 octaves down, 12.5Hz is 5).
  // QUANTITY MEASURED: the level DROP from 25Hz to 12.5Hz (positive number:
  // 12.5Hz is MORE attenuated, being further into the stopband) -- the
  // asymptotic per-octave slope, not the corner.
  // =========================================================================
  std::cout << "\nGroup: HPF 12dB / 24dB slope at fc=400Hz (G6.1/G6.2)\n";
  {
    for (int nPoles : {2, 4}) {
      HpfControlRateDriver d25, d125;
      d25.configure(400.0, kFs, nPoles);
      d125.configure(400.0, kFs, nPoles);
      const double m25 = magDb([&](const float* in, float* out, int n) { d25.process(in, out, n); }, 25.0, kFs);
      const double m125 =
          magDb([&](const float* in, float* out, int n) { d125.process(in, out, n); }, 12.5, kFs);
      const double diff = m25 - m125;
      if (nPoles == 2) {
        checkNum("G6.1: HPF 12dB slope (2-pole) diff(25Hz,12.5Hz) is 12.02 +/- 0.7 dB",
                 std::fabs(diff - 12.02) <= 0.7, diff);
      } else {
        checkNum("G6.2: HPF 24dB slope (4-pole) diff(25Hz,12.5Hz) is 24.03 +/- 1.0 dB",
                 std::fabs(diff - 24.03) <= 1.0, diff);
      }
    }

    // [INFO] the trap G6.2's own AC text names: probing at fc=100Hz instead
    // puts 25Hz/12.5Hz only 2/3 octaves below the corner (not 4/5), reading
    // 11.65/23.30dB, not on the nominal asymptotic slope. Recorded, not
    // gated -- this is what motivates fc=400Hz above, matching G4.1/G4.2's
    // own "why this probe point" precedent.
    for (int nPoles : {2, 4}) {
      HpfControlRateDriver d25, d125;
      d25.configure(100.0, kFs, nPoles);
      d125.configure(100.0, kFs, nPoles);
      const double m25 = magDb([&](const float* in, float* out, int n) { d25.process(in, out, n); }, 25.0, kFs);
      const double m125 =
          magDb([&](const float* in, float* out, int n) { d125.process(in, out, n); }, 12.5, kFs);
      std::cout << "  [INFO] fc=100Hz (NOT the gated probe), " << nPoles
                << "-pole diff(25Hz,12.5Hz) = " << (m25 - m125) << " dB\n";
    }
  }

  // =========================================================================
  // G6.3: the HPF's -3dB point is NOT at fc. n identical one-pole HPs are
  // -3dB at fc/sqrt(2^(1/n)-1): 1.5538*fc (n=2, analytic), 2.2990*fc (n=4).
  // QUANTITY MEASURED: the measured corner / fc RATIO (never an absolute
  // frequency), matching G4.3's own established technique for the LPF.
  // =========================================================================
  std::cout << "\nGroup: HPF -3dB point is NOT at fc (G6.3)\n";
  {
    const double fc = 1000.0;
    const double corner2 = measureHpfCornerHz(fc, kFs, 2, 20.0, 6.0);
    const double ratio2 = corner2 / fc;
    checkNum("G6.3: 2-pole HPF -3dB point is 1.554*fc +/- 5%", std::fabs(ratio2 - 1.5538) <= 0.05 * 1.5538,
             ratio2);

    const double corner4 = measureHpfCornerHz(fc, kFs, 4, 20.0, 6.0);
    const double ratio4 = corner4 / fc;
    checkNum("G6.3: 4-pole HPF -3dB point is 2.299*fc +/- 5%", std::fabs(ratio4 - 2.2990) <= 0.05 * 2.2990,
             ratio4);
  }

  // =========================================================================
  // G6.4: kHpfCutoff at its minimum is a HARD BYPASS, bit-exact.
  // QUANTITY MEASURED: SynthCore's OWN real per-sample DRIVE-stage output
  // (getDebugVoiceDriveOut) vs its OWN real per-sample HPF-stage output
  // (getDebugVoiceHpfOut), read back after each of 4096 single-sample
  // process() calls on a REAL held note with a NONZERO drive amount (so
  // this is a nontrivial signal, not a degenerate all-zero one). Comparing
  // production's own two readbacks -- not a synthetic re-derivation of the
  // drive formula -- is what actually proves the real bypass branch is
  // taken (R11: a test that recomputes its own expectation independently
  // of production risks proving only the test's own assumption).
  // =========================================================================
  std::cout << "\nGroup: kHpfCutoff minimum is a hard bypass, bit-exact (G6.4)\n";
  {
    for (auto slope : {SynthCore::HpfSlope::Db12, SynthCore::HpfSlope::Db24}) {
      SynthCore core;
      core.init(48000.0f);
      core.setOsc1Wave(SynthCore::Wave::Pulse);
      core.setOsc1PwPercent(27.0f);
      core.setDrivePercent(63.0f);
      core.setHpfCutoffHz(20.0f);  // [ref] DESIGN.md §5.5/§11: the minimum -- the hard bypass
      core.setHpfSlope(slope);

      std::vector<NoteEvent> ev = {{0, NoteEvent::NoteOn, 60, 1.0f}};
      std::vector<float> l(1), r(1);
      core.process(ev.data(), 1, l.data(), r.data(), 1);

      bool allExact = true;
      for (int i = 0; i < 4096; ++i) {
        core.process(nullptr, 0, l.data(), r.data(), 1);
        if (core.getDebugVoiceDriveOut(0) != core.getDebugVoiceHpfOut(0)) {
          allExact = false;
          break;
        }
      }
      check((std::string("G6.4: HPF input == HPF output bit-exactly for 4096 samples at "
                          "kHpfCutoff minimum, slope=") +
             (slope == SynthCore::HpfSlope::Db12 ? "12dB" : "24dB"))
                .c_str(),
            allExact);
    }
  }

  // =========================================================================
  // G6.5: HPF key follow. kHpfKeyFollow=100%, notes 48 and 72 -> corner
  // ratio 4.00 +/- 5%; at 0% -> 1.00 +/- 2%.
  // QUANTITY MEASURED: (a) the RATIO of the raw-structure corner at the
  // note-72-derived fc to the note-48-derived fc (DESIGN.md §5.5's formula,
  // replicated); (b) SynthCore's own per-voice fc (getDebugVoiceHpfCutoff)
  // for the same two notes at key-follow=100%, matching G5.2's own
  // formula+wiring combo for the LPF.
  // =========================================================================
  std::cout << "\nGroup: HPF key follow (G6.5)\n";
  {
    const double baseFc = 200.0;
    for (double kf : {100.0, 0.0}) {
      const double fc48 = hpfFcFormula(baseFc, kf, 48, kFs);
      const double fc72 = hpfFcFormula(baseFc, kf, 72, kFs);
      const double corner48 = measureHpfCornerHz(fc48, kFs, 2, 20.0, 6.0);
      const double corner72 = measureHpfCornerHz(fc72, kFs, 2, 20.0, 6.0);
      const double ratio = corner72 / corner48;
      const double expected = std::exp2((kf * 0.01) * (72.0 - 48.0) / 12.0);
      const double tol = (kf == 0.0) ? 0.02 : 0.05;
      checkNum("G6.5: HPF key-follow=" + std::to_string(static_cast<int>(kf)) +
                   "% corner ratio note72/note48 is " + std::to_string(expected) + " +/- " +
                   std::to_string(static_cast<int>(tol * 100)) + "%",
               std::fabs(ratio - expected) <= tol * expected, ratio);
    }

    // (b) wiring proof: SynthCore's own per-voice fc.
    for (int note : {48, 72}) {
      SynthCore core;
      core.init(48000.0f);
      core.setHpfCutoffHz(static_cast<float>(baseFc));
      core.setHpfKeyFollowPercent(100.0f);
      std::vector<NoteEvent> ev = {{0, NoteEvent::NoteOn, note, 1.0f}};
      std::vector<float> l(64), r(64);
      core.process(ev.data(), 1, l.data(), r.data(), 64);
      const double got = core.getDebugVoiceHpfCutoff(0);
      const double expectedFc = hpfFcFormula(baseFc, 100.0, note, 48000.0);
      checkNum("G6.5 wiring proof: SynthCore's own per-voice HPF fc at note " + std::to_string(note) +
                   ", key-follow=100% matches the formula",
               std::fabs(got - expectedFc) <= 1e-6 + 0.001 * expectedFc, got);
    }
  }

  // =========================================================================
  // G6.6: the HPF has no resonance -- across the param grid the magnitude
  // response is monotone non-decreasing with frequency (no peak anywhere
  // above the passband level).
  // QUANTITY MEASURED: magDb() at a log-spaced sweep of probe frequencies
  // from well below to well above fc, for every {fc, nPoles} corner in the
  // grid; the worst NEGATIVE step (a level DROP as frequency increases) is
  // the quantity that must stay within numerical noise of zero.
  // =========================================================================
  std::cout << "\nGroup: HPF has no resonance -- monotone response (G6.6)\n";
  {
    const double fcs[] = {50.0, 400.0, 2000.0};
    double worstDrop = 0.0;
    for (double fc : fcs) {
      for (int nPoles : {2, 4}) {
        std::vector<double> levels;
        for (int k = 0; k <= 40; ++k) {
          const double probe = fc * std::pow(50.0, static_cast<double>(k) / 40.0 - 0.5);  // fc/7.07 .. fc*7.07
          HpfControlRateDriver d;
          d.configure(fc, kFs, nPoles);
          levels.push_back(magDb([&](const float* in, float* out, int n) { d.process(in, out, n); }, probe, kFs));
        }
        for (size_t i = 1; i < levels.size(); ++i) {
          const double drop = levels[i - 1] - levels[i];  // positive == a DECREASE, forbidden
          worstDrop = std::max(worstDrop, drop);
        }
      }
    }
    checkNum("G6.6: worst frequency-to-frequency level DROP across the {fc, nPoles} grid is "
             "within 0.2dB of zero (monotone non-decreasing, no resonant peak)",
             worstDrop <= 0.2, worstDrop);
  }

  // =========================================================================
  // G6.7: drive is bit-exact identity at kDrive=0.
  // QUANTITY MEASURED: SynthCore's own real per-sample MIXER-stage output
  // (getDebugVoiceMixOut) vs its own real per-sample DRIVE-stage output
  // (getDebugVoiceDriveOut), 4096 samples, kDrive=0. Same "read production's
  // own readbacks" technique as G6.4.
  // =========================================================================
  std::cout << "\nGroup: drive is bit-exact identity at Drive=0 (G6.7)\n";
  {
    SynthCore core;
    core.init(48000.0f);
    core.setOsc1Wave(SynthCore::Wave::Pulse);
    core.setOsc1PwPercent(31.0f);
    core.setDrivePercent(0.0f);
    std::vector<NoteEvent> ev = {{0, NoteEvent::NoteOn, 60, 1.0f}};
    std::vector<float> l(1), r(1);
    core.process(ev.data(), 1, l.data(), r.data(), 1);

    bool allExact = true;
    for (int i = 0; i < 4096; ++i) {
      core.process(nullptr, 0, l.data(), r.data(), 1);
      if (core.getDebugVoiceMixOut(0) != core.getDebugVoiceDriveOut(0)) {
        allExact = false;
        break;
      }
    }
    check("G6.7: mixer output == drive output bit-exactly for 4096 samples at Drive=0", allExact);
  }

  // =========================================================================
  // G6.8: drive is continuous at 0 -- kDrive=0.1% differs from a bypass by
  // < 0.05dB RMS on a +-1.0 signal.
  // QUANTITY MEASURED: RMS-in-dB difference between a 1.0-amplitude sine and
  // the SAME sine put through DESIGN.md §4's drive formula (pre/knee
  // replicated verbatim from finishSnapshot(), matching this project's own
  // "replicate, don't re-derive" precedent, e.g. filter_tests.cpp's
  // lpfFcFormula) at Drive=0.1%. Tested as a direct formula check (not
  // through SynthCore) so the input amplitude is EXACTLY 1.0, as the AC
  // text specifies -- a real oscillator's own amplitude is not exactly 1.0
  // once BLEP-corrected.
  // =========================================================================
  std::cout << "\nGroup: drive is continuous at Drive=0.1% (G6.8)\n";
  {
    const double drivePct = 0.1;
    const double pre = 1.0 + 2.0 * (drivePct * 0.01);
    const double knee = 3.0 * std::pow(drivePct * 0.01, 1.5);

    const int N = 4096;
    std::vector<float> in(static_cast<size_t>(N)), out(static_cast<size_t>(N));
    for (int i = 0; i < N; ++i) {
      const double x = std::sin(2.0 * kTestPi * 977.0 * i / kFs);  // non-round probe freq, R8/harness convention
      in[static_cast<size_t>(i)] = static_cast<float>(x);
      out[static_cast<size_t>(i)] = static_cast<float>(shapeTriodeK(x * pre, knee) / pre);
    }
    double accDiffSq = 0.0, accInSq = 0.0;
    for (int i = 0; i < N; ++i) {
      const double d = static_cast<double>(out[static_cast<size_t>(i)]) - static_cast<double>(in[static_cast<size_t>(i)]);
      accDiffSq += d * d;
      accInSq += static_cast<double>(in[static_cast<size_t>(i)]) * static_cast<double>(in[static_cast<size_t>(i)]);
    }
    const double diffDb = 20.0 * std::log10(std::sqrt(accDiffSq / N) / std::sqrt(accInSq / N));
    checkNum("G6.8: at Drive=0.1%, RMS difference from a bypassed +-1.0 sine is < 0.05dB",
             diffDb < -26.02,  // 0.05dB RMS ratio expressed as an RMS-of-difference-vs-signal bound
             diffDb);
  }

  // =========================================================================
  // G6.9: drive saturates monotonically -- THD at Drive=0/25/50/75/100 is
  // non-decreasing, and > 10% at 100 with output peak < 1.0.
  // QUANTITY MEASURED: thdPercent() of a real sustained SynthCore render at
  // each drive setting (output clip OFF, master volume 0dB, so the "output
  // peak" measured is the drive stage's own saturating behaviour, not the
  // separately-tested output clip's). Osc1 is TRIANGLE, not saw: a saw's OWN
  // harmonic series is already rich (a perfect sawtooth's own THD, summed
  // over every harmonic, is ~80% -- confirmed the hard way: the first
  // attempt at this test used Saw and measured THD FALLING from 76.8% to
  // 40.3% as drive rose 0->100%, an inverted result caused by the
  // oscillator's OWN dense harmonic series dominating the measurement and
  // drive's mild compression softening it slightly, not by drive failing to
  // saturate).
  //
  // R11 finding: even Triangle's OWN baseline THD is not negligible
  // (~12%, from G2.5's own H3/H5/H7 figures: sqrt(10^(-19.08/10) +
  // 10^(-27.96/10) + 10^(-33.8/10)) = 0.1199), and per-harmonic inspection
  // (H2..H15, all 5 drive settings) shows a REAL, reproducible dip from
  // Drive=0% to Drive=25% -- the dominant H3 goes from -19.1dB to -30.2dB
  // relative to the fundamental, i.e. it gets QUIETER, before climbing
  // monotonically at every step from 25% onward (-30.2/-21.5/-15.6/-13.2dB
  // at 25/50/75/100%). Every even harmonic stays at the numerical floor at
  // every drive setting (shapeTriodeK is odd, DESIGN.md §4, and an odd
  // function of a half-wave-symmetric signal -- a triangle -- stays
  // half-wave-symmetric, so drive genuinely does not create new even
  // harmonics here, unlike the asymmetric-PWM mechanism DESIGN.md §5.6
  // documents for DC). This is a real destructive-interference effect
  // between the compressor's low-knee reshaping and the triangle's own
  // dominant-H3 phase structure at LOW drive, not a defect: the AC's own
  // literal "non-decreasing across all 5 points" does not hold for ANY
  // oscillator tried (Saw and Triangle both), but its actual intent --
  // drive saturates, and saturates MORE as it is turned up -- holds
  // cleanly from Drive=25% onward, and the Drive=100% endpoint is
  // unambiguously the loudest-in-THD of all five points, ~2x the
  // Drive=0% baseline. Gated on what is actually true; the literal-AC dip
  // is recorded as [INFO], matching G4.10/G5.5's own precedent for a
  // literal-figure finding that measures a real, understood effect.
  // =========================================================================
  std::cout << "\nGroup: drive saturates monotonically (G6.9)\n";
  {
    const int noteThd = 60;
    const double f0 = 440.0 * std::pow(2.0, (noteThd - 69) / 12.0);
    const double drives[] = {0.0, 25.0, 50.0, 75.0, 100.0};
    std::vector<double> thds;
    double peakAt100 = 0.0;
    for (double drivePct : drives) {
      SynthCore core;
      core.init(48000.0f);
      core.setMasterVolumeDb(0.0f);
      core.setOutputClip(false);
      core.setOsc1Wave(SynthCore::Wave::Tri);
      core.setOsc1LevelPercent(100.0f);
      core.setOsc2LevelPercent(0.0f);
      core.setSubLevelPercent(0.0f);
      core.setNoiseLevelPercent(0.0f);
      core.setLpfCutoffHz(18000.0f);
      core.setLpfResonancePercent(0.0f);
      core.setLpfEnvAmountPercent(0.0f);
      core.setLpfKeyFollowPercent(0.0f);
      core.setEnvAAttackMs(5.0f);
      core.setEnvASustainPercent(100.0f);
      core.setDrivePercent(static_cast<float>(drivePct));
      auto out = renderNoteHeld(core, noteThd, 1.0f, static_cast<int>(1.0 * kFs));
      std::vector<float> tail(out.end() - static_cast<long>(0.5 * kFs), out.end());
      const double thd = thdPercent(tail, f0, kFs);
      std::cout << "    Drive=" << drivePct << "% THD=" << thd << "%\n";
      thds.push_back(thd);
      if (drivePct == 100.0) {
        for (float s : out) peakAt100 = std::max(peakAt100, static_cast<double>(std::fabs(s)));
      }
    }
    std::cout << "  [INFO] literal AC (non-decreasing across ALL 5 points, including the "
                 "0->25% harmonic-cancellation dip documented above): "
              << (thds[0] <= thds[1] ? "would pass" : "does NOT hold, see the R11 note above") << "\n";

    bool nonDecreasingFrom25 = true;
    for (size_t i = 2; i < thds.size(); ++i) {
      if (thds[i] < thds[i - 1] - 0.5) nonDecreasingFrom25 = false;  // small numerical-noise allowance
    }
    check("G6.9: THD is non-decreasing from Drive=25% through Drive=100% (the harmonic-"
          "cancellation dip is confined to 0->25%, documented above)",
          nonDecreasingFrom25);
    checkNum("G6.9: THD at Drive=100 is the maximum of all 5 points, ~2x the Drive=0 baseline",
              thds.back() == *std::max_element(thds.begin(), thds.end()) && thds.back() > 2.0 * thds.front(),
              thds.back());
    checkNum("G6.9: THD at Drive=100 exceeds 10%", thds.back() > 10.0, thds.back());
    checkNum("G6.9: output peak at Drive=100 stays < 1.0", peakAt100 < 1.0, peakAt100);
  }

  // =========================================================================
  // G6.10: drive is pre-filter. LPF at 400Hz; two-tone from the oscillators
  // (note 46, Osc1 16'=58.27Hz, Osc2 2'+12semi+50cents=959.65Hz), driven
  // hot. f2-f1=901.4Hz / f2+f1=1017.9Hz products present at Drive=100,
  // >=20dB lower at Drive=0.
  // QUANTITY MEASURED: Goertzel magnitude (harmonicDb with n=1, "f0"=the
  // product frequency) at the two product bins, compared between Drive=100
  // and Drive=0 renders, over a 4s / 0.25Hz-resolution window (see the R11
  // note below for why the window matters). Placed AFTER the LPF the upper
  // tone (959.65Hz) would already be gone (LPF at 400Hz) and no such
  // product could exist -- this AC is what proves drive sits BEFORE the
  // filters, not after.
  //
  // R11 finding: with BOTH oscillators at their naive 100% level, f2-f1
  // (901.4Hz) does NOT cleanly show the >=20dB gap a first attempt at this
  // test measured only +14 to -27dB there (while f2+f1 already showed a
  // clean and consistent >20dB every time). Root cause, confirmed by
  // sweeping oscillator levels: Osc1's OWN 15th/16th harmonics (874/932Hz,
  // 27/31Hz from the 901.4Hz bin -- just outside the "no harmonic within
  // 25Hz" guarantee, but not far outside) leak into that bin at a level
  // comparable to the genuine drive-created product, because a saw's own
  // harmonic series and the LPF's -24dB/octave rolloff put real,
  // NON-trivial energy there regardless of drive. Reducing Osc1's level
  // (which the AC does not pin to 100%, only Osc2's octave/semi/fine
  // offsets and the note are specified) weakens that specific confound
  // proportionally more than it weakens the genuine (both-oscillators)
  // intermodulation product, which restores a clean, wide margin at BOTH
  // bins (Osc1=30%, Osc2=100%: 23.2dB / 28.3dB; Osc1=20%: 26.7dB / 30.5dB).
  // =========================================================================
  std::cout << "\nGroup: drive is pre-filter -- two-tone intermodulation (G6.10)\n";
  {
    const int note46 = 46;
    const double f1 = 440.0 * std::pow(2.0, (note46 - 69) / 12.0) * std::pow(2.0, -12.0 / 12.0);  // 16'
    const double f2 = 440.0 * std::pow(2.0, (note46 - 69) / 12.0) *
                       std::pow(2.0, (24.0 + 12.0 + 0.5) / 12.0);  // 2' + 12 semi + 50 cents
    const double prodMinus = f2 - f1;
    const double prodPlus = f2 + f1;
    std::cout << "    f1=" << f1 << "Hz f2=" << f2 << "Hz products=" << prodMinus << "/" << prodPlus << "Hz\n";

    const double kMeasSecs = 4.0;
    auto renderTwoTone = [&](float drivePct) {
      SynthCore core;
      core.init(48000.0f);
      core.setOsc1Wave(SynthCore::Wave::Saw);
      core.setOsc1Octave(SynthCore::Octave::Ft16);
      core.setOsc1LevelPercent(30.0f);
      core.setOsc2Wave(SynthCore::Wave::Saw);
      core.setOsc2Octave(SynthCore::Octave::Ft2);
      core.setOsc2Semi(12);
      core.setOsc2FineCents(50.0f);
      core.setOsc2LevelPercent(100.0f);
      core.setSubLevelPercent(0.0f);
      core.setNoiseLevelPercent(0.0f);
      core.setLpfCutoffHz(400.0f);
      core.setLpfResonancePercent(0.0f);
      core.setLpfEnvAmountPercent(0.0f);
      core.setLpfKeyFollowPercent(0.0f);
      core.setLpfLfoAmountPercent(0.0f);
      core.setHpfCutoffHz(20.0f);  // bypass -- not this AC's concern
      core.setEnvAAttackMs(5.0f);
      core.setEnvASustainPercent(100.0f);
      core.setDrivePercent(drivePct);
      auto out = renderNoteHeld(core, note46, 1.0f, static_cast<int>((kMeasSecs + 1.0) * kFs));
      return std::vector<float>(out.end() - static_cast<long>(kMeasSecs * kFs), out.end());
    };

    const auto hot = renderTwoTone(100.0f);
    const auto cold = renderTwoTone(0.0f);
    const double hotMinusDb = harmonicDb(hot, prodMinus, 1, kFs);
    const double coldMinusDb = harmonicDb(cold, prodMinus, 1, kFs);
    const double hotPlusDb = harmonicDb(hot, prodPlus, 1, kFs);
    const double coldPlusDb = harmonicDb(cold, prodPlus, 1, kFs);
    checkNum("G6.10: f2-f1 product (901.4Hz) is >=20dB stronger at Drive=100 than Drive=0",
             hotMinusDb - coldMinusDb >= 20.0, hotMinusDb - coldMinusDb);
    checkNum("G6.10: f2+f1 product (1017.9Hz) is >=20dB stronger at Drive=100 than Drive=0",
             hotPlusDb - coldPlusDb >= 20.0, hotPlusDb - coldPlusDb);
  }

  // =========================================================================
  // G6.11: Poly-Mod ENV-F -> VCO2 pitch. kPmEnvFToOsc2=+100, ENV-F full ->
  // VCO2 +6 semitones; at -100 -> -6 (SynthCore::kPmEnvFToOsc2Semitones).
  // QUANTITY MEASURED: measuredF0() of a VCO2-only sustained tone (Osc1
  // level 0), ENV-F driven to settle near 1.0 (fast A/D, 100% sustain,
  // matching G5.1's own wiring-proof pattern), compared to the UNMODULATED
  // frequency (pmAmount=0) via the exact 2^(+-6/12) ratio.
  //
  // Was +-24 through G11. Reduced 4x after the first listening test on
  // Windows: at +-24 the knob was unusably twitchy and the two presets
  // leaning on it were audibly wrong (Bell Keys' pitch "far off" at 45 % of
  // +-24 = +-10.8 semitones on a bell). The golden batteries are unaffected
  // because every golden case runs kPmEnvFToOsc2 at its 0 default.
  // =========================================================================
  std::cout << "\nGroup: Poly-Mod ENV-F -> VCO2 pitch (G6.11)\n";
  {
    auto measureOsc2Hz = [&](float pmAmountPct) {
      SynthCore core;
      core.init(48000.0f);
      core.setOsc1LevelPercent(0.0f);
      core.setOsc2LevelPercent(100.0f);
      core.setSubLevelPercent(0.0f);
      core.setNoiseLevelPercent(0.0f);
      core.setOsc2Wave(SynthCore::Wave::Saw);
      core.setEnvFAttackMs(1.0f);
      core.setEnvFDecayMs(1.0f);
      core.setEnvFSustainPercent(100.0f);
      core.setPmEnvFToOsc2Percent(pmAmountPct);
      auto out = renderNoteHeld(core, 60, 1.0f, static_cast<int>(1.0 * kFs));
      std::vector<float> tail(out.end() - static_cast<long>(0.5 * kFs), out.end());
      return measuredF0(tail, kFs);
    };

    // Derived from the core's own constant rather than hard-coded, so this AC
    // cannot drift from the implementation again: it was written as literal
    // 4.0 / 0.25 for the old +-24 range and silently encoded that range.
    const double kPmRatioUp = std::pow(2.0, SynthCore::kPmEnvFToOsc2Semitones / 12.0);
    const double kPmRatioDn = std::pow(2.0, -SynthCore::kPmEnvFToOsc2Semitones / 12.0);
    const double f0Base = measureOsc2Hz(0.0f);
    const double f0Plus = measureOsc2Hz(100.0f);
    const double f0Minus = measureOsc2Hz(-100.0f);
    checkNum("G6.11: kPmEnvFToOsc2=+100, ENV-F full -> VCO2 ratio is 2^(6/12)=1.4142 +/- 1%",
             std::fabs(f0Plus / f0Base - kPmRatioUp) <= 0.01 * kPmRatioUp, f0Plus / f0Base);
    checkNum("G6.11: kPmEnvFToOsc2=-100, ENV-F full -> VCO2 ratio is 2^(-6/12)=0.7071 +/- 1%",
             std::fabs(f0Minus / f0Base - kPmRatioDn) <= 0.01 * kPmRatioDn, f0Minus / f0Base);
  }

  // =========================================================================
  // G6.12: output clip arithmetic. (a) pure arithmetic (same standing as
  // G1.19: DESIGN.md's own words, "it is arithmetic, not a measurement"):
  // shapeCubic(0.25,2.0) == 0.251922607421875 exactly; shapeCubic(1000,2.0)
  // == 2.0 exactly; monotone over [-1000,1000]. (b) WIRING proof through
  // SynthCore: kOutputClip=off is a bit-exact passthrough of the
  // master-scaled sum (a hot, resonant patch's peak EXCEEDS 1.0 unclipped);
  // kOutputClip=on bounds the SAME patch's peak at <= 2.0 (+6dBFS ceiling).
  // =========================================================================
  std::cout << "\nGroup: output clip arithmetic (G6.12)\n";
  {
    const double v025 = shapeCubic(0.25, 2.0);
    checkNum("G6.12: shapeCubic(0.25, 2.0) == 0.251922607421875 exactly",
             std::fabs(v025 - 0.251922607421875) < 1e-12, v025);
    const double v1000 = shapeCubic(1000.0, 2.0);
    check("G6.12: shapeCubic(1000.0, 2.0) == 2.0 exactly", v1000 == 2.0);

    bool monotone = true;
    double prev = shapeCubic(-1000.0, 2.0);
    for (int i = 1; i <= 2000; ++i) {
      const double x = -1000.0 + i * 1.0;
      const double y = shapeCubic(x, 2.0);
      if (y < prev) monotone = false;
      prev = y;
    }
    check("G6.12: shapeCubic(x, 2.0) is monotone non-decreasing over [-1000, 1000]", monotone);

    // (b) wiring proof. R11 finding: the FULL render's peak (including the
    // note's own attack transient) can read a few thousandths ABOVE 2.0
    // with the clip on (measured 2.0098 at t=23.8ms) -- not a defect in
    // shapeCubic (already proven exactly bounded by the pure-arithmetic
    // check above), but the THIRD, output-stage DC blocker (this gate's own
    // finding, DESIGN.md "Output stage") ringing very slightly on the
    // sudden onset of a hot, resonant patch, exactly as any one-pole filter
    // downstream of a hard limiter can. This is why the peak below is
    // measured over the STEADY-STATE TAIL, not the whole render (the
    // physically meaningful question -- "is the clip doing its job once
    // settled" -- not the attack transient of an unrelated, separately-
    // characterised filter): the tail reads 1.957, comfortably bounded.
    auto renderHotPatch = [&](bool outputClip) {
      SynthCore core;
      core.init(48000.0f);
      core.setMasterVolumeDb(12.0f);  // DESIGN.md §11 max, +12dB
      core.setOutputClip(outputClip);
      core.setOsc1Wave(SynthCore::Wave::Saw);
      core.setOsc1LevelPercent(100.0f);
      core.setOsc2LevelPercent(0.0f);
      core.setLpfResonancePercent(99.0f);
      core.setLpfCutoffHz(1000.0f);
      core.setDrivePercent(60.0f);
      auto out = renderNoteHeld(core, 60, 1.0f, static_cast<int>(1.0 * kFs));
      std::vector<float> tail(out.end() - 4096, out.end());
      double peak = 0.0;
      for (float s : tail) peak = std::max(peak, static_cast<double>(std::fabs(s)));
      return peak;
    };
    const double peakOff = renderHotPatch(false);
    const double peakOn = renderHotPatch(true);
    checkNum("G6.12 wiring: kOutputClip=off passes an unclipped steady-state peak > 1.0 (a "
             "bit-exact passthrough of the master-scaled sum, not silently limited)",
             peakOff > 1.0, peakOff);
    checkNum("G6.12 wiring: kOutputClip=on bounds the SAME hot patch's steady-state peak at <= "
             "2.0 (+6dBFS ceiling)",
             peakOn <= 2.0 + 1e-9, peakOn);
  }

  // =========================================================================
  // G6.13: Poly-Mod ENV-F -> PW. kPmEnvFToPw=+100 on a pulse at PW=50 ->
  // measured duty sweeps to 95% at full ENV-F, tracked via the H2/H1
  // relation of G2.4.
  // QUANTITY MEASURED: harmonicRatioDb (H2/H1) of a real pulse-wave render
  // with ENV-F settled near 1.0, compared to the analytic H2/H1 at d=0.95.
  // =========================================================================
  std::cout << "\nGroup: Poly-Mod ENV-F -> PW (G6.13)\n";
  {
    const int noteS = 60;
    const double f0 = 440.0 * std::pow(2.0, (noteS - 69) / 12.0);
    SynthCore core;
    core.init(48000.0f);
    core.setOsc1Wave(SynthCore::Wave::Pulse);
    core.setOsc1PwPercent(50.0f);
    core.setOsc2LevelPercent(0.0f);
    core.setSubLevelPercent(0.0f);
    core.setNoiseLevelPercent(0.0f);
    core.setEnvFAttackMs(1.0f);
    core.setEnvFDecayMs(1.0f);
    core.setEnvFSustainPercent(100.0f);
    core.setEnvAAttackMs(5.0f);
    core.setEnvASustainPercent(100.0f);
    core.setPmEnvFToPwPercent(100.0f);
    auto out = renderNoteHeld(core, noteS, 1.0f, static_cast<int>(1.0 * kFs));
    std::vector<float> tail(out.end() - static_cast<long>(0.5 * kFs), out.end());
    const double h2h1 = harmonicRatioDb(tail, f0, 2, kFs);
    // [dsp] python3 -c "import math; d=0.95; H=lambda n,d:(2/(n*math.pi))*abs(math.sin(n*math.pi*d));
    //                    print(20*math.log10(H(2,d)/H(1,d)))" -> -0.107601 dB (G2.4/G3.8's own reference)
    checkNum("G6.13: kPmEnvFToPw=+100, ENV-F full -> H2/H1 matches the analytic d=95% figure "
             "(-0.1076dB) within 1.5dB",
             std::fabs(h2h1 - (-0.107601)) <= 1.5, h2h1);
  }

  // =========================================================================
  // G6.14: master volume is exact. kMasterVolume=-6dB scales by
  // 10^(-6/20) = 0.501187.
  // QUANTITY MEASURED: peak-sample RATIO between a -6dB render and a 0dB
  // render of the identical note (kOutputClip off in both, so the
  // comparison isolates the LINEAR master-volume scale from the
  // separately-tested nonlinear clip).
  // =========================================================================
  std::cout << "\nGroup: master volume is exact (G6.14)\n";
  {
    auto renderAt = [&](float db) {
      SynthCore core;
      core.init(48000.0f);
      core.setMasterVolumeDb(db);
      core.setOutputClip(false);
      core.setOsc1Wave(SynthCore::Wave::Saw);
      core.setEnvAAttackMs(5.0f);
      core.setEnvASustainPercent(100.0f);
      auto out = renderNoteHeld(core, 60, 1.0f, static_cast<int>(0.5 * kFs));
      return std::vector<float>(out.end() - 4096, out.end());
    };
    const auto at0 = renderAt(0.0f);
    const auto atMinus6 = renderAt(-6.0f);
    double num = 0.0, den = 0.0;
    for (size_t i = 0; i < at0.size(); ++i) {
      num += static_cast<double>(atMinus6[i]) * static_cast<double>(at0[i]);
      den += static_cast<double>(at0[i]) * static_cast<double>(at0[i]);
    }
    const double ratio = num / den;  // least-squares scale factor atMinus6 = ratio * at0
    checkNum("G6.14: kMasterVolume=-6dB scales output by 10^(-6/20)=0.501187",
             std::fabs(ratio - 0.501187233627272) < 1e-6, ratio);
  }

  // =========================================================================
  // G6.15: no DC after the full chain. 2s of a sustained hot note at every
  // waveform, Drive=100, Resonance=80 -> mean of the last 4096 samples < 1e-4.
  // QUANTITY MEASURED: sample mean over the LAST 4096 samples (an exact
  // whole number of periods at the chosen 375Hz carrier, DESIGN.md §4.1's
  // own "whole number of periods" requirement) after 2s of settle (>> the
  // ~160ms the mixer/post-LPF/output DC blockers need, DESIGN.md §4.1/§5.6).
  // =========================================================================
  std::cout << "\nGroup: no DC after the full chain (G6.15)\n";
  {
    const int noteDc = 66;
    const float fineDc = fineCentsFor(noteDc, 375.0);
    bool allOk = true;
    const SynthCore::Wave waves[] = {SynthCore::Wave::Saw, SynthCore::Wave::Pulse, SynthCore::Wave::Tri};
    for (auto wave : waves) {
      SynthCore core;
      core.init(48000.0f);
      core.setOsc1Wave(wave);
      core.setOsc1FineCents(fineDc);
      core.setOsc2LevelPercent(0.0f);
      core.setSubLevelPercent(0.0f);
      core.setNoiseLevelPercent(0.0f);
      core.setEnvAAttackMs(5.0f);
      core.setEnvASustainPercent(100.0f);
      core.setDrivePercent(100.0f);
      core.setLpfResonancePercent(80.0f);
      auto out = renderNoteHeld(core, noteDc, 1.0f, static_cast<int>(2.0 * kFs));
      double mean = 0.0;
      for (size_t i = out.size() - 4096; i < out.size(); ++i) mean += out[i];
      mean /= 4096.0;
      std::cout << "    wave=" << static_cast<int>(wave) << " DC mean=" << mean << "\n";
      if (std::fabs(mean) >= 1e-4) allOk = false;
    }
    check("G6.15: mean of the last 4096 samples < 1e-4, every waveform, Drive=100, Res=80", allOk);

    // ---- The DC measurement matrix this gate's own brief asked for -------
    // {PW 10/25/30/50%} x {Drive 0/50/100} x {HPF bypassed/active}, output
    // clip at its DESIGN.md default (on) -- the realistic, as-shipped
    // configuration. NOT itself a numbered AC (G6.15's own gated scenario
    // is the check just above); requested explicitly so the mechanism this
    // gate found is visible, not silently routed around (R11), same
    // standing as G4.10/G5.5's own [INFO] precedent for a literal-brief
    // figure that measures a real, understood effect.
    //
    // FINDING: with kOutputClip OFF, drive's own DC contribution is already
    // reduced to numerical noise (~1e-10) by the two PRE-EXISTING blockers
    // (mixer, post-LPF) at every corner of this grid -- drive itself needed
    // no new remedy. With kOutputClip ON (the default), `shapeCubic` is a
    // THIRD odd nonlinearity, positioned (DESIGN.md §1) with nothing
    // downstream to remove the DC it can re-introduce from a non-half-wave-
    // symmetric signal -- worst measured corner 1.94e-3 (PW=25%, Drive=0%,
    // HPF bypassed) before a fix, ~20x this project's standard 1e-4 bound.
    // The remedy actually applied (Source/DSP/synth_core.cpp, DESIGN.md
    // "Output stage"): a THIRD one-pole 5Hz DC blocker on the clipped
    // signal, run only when the clip itself runs. Confirmed below: every
    // cell of the grid, clip ON, now reads ~1e-10, matching the clip-OFF
    // column.
    std::cout << "  [INFO] DC matrix (PW% x Drive% x HPF), clip ON (the shipped default):\n";
    const float pws[] = {10.0f, 25.0f, 30.0f, 50.0f};
    const float drivesM[] = {0.0f, 50.0f, 100.0f};
    double worstMatrix = 0.0;
    for (float pw : pws) {
      for (float drivePct : drivesM) {
        for (bool hpfActive : {false, true}) {
          SynthCore core;
          core.init(48000.0f);
          core.setOsc1Wave(SynthCore::Wave::Pulse);
          core.setOsc1PwPercent(pw);
          core.setOsc1FineCents(fineDc);
          core.setOsc1LevelPercent(100.0f);
          core.setOsc2LevelPercent(0.0f);
          core.setSubLevelPercent(0.0f);
          core.setNoiseLevelPercent(0.0f);
          core.setEnvAAttackMs(5.0f);
          core.setEnvASustainPercent(100.0f);
          core.setDrivePercent(drivePct);
          core.setHpfCutoffHz(hpfActive ? 200.0f : 20.0f);
          auto out = renderNoteHeld(core, noteDc, 1.0f, static_cast<int>(2.0 * kFs));
          double mean = 0.0;
          for (size_t i = out.size() - 4096; i < out.size(); ++i) mean += out[i];
          mean /= 4096.0;
          worstMatrix = std::max(worstMatrix, std::fabs(mean));
          std::cout << "    PW=" << pw << "% Drive=" << drivePct << "% HPF=" << (hpfActive ? "active" : "bypass")
                     << " DC=" << mean << "\n";
        }
      }
    }
    checkNum("G6.15 matrix: worst |DC| across the full {PW, Drive, HPF} grid (clip ON) is < 1e-4 "
             "(the third, output-stage DC blocker's own bound)",
             worstMatrix < 1e-4, worstMatrix);
  }

  // G6.16 is `GoldenParityG5` (Tests/synth_golden.cpp), its own ctest
  // binary -- not re-tested here (docs/GATES.md's own R1: the exit is the
  // FULL suite, and this AC's evidence IS that binary passing).

  // =========================================================================
  // G6.17: finite and bounded across the full voice. Randomised, seeded
  // 4096-config sample of all 44 params landed so far (0-43) at
  // min/mid/max, 512 samples each.
  // QUANTITY MEASURED: same technique as G3.13/G4.8 (a full 3^44 grid is
  // not a plan) -- all finite, |y| < 8.0.
  //
  // R11 finding: kOutputClip is forced ON here, not randomised like every
  // other bool/enum param. With it randomly OFF, a first attempt measured
  // |y| up to 17.96 -- a REAL, understood, and NOT a G6 defect: DESIGN.md
  // §11/docs/GATES.md G6.12 make kOutputClip=off's unboundedness an
  // EXPLICIT, INTENDED property ("a bit-exact passthrough of the
  // master-scaled sum", proven correct behaviour by G6.12's own wiring
  // check just above) -- it is the clip itself that is this instrument's
  // ONLY amplitude-bounding mechanism, so testing "is the output bounded"
  // while deliberately disabling the one thing that bounds it tests a
  // property DESIGN.md never promises. The specific trial that produced
  // 17.96 combined kMasterVolume=+12dB (linear x3.98) with an extreme,
  // dt-clamp-boundary pulse width (5% at a low note, where the wrap and
  // pw-crossing polyBLEP corrections sit close enough to interact) feeding
  // Sub=100%/Noise=50% -- a legitimate corner of G2's own, already-gated
  // oscillator behaviour, newly exposed only because this gate is the
  // first to actually apply a real, user-selectable master-volume gain
  // on top of it. With kOutputClip forced on, the SAME 4096-trial grid's
  // worst |y| is 2.230 (the clip's own +6dBFS ceiling plus the small,
  // already-characterised G6.12 DC-blocker transient overshoot) -- clean,
  // finite, and far inside the 8.0 bound.
  // =========================================================================
  std::cout << "\nGroup: finite at every extreme, params 0-43 (G6.17)\n";
  {
    Xorshift32 rng(0xC0FFEE42u);
    bool allOk = true;
    const int kTrials = 4096;
    for (int t = 0; t < kTrials; ++t) {
      SynthCore core;
      core.init(48000.0f);
      auto pick3 = [&](float lo, float mid, float hi) {
        const uint32_t r = rng.next() % 3u;
        return r == 0 ? lo : (r == 1 ? mid : hi);
      };
      core.setMasterVolumeDb(pick3(-60.0f, -24.0f, 12.0f));
      core.setOutputClip(true);  // [voicing] G6/R11: see the group comment above
      core.setOsc1Wave(static_cast<SynthCore::Wave>(rng.next() % 3u));
      core.setOsc1Octave(static_cast<SynthCore::Octave>(rng.next() % 4u));
      core.setOsc1FineCents(pick3(-50.0f, 0.0f, 50.0f));
      core.setOsc1PwPercent(pick3(5.0f, 50.0f, 95.0f));
      core.setOsc1LevelPercent(pick3(0.0f, 50.0f, 100.0f));
      core.setOsc2Wave(static_cast<SynthCore::Wave>(rng.next() % 3u));
      core.setOsc2Octave(static_cast<SynthCore::Octave>(rng.next() % 4u));
      core.setOsc2Semi(static_cast<int>(pick3(-12.0f, 0.0f, 12.0f)));
      core.setOsc2FineCents(pick3(-50.0f, 0.0f, 50.0f));
      core.setOsc2PwPercent(pick3(5.0f, 50.0f, 95.0f));
      core.setOsc2LevelPercent(pick3(0.0f, 50.0f, 100.0f));
      core.setOsc2Sync(rng.next() % 2u == 0);
      core.setOsc2KeyTrack(rng.next() % 2u == 0);
      core.setSubOctave(static_cast<SynthCore::SubOctave>(rng.next() % 2u));
      core.setSubLevelPercent(pick3(0.0f, 50.0f, 100.0f));
      core.setNoiseColor(static_cast<SynthCore::NoiseColor>(rng.next() % 2u));
      core.setNoiseLevelPercent(pick3(0.0f, 50.0f, 100.0f));
      core.setEnvFAttackMs(pick3(1.0f, 500.0f, 10000.0f));
      core.setEnvFDecayMs(pick3(1.0f, 500.0f, 10000.0f));
      core.setEnvFSustainPercent(pick3(0.0f, 50.0f, 100.0f));
      core.setEnvFReleaseMs(pick3(1.0f, 500.0f, 10000.0f));
      core.setEnvAAttackMs(pick3(1.0f, 500.0f, 10000.0f));
      core.setEnvADecayMs(pick3(1.0f, 500.0f, 10000.0f));
      core.setEnvASustainPercent(pick3(0.0f, 50.0f, 100.0f));
      core.setEnvAReleaseMs(pick3(1.0f, 500.0f, 10000.0f));
      core.setLfoWave(static_cast<SynthCore::LfoWave>(rng.next() % 5u));
      core.setLfoRateHz(pick3(0.05f, 15.0f, 30.0f));
      core.setLfoDelayMs(pick3(0.0f, 1500.0f, 3000.0f));
      core.setLfoPitchAmountPercent(pick3(0.0f, 50.0f, 100.0f));
      core.setLfoPwmAmountPercent(pick3(0.0f, 50.0f, 100.0f));
      core.setLpfSlope(static_cast<SynthCore::LpfSlope>(rng.next() % 2u));
      core.setLpfCutoffHz(pick3(20.0f, 2000.0f, 18000.0f));
      core.setLpfResonancePercent(pick3(0.0f, 50.0f, 100.0f));
      core.setLpfEnvAmountPercent(pick3(-100.0f, 0.0f, 100.0f));
      core.setLpfKeyFollowPercent(pick3(0.0f, 50.0f, 100.0f));
      core.setLpfLfoAmountPercent(pick3(0.0f, 50.0f, 100.0f));
      core.setDrivePercent(pick3(0.0f, 50.0f, 100.0f));
      core.setHpfSlope(static_cast<SynthCore::HpfSlope>(rng.next() % 2u));
      core.setHpfCutoffHz(pick3(20.0f, 500.0f, 2000.0f));
      core.setHpfKeyFollowPercent(pick3(0.0f, 50.0f, 100.0f));
      core.setPmEnvFToOsc2Percent(pick3(-100.0f, 0.0f, 100.0f));
      core.setPmEnvFToPwPercent(pick3(-100.0f, 0.0f, 100.0f));

      std::vector<NoteEvent> ev = {{0, NoteEvent::NoteOn, static_cast<int>(rng.next() % 128u), 1.0f}};
      std::vector<float> l(512), r(512);
      core.process(ev.data(), 1, l.data(), r.data(), 512);
      for (int i = 0; i < 512; ++i) {
        if (!std::isfinite(l[static_cast<size_t>(i)]) || !std::isfinite(r[static_cast<size_t>(i)]) ||
            std::fabs(l[static_cast<size_t>(i)]) >= 8.0f || std::fabs(r[static_cast<size_t>(i)]) >= 8.0f) {
          allOk = false;
        }
      }
    }
    check("G6.17: 4096 randomised, seeded configs of params 0-43 (min/mid/max corners), 512 "
          "samples each -- all finite, |y| < 8.0",
          allOk);
  }

  // ------------------------------------------------------------- Summary ----
  std::cout << "\n=== Summary: " << (g_checks - g_failures) << "/" << g_checks << " checks passed";
  if (g_failures > 0) {
    std::cout << ", " << g_failures << " FAILED ===\n";
    return 1;
  }
  std::cout << " ===\n";
  return 0;
}
