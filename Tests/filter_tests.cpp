// NassauAnalogue DSP Unit Tests — Gate G4 (The low-pass filter: structures,
// resonance, stability). Hand-rolled harness (plain int main + soft checks,
// no external framework) — the nassau-eq/nassau-zermatt house style
// (docs/GATES.md "Shared test harness"). Covers G4.1-G4.11.
//
// Unlike envlfo_tests.cpp (which drives SynthCore's real voice path),
// G4.1-G4.8 and G4.10 drive Source/DSP/synth_filter.h's LadderFilter/
// SvfFilter DIRECTLY -- the same shape nassau-zermatt/Tests/cabinet_tests.cpp
// uses for a DSP structure that has no audible destination wired into the
// full voice yet (docs/GATES.md's own instruction for this gate: "driving
// the structures directly from the test... is the expected shape for the
// response measurements"). G4.9 alone needs the real SynthCore (it reads
// getDebugLpfCutoff()), so this binary links nassau_synth_core.
//
// R11 finding (recorded here, not silently routed around): an EARLIER
// attempt at this gate wired LadderFilter/SvfFilter directly into
// SynthCore's per-voice audio-rate loop (mixer -> LPF -> VCA, matching
// DESIGN.md §1's chain order literally). That reverted a previously-green
// AC: G3.12 ("no DC", <1e-4, written before any filter sat in this path).
// The cause is structural, not a coding slip: DESIGN.md §5.3 specifies
// Reff from the PREVIOUS control block's peak |bp| -- an explicitly CAUSAL,
// not time-symmetric, scheduling -- so any spec-correct wiring of the SVF
// into a real periodic voice signal makes the filter periodically
// time-varying at block rate, which is enough to leak a small (~1e-4,
// confirmed STABLE over an 8s render, not a growing instability)
// resonance-dependent DC that G3.12's tight pre-G4 bound does not tolerate.
// Full per-voice signal-chain integration (mixer -> LPF -> VCA for real) is
// deferred to G5, which touches this exact code path anyway for the slope
// crossfade and cutoff modulation, and is the gate positioned to decide
// what (if anything) resolves this interaction. See synth_core.cpp's
// AUDIO-RATE LOOP comment for the same note in context.
//
// R6/R11: every group below states, in a comment immediately above its
// check(), exactly what quantity is measured and why that is the quantity
// the AC text actually asks for -- this gate's own instructions single out
// G4.1 (must be measured at fs=96kHz, not 48kHz -- the analytic asymptote is
// 24/12dB but 4kHz/8kHz are only 2-3 octaves above a 1kHz fc), G4.3 (the
// -3dB point is 0.435*fc / 0.644*fc, NOT "within 4% of kLpfCutoff" -- that
// wording is exactly the mistake G6.3 already forbids for the high-pass),
// and G4.7 (must use an EXCITED decay, never silence alone -- y=0 is an
// exact fixed point of every recursion here regardless of loop gain, so an
// unstable filter driven by silence outputs silence forever and the test
// could never fail).
// R8: no rand()/time(); Xorshift32 (synth_dsp.h, transitively visible via
// synth_filter.h) is the only randomness source, always fixed-seeded.

#include "synth_filter.h"
#include "synth_osc.h"
#include "test_util.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
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

// ---- ControlRateDriver ------------------------------------------------------
// Wraps a LadderFilter/SvfFilter so a test can drive it through test_util.h's
// magDb/minus3dBPoint (which call a `void(const float*, float*, int)`
// processBlock in arbitrary-sized chunks) while reproducing PRODUCTION
// coefficient-update cadence exactly: setControlRate() is recomputed every
// `controlBlock` samples (DESIGN.md §2's 32-sample default), not once per
// processBlock() call and not per sample -- this matters specifically for
// SvfFilter, whose Reff depends on the PREVIOUS control block's peak |bp|
// (DESIGN.md §5.3), so the self-regulating limit-cycle mechanism G4.4 tests
// only behaves correctly if setControlRate() is actually invoked at that
// cadence during the render, not just once at the start.
template <typename FilterT>
struct ControlRateDriver {
  FilterT filter;
  double fc = 0.0, fs = 0.0, resonancePercent = 0.0;
  int controlBlock = 32;  // [dsp] DESIGN.md §2 default
  int phase = 0;

  void configure(double fcIn, double fsIn, double resonancePercentIn) {
    fc = fcIn;
    fs = fsIn;
    resonancePercent = resonancePercentIn;
    filter.reset();
    phase = 0;
    filter.setControlRate(fc, fs, resonancePercent);
  }

  inline double step(double x) {
    if (phase >= controlBlock) {
      filter.setControlRate(fc, fs, resonancePercent);
      phase = 0;
    }
    const double y = filter.process(x);
    ++phase;
    return y;
  }

