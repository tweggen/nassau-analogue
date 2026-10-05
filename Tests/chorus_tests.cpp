// NassauAnalogue DSP Unit Tests — Gate G12 (Juno-style output-stage chorus,
// DESIGN.md §13). Hand-rolled harness (plain int main + soft checks, no
// external framework) -- the nassau-eq/nassau-zermatt house style
// (docs/GATES.md "Shared test harness"). Covers G12.1-G12.14.
//
// Two levels are exercised deliberately:
//   * JunoChorus (Source/DSP/synth_chorus.h) is header-only and is driven
//     DIRECTLY where the quantity under test is the block's own (the R12
//     source-text inspection), the same shape dsp_tests.cpp/osc_tests.cpp
//     use for their primitives;
//   * everything else drives the REAL SynthCore output stage (NoteEvent in,
//     rendered stereo audio out), because G12's actual job is the WIRING of
//     one shared chorus into the summed accumulator ahead of master volume,
//     and a header-only test cannot see that.
//
// R6: every group states, immediately above its check(), exactly what
// quantity is measured and why that is the quantity the AC asks for.
// R8: no rand()/time(); every AC here is deterministic by construction.

#include "synth_chorus.h"
#include "synth_core.h"
#include "test_util.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <new>
#include <string>
#include <vector>

// ---- R3/G0.11 precedent, re-armed for the chorus specifically -------------
// G0.11 proves SynthCore::process() allocates nothing -- but it was written
// before this stage existed, and the chorus is the one block in the tree
// that owns a kilobyte-scale buffer. A claim that "the delay lines are fixed
// and constructed once" deserves its own measurement rather than resting on
// a gate that could not have covered it. Same overload set as
// synth_tests.cpp/stereo_tests.cpp so no call site can bypass the counter.
static std::size_t gAllocCount = 0;

void* operator new(std::size_t n) {
  ++gAllocCount;
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new[](std::size_t n) {
  ++gAllocCount;
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

// ---- Soft check harness (runs all checks, tallies failures) ---------------
int g_checks = 0;
int g_failures = 0;

void check(const char* name, bool cond) {
  ++g_checks;
  std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name << "\n";
  if (!cond) ++g_failures;
}

void checkNum(const std::string& name, bool cond, double measured) {
  ++g_checks;
  std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name << " (measured: " << measured << ")\n";
  if (!cond) ++g_failures;
}

constexpr double kFs = 48000.0;
constexpr int kControlBlock = 32;  // DESIGN.md §2's default, mirrored here for the control-step maths

// ---- Render helpers (same shape as stereo_tests.cpp) ----------------------

void renderAbsEvents(SynthCore& core, std::vector<NoteEvent> absEvents, int totalSamples, int block,
                      std::vector<float>& outL, std::vector<float>& outR) {
  outL.assign(static_cast<size_t>(totalSamples), 0.0f);
  outR.assign(static_cast<size_t>(totalSamples), 0.0f);
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
    core.process(chunk.empty() ? nullptr : chunk.data(), static_cast<int>(chunk.size()),
                 outL.data() + pos, outR.data() + pos, n);
    pos += n;
  }
}

// A deliberately SMOOTH patch: one triangle oscillator, nothing else in the
// mixer, no drive, a low-passed sustained tone. Used by the click ACs --
// a saw's own wrap discontinuity is larger than any click a 20 ms fade could
// produce, so measuring "max sample-to-sample step" on a saw would measure
// the oscillator, not the chorus (R6: the quantity has to be the one the AC
// is about).
void configureSmooth(SynthCore& core) {
  core.init(static_cast<float>(kFs));
  core.setOsc1Wave(SynthCore::Wave::Tri);
  core.setOsc1LevelPercent(100.0f);
  core.setOsc2LevelPercent(0.0f);
  core.setSubLevelPercent(0.0f);
  core.setNoiseLevelPercent(0.0f);
  core.setDrivePercent(0.0f);
  core.setLpfCutoffHz(1200.0f);
  core.setLpfResonancePercent(0.0f);
  core.setLpfEnvAmountPercent(0.0f);
  core.setEnvAAttackMs(5.0f);
  core.setEnvADecayMs(10.0f);
  core.setEnvASustainPercent(100.0f);
  core.setOutputClip(false);  // the clip's own DC blocker is stateful; keep it out of bit-exact ACs
  core.setMasterVolumeDb(-12.0f);
}

double maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b, int from, int to) {
  double m = 0.0;
  for (int i = from; i < to; ++i) {
    m = std::max(m, std::fabs(static_cast<double>(a[static_cast<size_t>(i)]) -
                               static_cast<double>(b[static_cast<size_t>(i)])));
  }
  return m;
}

double maxAbs(const std::vector<float>& v) {
  double m = 0.0;
  for (float s : v) m = std::max(m, std::fabs(static_cast<double>(s)));
  return m;
}