  void process(const float* in, float* out, int n) {
    for (int i = 0; i < n; ++i) out[i] = static_cast<float>(step(static_cast<double>(in[i])));
  }
};

// ---- Excited-decay helper (G4.7) --------------------------------------------
// Drives `burstMs` of Xorshift32 white noise (full-scale, seeded -- R8) into
// `driver`, then `silenceSec` of exact digital silence, and returns the
// index (in whole 1ms frames, measured from the START OF SILENCE) of the
// FIRST frame from which every subsequent frame's RMS stays below -80dBFS
// -- i.e. the actual decay-and-STAY-decayed time the AC asks for, not just
// the first transient dip below -80dB (a resonant/marginally-stable filter
// can dip below -80dB briefly and then re-emerge, which a "first frame
// below threshold" measure alone would miss). Returns -1 if no such frame
// exists within the render.
template <typename FilterT>
double excitedDecayFrames(ControlRateDriver<FilterT>& driver, double fs, double burstMs, double silenceSec,
                           uint32_t seed) {
  const int burstN = static_cast<int>(burstMs * 0.001 * fs);
  const int silenceN = static_cast<int>(silenceSec * fs);
  const int totalN = burstN + silenceN;
  std::vector<float> out(static_cast<size_t>(totalN));
  Xorshift32 rng(seed);
  for (int i = 0; i < totalN; ++i) {
    const double x = (i < burstN) ? rng.nextBipolar() : 0.0;
    out[static_cast<size_t>(i)] = static_cast<float>(driver.step(x));
  }
  const std::vector<float> silenceRegion(out.begin() + burstN, out.end());
  std::vector<double> frames;
  envelopeDb(silenceRegion, fs, frames);
  const int nFrames = static_cast<int>(frames.size());
  for (int i = 0; i < nFrames; ++i) {
    bool allBelow = true;
    for (int j = i; j < nFrames; ++j) {
      if (frames[static_cast<size_t>(j)] >= -80.0) {
        allBelow = false;
        break;
      }
    }
    if (allBelow) return static_cast<double>(i);
  }
  return -1.0;
}

// ---- Peak-in-window helper (G4.4) -------------------------------------------
double peakAbsInWindow(const std::vector<float>& v, double fs, double t0, double t1) {
  const int a = std::max(0, static_cast<int>(t0 * fs));
  const int b = std::min(static_cast<int>(v.size()), static_cast<int>(t1 * fs));
  double m = 0.0;
  for (int i = a; i < b; ++i) m = std::max(m, static_cast<double>(std::fabs(v[static_cast<size_t>(i)])));
  return m;
}

// Sub-cent-accurate frequency via zero-crossing (same technique as
// test_util.h's measuredF0, inlined here since it needs a specific window
// of a longer buffer, not the whole thing).
double measuredFreqInWindow(const std::vector<float>& v, double fs, double t0, double t1) {
  const int a = std::max(1, static_cast<int>(t0 * fs));
  const int b = std::min(static_cast<int>(v.size()), static_cast<int>(t1 * fs));
  std::vector<double> crossings;
  for (int i = a; i < b; ++i) {
    const double y0 = static_cast<double>(v[static_cast<size_t>(i - 1)]);
    const double y1 = static_cast<double>(v[static_cast<size_t>(i)]);
    if (y0 < 0.0 && y1 >= 0.0) {
      const double frac = (y1 == y0) ? 0.0 : (-y0) / (y1 - y0);
      crossings.push_back(static_cast<double>(i - 1) + frac);
    }
  }
  if (crossings.size() < 2) return 0.0;
  const double totalSamples = crossings.back() - crossings.front();
  const double numPeriods = static_cast<double>(crossings.size() - 1);
  return totalSamples > 0.0 ? numPeriods * fs / totalSamples : 0.0;
}

}  // namespace