// Largest sample-to-sample step within [from, to).
double maxStep(const std::vector<float>& v, int from, int to) {
  double m = 0.0;
  for (int i = from + 1; i < to; ++i) {
    m = std::max(m, std::fabs(static_cast<double>(v[static_cast<size_t>(i)]) -
                               static_cast<double>(v[static_cast<size_t>(i - 1)])));
  }
  return m;
}

// Pearson correlation of two channels over [from, to). 1.0 means the two are
// the same signal up to scale; this is the number a "does the chorus
// actually widen it?" AC is about.
double correlation(const std::vector<float>& a, const std::vector<float>& b, int from, int to) {
  double sa = 0.0, sb = 0.0;
  const int n = to - from;
  for (int i = from; i < to; ++i) {
    sa += a[static_cast<size_t>(i)];
    sb += b[static_cast<size_t>(i)];
  }
  const double ma = sa / n, mb = sb / n;
  double num = 0.0, da = 0.0, db = 0.0;
  for (int i = from; i < to; ++i) {
    const double x = a[static_cast<size_t>(i)] - ma;
    const double y = b[static_cast<size_t>(i)] - mb;
    num += x * y;
    da += x * x;
    db += y * y;
  }
  const double den = std::sqrt(da * db);
  return den > 0.0 ? num / den : 0.0;
}

// Runs `controlSteps` control blocks of SILENCE through the core (one
// 32-sample host block each, so every host block is exactly one control
// block) and records the chorus's published delay for both channels after
// each. Silence is deliberate: the chorus LFO advances from the control-rate
// grid, not from anything a voice does, so a note would only add noise to
// what these ACs measure.
void sweepDelays(SynthCore& core, int controlSteps, std::vector<double>& d0, std::vector<double>& d1) {
  d0.clear();
  d1.clear();
  d0.reserve(static_cast<size_t>(controlSteps));
  d1.reserve(static_cast<size_t>(controlSteps));
  std::vector<float> l(kControlBlock), r(kControlBlock);
  for (int i = 0; i < controlSteps; ++i) {
    core.process(nullptr, 0, l.data(), r.data(), kControlBlock);
    d0.push_back(core.getDebugChorusDelayMs(0));
    d1.push_back(core.getDebugChorusDelayMs(1));
  }
}

const char* modeName(int m) {
  switch (m) {
    case 1: return "I";
    case 2: return "II";
    case 3: return "I+II";
    default: return "Off";
  }
}

}  // namespace