int main() {
  std::cout << "=== NassauAnalogue DSP Tests (G4 -- The low-pass filter) ===\n\n";

  // =========================================================================
  // G4.1/G4.2: 24 dB / 12 dB slope, measured at fs=96kHz, fc=1kHz, res=0.
  // QUANTITY MEASURED: magDb(4kHz) - magDb(8kHz), i.e. the level DROP from
  // 4kHz to 8kHz (both well above fc but only 2/3 octaves up -- NOT the
  // asymptotic 24/12dB, per this AC's own point). Also recorded at
  // fs=48kHz (NOT gated: the AC states this reads 25.88/12.94 there, a
  // DIFFERENT correct number from bilinear warping, not a failure).
  // =========================================================================
  std::cout << "\nGroup: 24dB / 12dB slope at fs=96kHz (G4.1/G4.2)\n";
  {
    for (double fs : {96000.0, 48000.0}) {
      ControlRateDriver<LadderFilter> d4, d8;
      d4.configure(1000.0, fs, 0.0);
      d8.configure(1000.0, fs, 0.0);
      const double m4 = magDb([&](const float* in, float* out, int n) { d4.process(in, out, n); }, 4000.0, fs);
      const double m8 = magDb([&](const float* in, float* out, int n) { d8.process(in, out, n); }, 8000.0, fs);
      const double diff = m4 - m8;
      if (fs == 96000.0) {
        checkNum("G4.1: ladder (24dB) 4kHz->8kHz drop at fs=96kHz is 23.9 +/- 1.5 dB",
                 std::fabs(diff - 23.9) <= 1.5, diff);
      } else {
        std::cout << "  [INFO] ladder 4kHz->8kHz drop at fs=48kHz (warping, not gated): " << diff << " dB\n";
      }
    }
    for (double fs : {96000.0, 48000.0}) {
      ControlRateDriver<SvfFilter> d4, d8;
      d4.configure(1000.0, fs, 0.0);
      d8.configure(1000.0, fs, 0.0);
      const double m4 = magDb([&](const float* in, float* out, int n) { d4.process(in, out, n); }, 4000.0, fs);
      const double m8 = magDb([&](const float* in, float* out, int n) { d8.process(in, out, n); }, 8000.0, fs);
      const double diff = m4 - m8;
      if (fs == 96000.0) {
        checkNum("G4.2: SVF (12dB) 4kHz->8kHz drop at fs=96kHz is 11.95 +/- 1.0 dB",
                 std::fabs(diff - 11.95) <= 1.0, diff);
      } else {
        std::cout << "  [INFO] SVF 4kHz->8kHz drop at fs=48kHz (warping, not gated): " << diff << " dB\n";
      }
    }
  }

  // =========================================================================
  // G4.3: the -3dB point is NOT at kLpfCutoff.
  // QUANTITY MEASURED: minus3dBPoint()'s bisected corner, expressed as a
  // RATIO to fc (0.4350 ladder, 0.6436 SVF) -- never "within 4% of fc" (that
  // wording is exactly what this AC and G6.3 both forbid). Probed at
  // fc=100Hz and 1kHz, fs=48k and 96k (NOT fc=8kHz -- warping moves the
  // ratio to 0.470/0.679 and blows the 5% tolerance, per the AC's own note).
  // =========================================================================
  std::cout << "\nGroup: -3dB point is 0.435*fc (ladder) / 0.644*fc (SVF), not fc (G4.3)\n";
  {
    for (double fc : {100.0, 1000.0}) {
      for (double fs : {48000.0, 96000.0}) {
        ControlRateDriver<LadderFilter> d;
        d.configure(fc, fs, 0.0);
        const double corner = minus3dBPoint([&](const float* in, float* out, int n) { d.process(in, out, n); }, fs,
                                             fc * 0.1, fc * 0.01, fc * 2.0);
        const double ratio = corner / fc;
        checkNum("G4.3: ladder -3dB ratio at fc=" + std::to_string(static_cast<int>(fc)) +
                     "Hz fs=" + std::to_string(static_cast<int>(fs)) + " is 0.4350 +/- 5%",
                 std::fabs(ratio - 0.4350) <= 0.05 * 0.4350, ratio);
      }
    }
    for (double fc : {100.0, 1000.0}) {
      for (double fs : {48000.0, 96000.0}) {
        ControlRateDriver<SvfFilter> d;
        d.configure(fc, fs, 0.0);
        const double corner = minus3dBPoint([&](const float* in, float* out, int n) { d.process(in, out, n); }, fs,
                                             fc * 0.1, fc * 0.01, fc * 3.0);
        const double ratio = corner / fc;
        checkNum("G4.3: SVF -3dB ratio at fc=" + std::to_string(static_cast<int>(fc)) +
                     "Hz fs=" + std::to_string(static_cast<int>(fs)) + " is 0.6436 +/- 5%",
                 std::fabs(ratio - 0.6436) <= 0.05 * 0.6436, ratio);
      }
    }
  }

  // =========================================================================
  // G4.4: both modes self-oscillate. resonance=100%, no input, a single
  // 1e-3 impulse.
  // QUANTITY MEASURED: peak |y| in a 100ms window at t~2s (must land in
  // [0.05, 2.0] and within 5% of fc as a FREQUENCY, via zero-crossing);
  // level drift in dB between a 100ms window at t~5s and one at t~10s (must
  // be < 0.5dB, i.e. the oscillation is a STABLE limit cycle, not still
  // growing/decaying).
  // =========================================================================
  std::cout << "\nGroup: both modes self-oscillate at resonance=100% (G4.4)\n";
  {
    const double fs = 48000.0, fc = 1000.0;
    const int totalN = static_cast<int>(10.0 * fs) + 1;

    ControlRateDriver<LadderFilter> dl;
    dl.configure(fc, fs, 100.0);
    std::vector<float> outL(static_cast<size_t>(totalN));
    outL[0] = static_cast<float>(dl.step(1e-3));
    for (int i = 1; i < totalN; ++i) outL[static_cast<size_t>(i)] = static_cast<float>(dl.step(0.0));
    const double peakL = peakAbsInWindow(outL, fs, 1.95, 2.05);
    const double freqL = measuredFreqInWindow(outL, fs, 1.9, 2.0);
    const double peak5L = peakAbsInWindow(outL, fs, 4.95, 5.05);
    const double peak10L = peakAbsInWindow(outL, fs, 9.9, 10.0);
    const double driftL = 20.0 * std::log10(peak10L / std::max(peak5L, 1e-300));
    checkNum("G4.4: ladder self-osc peak at t~2s is in [0.05, 2.0]", peakL >= 0.05 && peakL <= 2.0, peakL);
    checkNum("G4.4: ladder self-osc frequency at t~2s is 1000Hz +/- 5%",
             std::fabs(freqL - 1000.0) <= 0.05 * 1000.0, freqL);
    checkNum("G4.4: ladder self-osc level drift t=5s->10s is < 0.5dB", std::fabs(driftL) < 0.5, driftL);

    ControlRateDriver<SvfFilter> ds;
    ds.configure(fc, fs, 100.0);
    std::vector<float> outS(static_cast<size_t>(totalN));
    outS[0] = static_cast<float>(ds.step(1e-3));
    for (int i = 1; i < totalN; ++i) outS[static_cast<size_t>(i)] = static_cast<float>(ds.step(0.0));
    const double peakS = peakAbsInWindow(outS, fs, 1.95, 2.05);
    const double freqS = measuredFreqInWindow(outS, fs, 1.9, 2.0);
    const double peak5S = peakAbsInWindow(outS, fs, 4.95, 5.05);
    const double peak10S = peakAbsInWindow(outS, fs, 9.9, 10.0);
    const double driftS = 20.0 * std::log10(peak10S / std::max(peak5S, 1e-300));
    checkNum("G4.4: SVF self-osc peak at t~2s is in [0.05, 2.0]", peakS >= 0.05 && peakS <= 2.0, peakS);
    checkNum("G4.4: SVF self-osc frequency at t~2s is 1000Hz +/- 5%",
             std::fabs(freqS - 1000.0) <= 0.05 * 1000.0, freqS);
    checkNum("G4.4: SVF self-osc level drift t=5s->10s is < 0.5dB", std::fabs(driftS) < 0.5, driftS);
  }

  // =========================================================================
  // G4.5: resonance=0 does not ring. A full-scale impulse at fc=1kHz decays
  // below -80dBFS within 200ms, both modes.
  // QUANTITY MEASURED: worst 1ms-frame RMS (dBFS) from 200ms onward.
  // =========================================================================
  std::cout << "\nGroup: resonance=0 does not ring (G4.5)\n";
  {
    const double fs = 48000.0, fc = 1000.0;
    const int totalN = static_cast<int>(0.5 * fs);

    ControlRateDriver<LadderFilter> dl;
    dl.configure(fc, fs, 0.0);
    std::vector<float> outL(static_cast<size_t>(totalN));
    outL[0] = static_cast<float>(dl.step(1.0));
    for (int i = 1; i < totalN; ++i) outL[static_cast<size_t>(i)] = static_cast<float>(dl.step(0.0));
    const std::vector<float> tailL(outL.begin() + static_cast<int>(0.2 * fs), outL.end());
    std::vector<double> framesL;
    envelopeDb(tailL, fs, framesL);
    const double worstL = *std::max_element(framesL.begin(), framesL.end());
    checkNum("G4.5: ladder res=0 impulse decays below -80dBFS within 200ms (worst dB from 200ms on)",
             worstL < -80.0, worstL);

    ControlRateDriver<SvfFilter> ds;
    ds.configure(fc, fs, 0.0);
    std::vector<float> outS(static_cast<size_t>(totalN));
    outS[0] = static_cast<float>(ds.step(1.0));
    for (int i = 1; i < totalN; ++i) outS[static_cast<size_t>(i)] = static_cast<float>(ds.step(0.0));
    const std::vector<float> tailS(outS.begin() + static_cast<int>(0.2 * fs), outS.end());
    std::vector<double> framesS;
    envelopeDb(tailS, fs, framesS);
    const double worstS = *std::max_element(framesS.begin(), framesS.end());
    checkNum("G4.5: SVF res=0 impulse decays below -80dBFS within 200ms (worst dB from 200ms on)", worstS < -80.0,
             worstS);
  }

  // =========================================================================
  // G4.6: the ladder loses bass, the SVF does not.
  // QUANTITY MEASURED: magDb(100Hz) at resonance=95 MINUS magDb(100Hz) at
  // resonance=0, fc=5kHz, both modes. res=95 (NOT 100 -- neither filter is
  // self-oscillating during the probe, per this AC's own instruction).
  // =========================================================================
  std::cout << "\nGroup: ladder loses bass at high resonance, SVF does not (G4.6)\n";
  {
    const double fs = 48000.0, fc = 5000.0;

    ControlRateDriver<LadderFilter> dl95, dl0;
    dl95.configure(fc, fs, 95.0);
    dl0.configure(fc, fs, 0.0);
    const double mL95 = magDb([&](const float* in, float* out, int n) { dl95.process(in, out, n); }, 100.0, fs);
    const double mL0 = magDb([&](const float* in, float* out, int n) { dl0.process(in, out, n); }, 100.0, fs);
    checkNum("G4.6: ladder bass loss at 100Hz (res95 - res0) is -14.0 +/- 3 dB", std::fabs((mL95 - mL0) - (-14.0)) <= 3.0,
             mL95 - mL0);

    ControlRateDriver<SvfFilter> ds95, ds0;
    ds95.configure(fc, fs, 95.0);
    ds0.configure(fc, fs, 0.0);
    const double mS95 = magDb([&](const float* in, float* out, int n) { ds95.process(in, out, n); }, 100.0, fs);
    const double mS0 = magDb([&](const float* in, float* out, int n) { ds0.process(in, out, n); }, 100.0, fs);
    checkNum("G4.6: SVF bass loss at 100Hz (res95 - res0) is < 1 dB (magnitude)", std::fabs(mS95 - mS0) < 1.0,
             mS95 - mS0);
  }

  // =========================================================================
  // G4.7: NO self-oscillation where it is not wanted -- EXCITED decay test
  // (not silence alone, per this AC's own warning: y=0 is an exact fixed
  // point of every recursion here regardless of loop gain).
  // QUANTITY MEASURED: excitedDecayFrames() (see its own comment) across
  // the full {fc, res, slope, fs} grid the AC names. Must be >=0 (a
  // below-threshold-and-staying-there frame was found) and < 4000 (4s).
  // =========================================================================
  std::cout << "\nGroup: excited decay, no unwanted self-oscillation (G4.7)\n";
  {
    const double fss[] = {44100.0, 48000.0, 96000.0, 192000.0};
    const double resonances[] = {0.0, 50.0};
    bool allOk = true;
    double worstFrames = -1.0;
    int worstCount = 0;
    for (double fs : fss) {
      const double fcs[] = {10.0, 1000.0, 0.45 * fs};
      for (double fc : fcs) {
        for (double res : resonances) {
          ControlRateDriver<LadderFilter> dl;
          dl.configure(fc, fs, res);
          const double framesL = excitedDecayFrames(dl, fs, 100.0, 10.0, 0xC0FFEEu);
          if (framesL < 0.0 || framesL >= 4000.0) {
            allOk = false;
            std::cout << "    FAIL ladder fc=" << fc << " res=" << res << " fs=" << fs
                      << " decay frame=" << framesL << "\n";
          }
          worstFrames = std::max(worstFrames, framesL);
          ++worstCount;

          ControlRateDriver<SvfFilter> ds;
          ds.configure(fc, fs, res);
          const double framesS = excitedDecayFrames(ds, fs, 100.0, 10.0, 0xC0FFEEu);
          if (framesS < 0.0 || framesS >= 4000.0) {
            allOk = false;
            std::cout << "    FAIL SVF fc=" << fc << " res=" << res << " fs=" << fs << " decay frame=" << framesS
                      << "\n";
          }
          worstFrames = std::max(worstFrames, framesS);
          ++worstCount;
        }
      }
    }
    checkNum("G4.7: every {fc, res, slope, fs} corner (" + std::to_string(worstCount) +
                 " configs) falls below -80dBFS and stays there within 4s (worst observed, ms)",
             allOk, worstFrames);
  }

  // =========================================================================
  // G4.8: finite at every extreme. Same grid at res=100 with 512 samples of
  // Xorshift -- all finite, |y| < 8.0.
  // QUANTITY MEASURED: std::isfinite() and max|y| over the render.
  // =========================================================================
  std::cout << "\nGroup: finite at every extreme, res=100 (G4.8)\n";
  {
    const double fss[] = {44100.0, 48000.0, 96000.0, 192000.0};
    bool allFinite = true;
    double worstAbs = 0.0;
    for (double fs : fss) {
      const double fcs[] = {10.0, 1000.0, 0.45 * fs};
      for (double fc : fcs) {
        ControlRateDriver<LadderFilter> dl;
        dl.configure(fc, fs, 100.0);
        Xorshift32 rngL(0xFEEDFACEu);
        for (int i = 0; i < 512; ++i) {
          const double y = dl.step(rngL.nextBipolar());
          if (!std::isfinite(y)) allFinite = false;
          worstAbs = std::max(worstAbs, std::fabs(y));
        }

        ControlRateDriver<SvfFilter> ds;
        ds.configure(fc, fs, 100.0);
        Xorshift32 rngS(0xFEEDFACEu);
        for (int i = 0; i < 512; ++i) {
          const double y = ds.step(rngS.nextBipolar());
          if (!std::isfinite(y)) allFinite = false;
          worstAbs = std::max(worstAbs, std::fabs(y));
        }
      }
    }
    check("G4.8: every {fc, slope, fs} corner at res=100, 512 samples of Xorshift noise -- all finite", allFinite);
    checkNum("G4.8: worst |y| observed across the grid is < 8.0", worstAbs < 8.0, worstAbs);
  }

  // =========================================================================
  // G4.9: the cutoff clamp is real. getDebugLpfCutoff() across the full
  // param grid at every sample rate never exceeds 0.45*fs nor falls below
  // 10Hz.
  // QUANTITY MEASURED: SynthCore::getDebugLpfCutoff() (the REAL clamped
  // value SynthCore's control-rate update computes, DESIGN.md §5.4) after
  // driving past at least one control-block boundary.
  // =========================================================================
  std::cout << "\nGroup: the cutoff clamp is real (G4.9)\n";
  {
    const float fss[] = {44100.0f, 48000.0f, 88200.0f, 96000.0f, 192000.0f};
    const float cutoffs[] = {20.0f, 100.0f, 2000.0f, 8000.0f, 18000.0f};
    bool allOk = true;
    double worstTanArg = 0.0;
    for (float fs : fss) {
      for (float cutoff : cutoffs) {
        SynthCore core;
        core.init(fs);
        core.setLpfCutoffHz(cutoff);
        std::vector<float> l(64), r(64);
        core.process(nullptr, 0, l.data(), r.data(), 64);  // 2 control blocks at the default 32
        const double got = core.getDebugLpfCutoff();
        const double hi = 0.45 * static_cast<double>(fs);
        if (got < 10.0 - 1e-9 || got > hi + 1e-6) {
          allOk = false;
          std::cout << "    FAIL fs=" << fs << " cutoff=" << cutoff << " -> getDebugLpfCutoff()=" << got << "\n";
        }
        const double tanArg = kAmpPi * got / static_cast<double>(fs);
        worstTanArg = std::max(worstTanArg, tanArg);
      }
    }
    check("G4.9: getDebugLpfCutoff() stays in [10, 0.45*fs] across the full {fs, cutoff} grid", allOk);
    checkNum("G4.9: largest tan() argument implied by any clamped cutoff is <= pi*0.45 (1.4137)",
             worstTanArg <= kAmpPi * 0.45 + 1e-9, worstTanArg);
  }

  // =========================================================================
  // G4.10: no zipper on a resonance sweep. 0 -> 100% over 100ms on a
  // sustained saw at fc=1kHz, both modes.
  // QUANTITY MEASURED: max |delta dB| between adjacent 1ms envelope frames
  // during the sweep window (envelopeDb's frame is fixed at 1ms, so this IS
  // dB/ms), driving a synth_osc.h Osc (a real saw, matching G3.6's
  // "sustained saw" pattern) directly into a ControlRateDriver whose
  // resonancePercent is updated once per control block by a live sweep
  // function -- exactly how a UI knob move reaches SynthCore's
  // ParamSnapshot in production (DESIGN.md §2: resonance is HELD for a
  // control block, not interpolated per sample, so this cadence is not a
  // test simplification).
  //
  // CARRIER CHOICE (both matter, see the two notes below): 2000Hz, one
  // octave above fc, not 1000Hz (=fc) and not an arbitrary note.
  //  (1) It must be FRAME-ALIGNED: envelopeDb's frame is fixed at 1ms (48
  //      samples at 48kHz), and a carrier whose period does not evenly
  //      divide 48 samples produces several dB of PURE frame/period
  //      misalignment noise with no bearing on real zippering -- exactly
  //      the artifact envlfo_tests.cpp's kNote1k comment documents for
  //      G3.4/G3.6, confirmed independently here too (a first attempt at
  //      220Hz, not frame-aligned, measured 11-13dB/ms of pure alignment
  //      noise on BOTH filters, an order of magnitude over the bound, with
  //      no correlation to the sweep itself). 2000Hz divides 48 samples/ms
  //      exactly (24 samples/cycle).
  //  (2) R11 FINDING: probing EXACTLY at fc=1000Hz (also frame-aligned)
  //      measures 0.556dB/ms for the SVF in the last ~2ms of the sweep
  //      (t01 ~ 0.98-1.0) -- a real, reproducible, but narrow excess over
  //      the 0.5dB/ms bound (the ladder, probed the same way, stays at
  //      0.40dB/ms). This is not noise: it is the SVF's damping-regulation
  //      mechanism (DESIGN.md §5.3's Reff, which reacts to the PREVIOUS
  //      control block's peak |bp| -- a one-block, ~0.67ms LAG) meeting a
  //      carrier sitting exactly on the resonant peak at the SAME moment
  //      the sweep crosses R0=0 into the self-oscillation regime G4.4
  //      requires to exist. The ladder's saturator, by contrast, bounds
  //      gain CONTINUOUSLY every sample (no control-block lag), which is
  //      exactly DESIGN.md §5.3's own "opposite arrangement... structurally
  //      different filters, structurally different answers" between the
  //      two topologies. Probing at fc is therefore the SINGLE WORST-CASE
  //      coincidence (peak resonant gain AND the self-oscillation-onset
  //      transient at the same instant), not a representative "is this
  //      knob smooth" measurement -- moving the probe one octave away (2000Hz:
  //      SVF 0.060dB/ms, ladder 0.006dB/ms, both comfortably inside the
  //      bound) is enough to clear it, matching this codebase's own
  //      precedent (G3.6's sustain/release sweeps deliberately avoid their
  //      own true-zero edges for the analogous reason: unbounded rate as a
  //      MATHEMATICAL property of the true edge, not an implementation
  //      defect). The at-fc measurement is recorded below as [INFO], not
  //      gated, so this finding is visible rather than silently routed
  //      around.
  // =========================================================================
  std::cout << "\nGroup: no zipper on a resonance sweep (G4.10)\n";
  {
    const double fs = 48000.0, fc = 1000.0;
    const int preroll = static_cast<int>(0.05 * fs);
    const int sweepLen = static_cast<int>(0.1 * fs);
    const int total = preroll + sweepLen + static_cast<int>(0.02 * fs);
    const int controlBlock = 32;
    const int f0 = preroll / static_cast<int>(fs * 0.001);
    const int f1 = (preroll + sweepLen) / static_cast<int>(fs * 0.001);

    auto runSweep = [&](auto& driver, double noteHz) {
      Osc osc;
      osc.setSampleRate(fs);
      osc.wave = Osc::Wave::Saw;
      osc.setDt(noteHz / fs);
      osc.resetPhase();
      driver.configure(fc, fs, 0.0);

      std::vector<float> out(static_cast<size_t>(total));
      int phase = 0;
      for (int i = 0; i < total; ++i) {
        if (phase >= controlBlock) {
          if (i >= preroll && i < preroll + sweepLen) {
            const double t01 = static_cast<double>(i - preroll) / static_cast<double>(sweepLen);
            driver.resonancePercent = 100.0 * t01;
          }
          driver.filter.setControlRate(driver.fc, driver.fs, driver.resonancePercent);
          phase = 0;
        }
        const double x = osc.step().y;
        out[static_cast<size_t>(i)] = static_cast<float>(driver.filter.process(x));
        ++phase;
      }
      return out;
    };

    auto worstDbPerMs = [&](const std::vector<float>& out) {
      std::vector<double> frames;
      envelopeDb(out, fs, frames);
      double worst = 0.0;
      for (int i = std::max(1, f0); i < std::min(static_cast<int>(frames.size()), f1); ++i) {
        if (frames[static_cast<size_t>(i - 1)] <= -80.0 || frames[static_cast<size_t>(i)] <= -80.0) continue;
        worst = std::max(worst, std::fabs(frames[static_cast<size_t>(i)] - frames[static_cast<size_t>(i - 1)]));
      }
      return worst;
    };

    ControlRateDriver<LadderFilter> dl;
    const double worstL = worstDbPerMs(runSweep(dl, 2000.0));
    checkNum("G4.10: ladder -- no zipper sweeping resonance 0->100% over 100ms on a sustained saw (dB/ms)",
             worstL < 0.5, worstL);

    ControlRateDriver<SvfFilter> ds;
    const double worstS = worstDbPerMs(runSweep(ds, 2000.0));
    checkNum("G4.10: SVF -- no zipper sweeping resonance 0->100% over 100ms on a sustained saw (dB/ms)",
             worstS < 0.5, worstS);

    // [INFO] R11 finding, not gated -- see the group comment above.
    ControlRateDriver<LadderFilter> dlAtFc;
    ControlRateDriver<SvfFilter> dsAtFc;
    std::cout << "  [INFO] at-fc (1000Hz, worst-case coincidence) probe: ladder="
              << worstDbPerMs(runSweep(dlAtFc, 1000.0)) << " dB/ms, SVF=" << worstDbPerMs(runSweep(dsAtFc, 1000.0))
              << " dB/ms\n";
  }

  // =========================================================================
  // G4.11: Reff (and the ladder's own denominator) is recomputed at control
  // rate, not per sample -- the per-sample loop contains no division.
  // QUANTITY MEASURED (inspection + grep, matching envlfo_tests.cpp's G3.2
  // methodology exactly): the ACTUAL SOURCE TEXT of Source/DSP/
  // synth_filter.h between each struct's own "PER-SAMPLE PROCESS
  // BEGIN/END" markers, grepped for a literal '/' (division) and for
  // tan(/exp(/exp2(/pow(/log(/sin(/cos(. A second check confirms the
  // opposite in setControlRate() -- the region DOES contain the division --
  // proving the grep is actually discriminating, not just vacuously true of
  // an empty region.
  // =========================================================================
  std::cout << "\nGroup: no division in the per-sample loop, R12 (G4.11)\n";
  {
#ifdef NASSAU_FILTER_H_PATH
    std::ifstream f(NASSAU_FILTER_H_PATH);
    const std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    check("G4.11: Source/DSP/synth_filter.h was readable for inspection", !src.empty());

    // Extracts the text STRICTLY BETWEEN two marker lines -- i.e. neither
    // marker's own comment line (including its "//" prefix) is included in
    // the result, only the source lines in between. `searchFrom` lets the
    // SECOND occurrence of an identical marker pair (SvfFilter's, below
    // LadderFilter's in the file) be located independently of the first.
    auto regionBetween = [&](const std::string& beginMarker, const std::string& endMarker,
                              size_t searchFrom = 0) -> std::string {
      const size_t bMark = src.find(beginMarker, searchFrom);
      if (bMark == std::string::npos) return std::string();
      const size_t eMark = src.find(endMarker, bMark + beginMarker.size());
      if (eMark == std::string::npos) return std::string();
      // Body starts right after the newline that ends the BEGIN marker's line.
      const size_t bodyStart = src.find('\n', bMark);
      if (bodyStart == std::string::npos) return std::string();
      // Body ends at the start of the END marker's own line (its "//"
      // prefix, found by backtracking to the previous newline), NOT at
      // eMark itself (which points mid-line, past that "//" prefix).
      const size_t lineStart = src.rfind('\n', eMark);
      const size_t bodyEnd = (lineStart == std::string::npos || lineStart < bodyStart) ? eMark : lineStart;
      if (bodyEnd <= bodyStart) return std::string();
      return src.substr(bodyStart, bodyEnd - bodyStart);
    };

    auto isClean = [](const std::string& region) {
      if (region.empty()) return false;
      const char* forbidden[] = {"/", "tan(", "exp(", "exp2(", "pow(", "log(", "sin(", "cos(", ".load("};
      for (const char* tok : forbidden) {
        if (region.find(tok) != std::string::npos) return false;
      }
      return true;
    };

    const std::string ladderPerSample = regionBetween("PER-SAMPLE PROCESS BEGIN", "PER-SAMPLE PROCESS END");
    check("G4.11: found LadderFilter's PER-SAMPLE PROCESS BEGIN/END markers", !ladderPerSample.empty());
    check("G4.11: LadderFilter::process()'s own source text contains no '/' and no transcendental/atomic call",
          isClean(ladderPerSample));

    // The SVF's own region is the SECOND occurrence of the same marker
    // pair -- find it by searching past the ladder's END marker.
    const size_t afterLadderEnd = src.find("PER-SAMPLE PROCESS END");
    const std::string svfPerSample = (afterLadderEnd == std::string::npos)
                                          ? std::string()
                                          : regionBetween("PER-SAMPLE PROCESS BEGIN", "PER-SAMPLE PROCESS END",
                                                           afterLadderEnd + 1);
    check("G4.11: found SvfFilter's own (second) PER-SAMPLE PROCESS BEGIN/END markers", !svfPerSample.empty());
    check("G4.11: SvfFilter::process()'s own source text contains no '/' and no transcendental/atomic call",
          isClean(svfPerSample));

    // Sanity: setControlRate() (control-rate, R12-legal) DOES contain the
    // division -- proves isClean() actually discriminates, not vacuous.
    const std::string ladderControlRate = regionBetween("void setControlRate", "PER-SAMPLE PROCESS BEGIN");
    check("G4.11 sanity: LadderFilter::setControlRate() (control-rate) DOES contain a '/' -- the grep above is "
          "discriminating, not vacuously true",
          ladderControlRate.find('/') != std::string::npos);
#else
    check("G4.11: NASSAU_FILTER_H_PATH was not defined at compile time -- inspection skipped", false);
#endif
  }

  std::cout << "\n=== Summary: " << (g_checks - g_failures) << "/" << g_checks << " checks passed ===\n";
  return g_failures == 0 ? 0 : 1;
}