int main() {
  std::cout << "=== NassauAnalogue DSP Tests (G12 -- Juno-style output-stage chorus) ===\n\n";

  // =========================================================================
  // G12.1: kChorus = Off is a BIT-EXACT passthrough, and the chorus returns
  // to that state after being switched off.
  //
  // QUANTITY MEASURED: max abs sample difference between (a) a render that
  // had the chorus Off throughout and (b) a render of the IDENTICAL note
  // that had it on II for the first 0.5 s and Off thereafter -- compared
  // only from 1.0 s onward, i.e. long after the 20 ms fade-out has
  // completed. Exactly 0.0 is the only passing value.
  //
  // This is the AC the whole gate's "adding a chorus changed nothing about
  // the instrument" claim rests on, and it is stronger than it looks: it
  // fails if the wet fade merely APPROACHES zero instead of landing on it,
  // if the passthrough is written as `x + 0.0*wet` rather than an
  // assignment, or if anything about the chorus leaks into the voice path.
  // (The other half of the claim -- that the pre-G12 sound is unchanged --
  // is carried by the two golden batteries, whose fixtures predate this
  // gate and still verify at exactly 0.000e+00.)
  //
  // kOutputClip is off for this comparison BY NECESSITY, not convenience:
  // the clip's own DC blocker is a stateful IIR that only runs when the clip
  // does, so the two renders would carry different filter memory for a
  // while after the toggle and the comparison would be measuring that
  // instead (DESIGN.md §11 "Output stage").
  // =========================================================================
  std::cout << "Group: chorus Off is a bit-exact passthrough (G12.1)\n";
  {
    const int total = static_cast<int>(2.0 * kFs);
    const int firstHalf = static_cast<int>(0.5 * kFs);
    const int compareFrom = static_cast<int>(1.0 * kFs);
    const std::vector<NoteEvent> ev = {{0, NoteEvent::NoteOn, 57, 0.9f}};

    // (a) Off throughout.
    SynthCore alwaysOff;
    configureSmooth(alwaysOff);
    std::vector<float> aL, aR;
    renderAbsEvents(alwaysOff, ev, total, 512, aL, aR);

    // (b) II for 0.5 s, then Off for the rest -- rendered through the SAME
    // helper in two contiguous segments so the only difference from (a) is
    // the parameter, not the blocking.
    SynthCore toggled;
    configureSmooth(toggled);
    toggled.setChorusMode(SynthCore::Chorus::Two);
    std::vector<float> bL(static_cast<size_t>(total), 0.0f), bR(static_cast<size_t>(total), 0.0f);
    {
      std::vector<float> segL, segR;
      renderAbsEvents(toggled, ev, firstHalf, 512, segL, segR);
      std::copy(segL.begin(), segL.end(), bL.begin());
      std::copy(segR.begin(), segR.end(), bR.begin());
      toggled.setChorusMode(SynthCore::Chorus::Off);
      renderAbsEvents(toggled, {}, total - firstHalf, 512, segL, segR);
      std::copy(segL.begin(), segL.end(), bL.begin() + firstHalf);
      std::copy(segR.begin(), segR.end(), bR.begin() + firstHalf);
    }

    const double diff = std::max(maxAbsDiff(aL, bL, compareFrom, total),
                                  maxAbsDiff(aR, bR, compareFrom, total));
    const double onDiff = std::max(maxAbsDiff(aL, bL, 0, firstHalf), maxAbsDiff(aR, bR, 0, firstHalf));

    checkNum("G12.1: after the chorus is switched off, output is BIT-IDENTICAL to a render that "
             "never had it on (max abs diff == 0 exactly)",
             diff == 0.0, diff);
    // The control: if the two renders were identical throughout, the check
    // above would pass for the wrong reason (a chorus that does nothing).
    checkNum("G12.1 control: while the chorus WAS on, the same two renders genuinely differ",
             onDiff > 1e-4, onDiff);
    check("G12.1: a core with kChorus = Off reports its audio path as not audible (the passthrough "
          "branch is the one being taken)",
          !alwaysOff.getDebugChorusAudible());
  }

  // =========================================================================
  // G12.2: mono L==R bit-identity (G8.1) survives the new stage, and the
  // chorus is what breaks it on purpose.
  //
  // QUANTITY MEASURED: max abs |outL - outR| over a full render with
  // kStereoMode off. With kChorus Off this must be EXACTLY 0 -- G8.1's
  // property, unchanged. With kChorus on it must be clearly non-zero: L and
  // R are the same dry signal plus/MINUS two antiphase-swept delay taps, so
  // decorrelating them is the entire point of the stage.
  // =========================================================================
  std::cout << "\nGroup: mono L==R bit-identity preserved, and broken only by the chorus (G12.2)\n";
  {
    const int total = static_cast<int>(1.0 * kFs);
    const std::vector<NoteEvent> ev = {{0, NoteEvent::NoteOn, 60, 0.9f}};

    SynthCore off;
    configureSmooth(off);
    std::vector<float> oL, oR;
    renderAbsEvents(off, ev, total, 512, oL, oR);
    checkNum("G12.2: kStereoMode off + kChorus Off -> outL == outR bit-exactly (G8.1 unperturbed)",
             maxAbsDiff(oL, oR, 0, total) == 0.0, maxAbsDiff(oL, oR, 0, total));

    SynthCore on;
    configureSmooth(on);
    on.setChorusMode(SynthCore::Chorus::Two);
    std::vector<float> nL, nR;
    renderAbsEvents(on, ev, total, 512, nL, nR);
    const double spread = maxAbsDiff(nL, nR, 0, total);
    checkNum("G12.2: kStereoMode off + kChorus II -> outL != outR (the chorus is what makes a "
             "mono voice stack stereo)",
             spread > 1e-3, spread);

    const int from = static_cast<int>(0.3 * kFs);
    const double corrOff = correlation(oL, oR, from, total);
    const double corrOn = correlation(nL, nR, from, total);
    checkNum("G12.2: L/R correlation is exactly 1 with the chorus off", corrOff == 1.0, corrOff);
    checkNum("G12.2: L/R correlation drops well below 1 with the chorus on II", corrOn < 0.9, corrOn);
  }

  // =========================================================================
  // G12.3: each mode's LFO runs at the rate its own table says.
  //
  // QUANTITY MEASURED: total chorus-LFO phase advanced over a known number
  // of control steps, divided by the elapsed time -- read back from
  // getDebugChorusLfoPhase(), accumulating wraps, so the measurement is the
  // real advanced phase and not a wrap count rounded to whole cycles. This
  // is the quantity "Chorus I sweeps at 0.513 Hz" literally asserts, and it
  // exercises the whole block-rate chain (kChorus -> finishSnapshot's
  // rate/fsControl -> setControlRate) rather than the table alone.
  // =========================================================================
  std::cout << "\nGroup: per-mode LFO rate (G12.3)\n";
  for (int m = 1; m <= 3; ++m) {
    SynthCore core;
    core.init(static_cast<float>(kFs));
    core.setChorusMode(static_cast<SynthCore::Chorus>(m));

    const int steps = static_cast<int>(8.0 * kFs / kControlBlock);  // 8 s of control steps
    std::vector<float> l(kControlBlock), r(kControlBlock);
    double prev = core.getDebugChorusLfoPhase();
    double totalPhase = 0.0;
    for (int i = 0; i < steps; ++i) {
      core.process(nullptr, 0, l.data(), r.data(), kControlBlock);
      const double p = core.getDebugChorusLfoPhase();
      totalPhase += (p >= prev) ? (p - prev) : (p + 1.0 - prev);
      prev = p;
    }
    const double elapsed = steps * kControlBlock / kFs;
    const double measuredHz = totalPhase / elapsed;
    const double expectedHz = JunoChorus::kRateHz[m];
    const double errPct = 100.0 * std::fabs(measuredHz - expectedHz) / expectedHz;
    checkNum(std::string("G12.3: mode ") + modeName(m) + " LFO rate matches JunoChorus::kRateHz[" +
                 std::to_string(m) + "] = " + std::to_string(expectedHz) + " Hz within 0.5%",
             errPct < 0.5, measuredHz);
  }

  // =========================================================================
  // G12.4: each mode's delay envelope spans exactly centre +/- depth, and
  // G12.5: the two lines are swept in ANTIPHASE.
  //
  // QUANTITIES MEASURED, both sampled once per control step over two full
  // LFO cycles of the mode under test:
  //   (a) min and max of the published delay, against
  //       kCenterMs[m] -/+ kDepthMs[m]. Tolerance 0.02 ms, which is an order
  //       of magnitude above the largest peak a discretely-sampled triangle
  //       can miss by (depth * 2 * rate/fsControl, worst case 3.3e-3 ms on
  //       I+II) and an order of magnitude below the smallest depth in the
  //       table.
  //   (b) delay[0] + delay[1] against 2*centre, at EVERY sample of the
  //       sweep. The two lines are driven from phase and phase+0.5 of the
  //       same triangle, and tri(p) + tri(p+0.5) == 0 identically -- so
  //       their sum is a constant, and any drift in it means the antiphase
  //       relationship the stereo image depends on has been broken.
  // =========================================================================
  std::cout << "\nGroup: per-mode delay envelope and antiphase sweep (G12.4, G12.5)\n";
  for (int m = 1; m <= 3; ++m) {
    SynthCore core;
    core.init(static_cast<float>(kFs));
    core.setChorusMode(static_cast<SynthCore::Chorus>(m));

    const double cycles = 2.0;
    const int steps = static_cast<int>(cycles * kFs / (JunoChorus::kRateHz[m] * kControlBlock)) + 4;
    std::vector<double> d0, d1;
    sweepDelays(core, steps, d0, d1);

    const double lo = *std::min_element(d0.begin(), d0.end());
    const double hi = *std::max_element(d0.begin(), d0.end());
    const double expLo = JunoChorus::kCenterMs[m] - JunoChorus::kDepthMs[m];
    const double expHi = JunoChorus::kCenterMs[m] + JunoChorus::kDepthMs[m];
    checkNum(std::string("G12.4: mode ") + modeName(m) + " delay reaches its minimum " +
                 std::to_string(expLo) + " ms (+/- 0.02)",
             std::fabs(lo - expLo) < 0.02, lo);
    checkNum(std::string("G12.4: mode ") + modeName(m) + " delay reaches its maximum " +
                 std::to_string(expHi) + " ms (+/- 0.02)",
             std::fabs(hi - expHi) < 0.02, hi);

    double maxSumErr = 0.0;
    for (size_t i = 0; i < d0.size(); ++i) {
      maxSumErr = std::max(maxSumErr, std::fabs(d0[i] + d1[i] - 2.0 * JunoChorus::kCenterMs[m]));
    }
    checkNum(std::string("G12.5: mode ") + modeName(m) +
                 " -- the two delay lines stay exactly in antiphase (delay[0]+delay[1] == 2*centre "
                 "at every control step)",
             maxSumErr < 1e-9, maxSumErr);
  }

  // =========================================================================
  // G12.6: the three modes' pitch deviations are the ones synth_chorus.h's
  // header comment claims, and are strictly ordered I < II < I+II.
  //
  // QUANTITY MEASURED: the delay's own slope, measured off the readback
  // sweep (median |d(delay)/dt| over a full cycle, excluding the two
  // turnaround steps where a triangle reverses), converted to cents via
  // 1200*log2(1 + slope). A swept delay line's pitch shift IS its delay
  // slope -- so this measures the thing a listener hears, from the running
  // implementation, rather than re-deriving the table's own arithmetic.
  // =========================================================================
  std::cout << "\nGroup: per-mode pitch deviation, measured from the delay slope (G12.6)\n";
  {
    double cents[4] = {0.0, 0.0, 0.0, 0.0};
    for (int m = 1; m <= 3; ++m) {
      SynthCore core;
      core.init(static_cast<float>(kFs));
      core.setChorusMode(static_cast<SynthCore::Chorus>(m));

      const int steps = static_cast<int>(2.0 * kFs / (JunoChorus::kRateHz[m] * kControlBlock)) + 4;
      std::vector<double> d0, d1;
      sweepDelays(core, steps, d0, d1);

      const double dt = kControlBlock / kFs;  // seconds per control step
      std::vector<double> slopes;
      for (size_t i = 1; i < d0.size(); ++i) slopes.push_back(std::fabs(d0[i] - d0[i - 1]) / dt);
      std::sort(slopes.begin(), slopes.end());
      // Median: a triangle's slope is constant except at the two turnaround
      // steps per cycle, so the median is the constant and is immune to them.
      const double slopeMsPerS = slopes[slopes.size() / 2];
      const double deviation = slopeMsPerS * 1e-3;  // ms/s -> dimensionless
      cents[m] = 1200.0 * std::log2(1.0 + deviation);

      const double expected =
          1200.0 * std::log2(1.0 + 4.0 * JunoChorus::kDepthMs[m] * 1e-3 * JunoChorus::kRateHz[m]);
      checkNum(std::string("G12.6: mode ") + modeName(m) + " peak pitch deviation is " +
                   std::to_string(expected) + " cents (+/- 0.15), i.e. 4*depth*rate as documented",
               std::fabs(cents[m] - expected) < 0.15, cents[m]);
    }
    check("G12.6: the three modes are strictly ordered by deviation, I < II < I+II -- II is roughly "
          "twice I, and I+II is a different effect rather than more of the same",
          cents[1] < cents[2] && cents[2] < cents[3]);
  }

  // =========================================================================
  // G12.7: every mode is finite and bounded under a full chord.
  //
  // QUANTITY MEASURED: peak |sample| and an all-finite scan over 2 s of an
  // 8-note chord at full velocity, for all four modes, with the chorus's
  // worst case (dry 1.0 + wet 0.7 arriving in phase) fully in play.
  // =========================================================================
  std::cout << "\nGroup: finite and bounded in every mode (G12.7)\n";
  {
    const std::vector<int> chord = {36, 43, 48, 52, 55, 60, 64, 67};
    const int total = static_cast<int>(2.0 * kFs);
    for (int m = 0; m <= 3; ++m) {
      SynthCore core;
      core.init(static_cast<float>(kFs));
      core.setChorusMode(static_cast<SynthCore::Chorus>(m));
      std::vector<NoteEvent> ev;
      for (int n : chord) ev.push_back({0, NoteEvent::NoteOn, n, 1.0f});
      std::vector<float> l, r;
      renderAbsEvents(core, ev, total, 512, l, r);

      bool finite = true;
      for (size_t i = 0; i < l.size(); ++i) {
        if (!std::isfinite(l[i]) || !std::isfinite(r[i])) {
          finite = false;
          break;
        }
      }
      const double peak = std::max(maxAbs(l), maxAbs(r));
      check((std::string("G12.7: mode ") + modeName(m) + " -- all samples finite").c_str(), finite);
      checkNum(std::string("G12.7: mode ") + modeName(m) + " -- peak stays bounded (|y| < 8)",
               peak < 8.0, peak);
    }
  }

  // =========================================================================
  // G12.8: switching the chorus on or off mid-note does not click.
  //
  // QUANTITY MEASURED: the largest sample-to-sample step in the 100 ms
  // straddling the toggle, against the largest step either STEADY STATE
  // produces -- chorus permanently off, and chorus permanently on. A click
  // is by definition a step neither steady state would have produced, so
  // the worse of the two is the right reference.
  //
  // Using the chorus-OFF render alone would be WRONG, and measurably so: at
  // full wet the output is dry + 0.7*wet, and wet is the same signal
  // delayed, so the summed slope can legitimately reach ~1.7x the dry's own
  // without anything having clicked. An earlier draft of this AC did
  // exactly that and read 1.59 -- which was the chorus being present, not
  // the toggle being abrupt.
  //
  // The patch is deliberately a low-passed triangle (configureSmooth): on a
  // saw, the wrap discontinuity dwarfs anything a 20 ms fade could do and
  // the measurement would be meaningless.
  // =========================================================================
  std::cout << "\nGroup: click-free enable/disable (G12.8)\n";
  {
    const int total = static_cast<int>(2.0 * kFs);
    const int toggleAt = static_cast<int>(1.0 * kFs);
    const int win = static_cast<int>(0.05 * kFs);
    const std::vector<NoteEvent> ev = {{0, NoteEvent::NoteOn, 55, 0.9f}};

    SynthCore refOff;
    configureSmooth(refOff);
    std::vector<float> rL, rR;
    renderAbsEvents(refOff, ev, total, 512, rL, rR);

    SynthCore refOn;
    configureSmooth(refOn);
    refOn.setChorusMode(SynthCore::Chorus::Two);
    std::vector<float> oL, oR;
    renderAbsEvents(refOn, ev, total, 512, oL, oR);

    const double refStep = std::max(std::max(maxStep(rL, 0, total), maxStep(rR, 0, total)),
                                     std::max(maxStep(oL, 0, total), maxStep(oR, 0, total)));

    for (int direction = 0; direction < 2; ++direction) {
      const bool onAtToggle = direction == 0;
      SynthCore core;
      configureSmooth(core);
      if (!onAtToggle) core.setChorusMode(SynthCore::Chorus::Two);

      std::vector<float> L(static_cast<size_t>(total), 0.0f), R(static_cast<size_t>(total), 0.0f);
      std::vector<float> segL, segR;
      renderAbsEvents(core, ev, toggleAt, 512, segL, segR);
      std::copy(segL.begin(), segL.end(), L.begin());
      std::copy(segR.begin(), segR.end(), R.begin());
      core.setChorusMode(onAtToggle ? SynthCore::Chorus::Two : SynthCore::Chorus::Off);
      renderAbsEvents(core, {}, total - toggleAt, 512, segL, segR);
      std::copy(segL.begin(), segL.end(), L.begin() + toggleAt);
      std::copy(segR.begin(), segR.end(), R.begin() + toggleAt);

      const double step = std::max(maxStep(L, toggleAt - win, toggleAt + win),
                                    maxStep(R, toggleAt - win, toggleAt + win));
      checkNum(std::string("G12.8: switching the chorus ") + (onAtToggle ? "ON" : "OFF") +
                   " mid-note produces no step larger than either steady state's own worst step "
                   "(ratio < 1.05)",
               step < 1.05 * refStep, step / refStep);
    }
  }

  // =========================================================================
  // G12.9: the wet fade is linear, 20 ms long, and lands on EXACTLY 1.0 and
  // EXACTLY 0.0.
  //
  // QUANTITY MEASURED: the number of samples between the first sample at
  // which the wet gain leaves its start value and the sample at which it
  // first equals its target exactly, rendering one sample per process()
  // call so the count is unambiguous. `kFadeMs * fs / 1000` = 960 samples at
  // 48 kHz. Landing on exactly 0.0 is not cosmetic: it is the condition
  // G12.1's bit-exact passthrough resumes on.
  //
  // The core is run for a while with the chorus Off FIRST, so that the
  // switch-on below is a genuine runtime toggle rather than the first
  // control block ever (which snaps by design -- see JunoChorus's own
  // `firstBlock`).
  // =========================================================================
  std::cout << "\nGroup: the 20 ms wet fade, exact endpoints (G12.9)\n";
  {
    SynthCore core;
    configureSmooth(core);
    std::vector<float> l(1), r(1);
    for (int i = 0; i < 4800; ++i) core.process(nullptr, 0, l.data(), r.data(), 1);
    check("G12.9: wet gain sits at exactly 0.0 while the chorus is off",
          core.getDebugChorusWetGain() == 0.0);

    core.setChorusMode(SynthCore::Chorus::One);
    int samplesToFull = 0;
    for (int i = 0; i < 4800; ++i) {
      core.process(nullptr, 0, l.data(), r.data(), 1);
      ++samplesToFull;
      if (core.getDebugChorusWetGain() == 1.0) break;
    }
    const double expected = JunoChorus::kFadeMs * 1e-3 * kFs;  // 960 samples
    checkNum("G12.9: fade-in reaches EXACTLY 1.0 after kFadeMs (960 samples at 48 kHz, +/- 2)",
             core.getDebugChorusWetGain() == 1.0 && std::fabs(samplesToFull - expected) <= 2.0,
             samplesToFull);

    core.setChorusMode(SynthCore::Chorus::Off);
    int samplesToZero = 0;
    for (int i = 0; i < 4800; ++i) {
      core.process(nullptr, 0, l.data(), r.data(), 1);
      ++samplesToZero;
      if (core.getDebugChorusWetGain() == 0.0) break;
    }
    checkNum("G12.9: fade-out reaches EXACTLY 0.0 after kFadeMs -- which is what re-arms the "
             "bit-exact passthrough (G12.1)",
             core.getDebugChorusWetGain() == 0.0 && std::fabs(samplesToZero - expected) <= 2.0,
             samplesToZero);
    check("G12.9: once faded out, the core reports its chorus path as inaudible again",
          !core.getDebugChorusAudible());
  }

  // =========================================================================
  // G12.10: switching BETWEEN two live modes glides the delay geometry
  // instead of stepping it.
  //
  // QUANTITY MEASURED: the largest single-control-step change in the
  // published delay across a II -> I+II switch -- the largest geometry
  // change the four positions can produce (depth 1.80 -> 0.25 ms AND rate
  // 0.863 -> 9.750 Hz at the same time). Unglided, the depth alone would
  // step the delay by ~1.55 ms in ONE control block; with both quantities
  // on the kGlideMs one-pole the worst step measures 0.018 ms, so a 0.05 ms
  // bound passes comfortably and fails hard if either glide is removed.
  //
  // FOUND HERE (R11): gliding the depth but NOT the rate measured 0.054 ms,
  // i.e. it FAILED this bound -- correctly. The new rate was driving the old
  // depth for the ~150 ms the depth took to settle, which is a 117-cent
  // pitch swoop against I+II's own 16.8 cents. See synth_chorus.h's
  // setControlRate() for the arithmetic; the fix was to glide both.
  // =========================================================================
  std::cout << "\nGroup: live mode changes glide, not jump (G12.10)\n";
  {
    SynthCore core;
    core.init(static_cast<float>(kFs));
    core.setChorusMode(SynthCore::Chorus::Two);
    std::vector<double> d0, d1;
    sweepDelays(core, 2000, d0, d1);  // settle on II

    core.setChorusMode(SynthCore::Chorus::OneTwo);
    std::vector<double> g0, g1;
    sweepDelays(core, 4000, g0, g1);

    double maxStepMs = 0.0;
    double prevDelay = d0.back();
    for (double d : g0) {
      maxStepMs = std::max(maxStepMs, std::fabs(d - prevDelay));
      prevDelay = d;
    }
    checkNum("G12.10: a II -> I+II switch never moves the delay more than 0.05 ms in one control "
             "step (unglided this jump would be ~1.55 ms)",
             maxStepMs < 0.05, maxStepMs);

    // ...and it must actually ARRIVE, not merely move slowly.
    const double lo = *std::min_element(g0.end() - 500, g0.end());
    const double hi = *std::max_element(g0.end() - 500, g0.end());
    const double expLo = JunoChorus::kCenterMs[3] - JunoChorus::kDepthMs[3];
    const double expHi = JunoChorus::kCenterMs[3] + JunoChorus::kDepthMs[3];
    checkNum("G12.10: the glide settles on I+II's own geometry (min)", std::fabs(lo - expLo) < 0.02, lo);
    checkNum("G12.10: the glide settles on I+II's own geometry (max)", std::fabs(hi - expHi) < 0.02, hi);
  }

  // =========================================================================
  // G12.11: block-size invariance with the chorus running.
  //
  // QUANTITY MEASURED: max abs sample difference across renders of the SAME
  // absolute event sequence delivered in host blocks of 1, 7, 32, 33, 512
  // and 8192 samples, with kChorus = II. This is G3.3's AC re-run against
  // the new stage, and it is the reason the chorus LFO advances on the
  // control-rate GRID and the delay is interpolated with the same `frac`
  // the VCA gain uses: anything anchored to host-block boundaries instead
  // would fail here.
  // =========================================================================
  std::cout << "\nGroup: block-size invariance with the chorus on (G12.11)\n";
  {
    const int total = static_cast<int>(1.0 * kFs);
    const std::vector<NoteEvent> ev = {{0, NoteEvent::NoteOn, 60, 0.9f},
                                        {static_cast<int>(0.5 * kFs), NoteEvent::NoteOff, 60, 0.0f}};
    const int blockSizes[] = {1, 7, 32, 33, 512, 8192};
    std::vector<std::vector<float>> rendersL, rendersR;
    for (int bs : blockSizes) {
      SynthCore core;
      configureSmooth(core);
      core.setChorusMode(SynthCore::Chorus::Two);
      std::vector<float> l, r;
      renderAbsEvents(core, ev, total, bs, l, r);
      rendersL.push_back(l);
      rendersR.push_back(r);
    }
    double maxDiff = 0.0;
    for (size_t k = 1; k < rendersL.size(); ++k) {
      maxDiff = std::max(maxDiff, maxAbsDiff(rendersL[k], rendersL[0], 0, total));
      maxDiff = std::max(maxDiff, maxAbsDiff(rendersR[k], rendersR[0], 0, total));
    }
    checkNum("G12.11: identical output at host block sizes 1/7/32/33/512/8192 with kChorus = II",
             maxDiff == 0.0, maxDiff);
  }

  // =========================================================================
  // G12.12: reset() clears the chorus, and a reset instance is
  // indistinguishable from a fresh one (R13).
  //
  // QUANTITY MEASURED: max abs sample difference between a fresh core's
  // render and the SAME core's second render after reset() -- with the
  // chorus on, so the delay lines are full of the first render's audio and
  // the LFO is mid-cycle when reset() is called. Exactly 0.0 is the only
  // passing value; any leftover delay-line content or LFO phase shows up
  // immediately.
  // =========================================================================
  std::cout << "\nGroup: reset() clears the chorus completely (G12.12)\n";
  {
    const int total = static_cast<int>(0.75 * kFs);
    const std::vector<NoteEvent> ev = {{0, NoteEvent::NoteOn, 62, 0.9f}};

    SynthCore core;
    configureSmooth(core);
    core.setChorusMode(SynthCore::Chorus::Two);
    std::vector<float> firstL, firstR;
    renderAbsEvents(core, ev, total, 512, firstL, firstR);

    core.reset();
    std::vector<float> secondL, secondR;
    renderAbsEvents(core, ev, total, 512, secondL, secondR);

    const double diff = std::max(maxAbsDiff(firstL, secondL, 0, total),
                                  maxAbsDiff(firstR, secondR, 0, total));
    checkNum("G12.12: a reset() core renders BIT-IDENTICALLY to its own first render, with the "
             "chorus on throughout (delay lines, LFO phase and fade gain all cleared)",
             diff == 0.0, diff);
  }

  // =========================================================================
  // G12.13: zero heap allocation in process() with the chorus running.
  //
  // QUANTITY MEASURED: the delta of an instrumented global operator new
  // counter across a 2 s render in every mode, including mid-render mode
  // changes. The chorus owns the largest buffer in the tree; this is the AC
  // that says it is genuinely fixed-size and constructed once (R3).
  // =========================================================================
  std::cout << "\nGroup: zero heap allocation with the chorus running (G12.13)\n";
  {
    SynthCore core;
    configureSmooth(core);
    const int block = 512;
    std::vector<float> l(static_cast<size_t>(block)), r(static_cast<size_t>(block));
    std::vector<NoteEvent> ev = {{0, NoteEvent::NoteOn, 60, 0.9f}};

    // Warm up outside the measured region (the first call touches nothing
    // that allocates, but keeping the measured window to pure process()
    // calls is the point).
    core.process(ev.data(), 1, l.data(), r.data(), block);

    const std::size_t before = gAllocCount;
    const int blocks = static_cast<int>(2.0 * kFs) / block;
    for (int b = 0; b < blocks; ++b) {
      if (b == blocks / 4) core.setChorusMode(SynthCore::Chorus::One);
      if (b == blocks / 2) core.setChorusMode(SynthCore::Chorus::OneTwo);
      if (b == 3 * blocks / 4) core.setChorusMode(SynthCore::Chorus::Off);
      core.process(nullptr, 0, l.data(), r.data(), block);
    }
    const std::size_t allocated = gAllocCount - before;
    checkNum("G12.13: 2 s of process() across every chorus mode allocates nothing",
             allocated == 0, static_cast<double>(allocated));
  }

  // =========================================================================
  // G12.14: R12 holds for the chorus's own per-sample path.
  //
  // QUANTITY MEASURED: the ACTUAL SOURCE TEXT of Source/DSP/synth_chorus.h
  // between its "PER-SAMPLE PROCESS BEGIN/END" markers, grepped for
  // tan(/exp(/exp2(/pow(/log(/sin(/cos(/.load(. Same grep+inspection
  // methodology as G3.2(b) and G4.11, automated here for the same reason:
  // this code runs INSIDE SynthCore's audio-rate loop, but lives in a
  // different file, so G3.2(b)'s own region scan cannot see it.
  // =========================================================================
  std::cout << "\nGroup: R12 -- no transcendental, no atomic load in the chorus's per-sample path (G12.14)\n";
  {
#ifdef NASSAU_CHORUS_H_PATH
    std::ifstream f(NASSAU_CHORUS_H_PATH);
    const std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const size_t b0 = src.find("PER-SAMPLE PROCESS BEGIN");
    const size_t e0 = src.find("PER-SAMPLE PROCESS END");
    check("G12.14: found the PER-SAMPLE PROCESS BEGIN/END markers in synth_chorus.h",
          !src.empty() && b0 != std::string::npos && e0 != std::string::npos && e0 > b0);
    if (b0 != std::string::npos && e0 != std::string::npos && e0 > b0) {
      const std::string region = src.substr(b0, e0 - b0);
      const char* forbidden[] = {"tan(", "exp(", "exp2(", "pow(", "log(", "sin(", "cos(", ".load("};
      bool clean = true;
      for (const char* tok : forbidden) {
        if (region.find(tok) != std::string::npos) {
          clean = false;
          std::cout << "    forbidden token found in the per-sample region: " << tok << "\n";
        }
      }
      check("G12.14: the chorus's per-sample source text contains none of "
            "tan(/exp(/exp2(/pow(/log(/sin(/cos(/.load(",
            clean);
    }
#else
    check("G12.14: NASSAU_CHORUS_H_PATH was not defined at compile time -- inspection skipped", false);
#endif
  }

  std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
  return g_failures == 0 ? 0 : 1;
}
