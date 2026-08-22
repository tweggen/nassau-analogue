#pragma once

// test_util.h — shared test harness helpers (docs/GATES.md "Shared test
// harness"). Every gate from G1 onward drives its DSP with real audio and
// MEASURES the result (R6) using the functions declared here, instead of
// each test file reinventing RMS/magnitude/harmonic measurement. This file
// is NOT part of Source/DSP/ — it is test-only code, so it is free of R2's
// include restriction.
//
// Started from nassau-zermatt/Tests/test_util.h (docs/GATES.md's own
// instruction), with two changes made in this gate:
//
//  1. nonHarmonicEnergyDb's harmonic-subtraction cap is REMOVED (was
//     std::min(40, ...) in Zermatt). See that function's own comment for why
//     this is load-bearing for every G2 alias AC, not a style preference.
//  2. The SynthCore-dependent helpers docs/GATES.md lists for later gates
//     (magDbAt(SynthCore&, ...), seqHoldNote, seqChord) are deliberately NOT
//     added here. G1's SynthCore is still G0's silent scaffold (no
//     oscillator/filter/voice section exists yet), so there is nothing real
//     for those helpers to drive or for a G1 test to verify against; adding
//     them now would be untested code guessing at an interface later gates
//     (G2/G3/G4/G7) are the ones actually positioned to specify and exercise.
//     They will be added by the first gate that needs them. measuredF0 and
//     minus3dBPoint ARE added below: both operate on a plain
//     std::vector<float> or a generic ProcessBlockFn, need no SynthCore, and
//     are self-checked in Tests/dsp_tests.cpp against known signals.
//
// Determinism (R8): no rand()/time(); all randomness in this file goes
// through Xorshift32 (also the production RNG, Source/DSP/synth_dsp.h),
// fixed seeds only.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// ---- Constants -------------------------------------------------------------

constexpr double kTestPi = 3.14159265358979323846;  // [dsp]

// ---- rms --------------------------------------------------------------------
// RMS of v[from, to).
inline double rms(const std::vector<float>& v, int from, int to) {
  double acc = 0.0;
  for (int i = from; i < to; ++i) {
    const double s = static_cast<double>(v[static_cast<size_t>(i)]);
    acc += s * s;
  }
  const int n = to - from;
  return n > 0 ? std::sqrt(acc / static_cast<double>(n)) : 0.0;
}

// ---- magDb ------------------------------------------------------------------
// Drive a steady sine of `freq` Hz at amplitude `amp` through `processBlock`
// (a callable `void(const float* in, float* out, int n)`, processing one
// block of a fixed input/output pair — e.g. a lambda wrapping a TptOnePole's
// or PinkFilter's per-sample process()), in realistic-sized blocks, and
// return the steady-state magnitude response in dB (output RMS / input RMS,
// measured after a warm-up region so filter transients have decayed).
template <typename ProcessBlockFn>
inline double magDb(ProcessBlockFn&& processBlock, double freq, double fs, double amp = 0.25) {
  const int kWarm = 16384;  // discard: let filter state settle
  const int kMeas = 49152;  // measured region (many cycles -> low RMS bias)
  const int N = kWarm + kMeas;

  std::vector<float> in(static_cast<size_t>(N)), out(static_cast<size_t>(N));
  for (int i = 0; i < N; ++i) {
    in[static_cast<size_t>(i)] =
        static_cast<float>(amp * std::sin(2.0 * kTestPi * freq * i / fs));
  }

  const int block = 512;
  for (int off = 0; off < N; off += block) {
    const int n = std::min(block, N - off);
    processBlock(in.data() + off, out.data() + off, n);
  }

  const double ri = rms(in, kWarm, N);
  const double ro = rms(out, kWarm, N);
  return 20.0 * std::log10(ro / std::max(ri, 1e-300));
}

// ---- Goertzel (single-bin DFT), internal to this file ----------------------
// Amplitude of the `freq` Hz component of v[from, to). Accurate (no
// spectral-leakage bias) when (to-from) spans a whole number of periods of
// `freq` at `fs` — thdPercent/harmonicDb/nonHarmonicEnergyDb below always
// trim their analysis window to a whole number of f0 periods first.
inline double goertzelMag(const std::vector<float>& v, int from, int to, double freq, double fs) {
  const int n = to - from;
  if (n <= 0) return 0.0;
  const double w = 2.0 * kTestPi * freq / fs;
  const double cw = std::cos(w);
  const double sw = std::sin(w);
  const double coeff = 2.0 * cw;
  double s1 = 0.0, s2 = 0.0;
  for (int i = from; i < to; ++i) {
    const double s0 = static_cast<double>(v[static_cast<size_t>(i)]) + coeff * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  const double re = (s1 - s2 * cw) * (2.0 / n);
  const double im = (s2 * sw) * (2.0 / n);
  return std::sqrt(re * re + im * im);
}

// Trim [from, to) down to the largest exact whole number of f0 periods that
// fits, in place, so the Goertzel bins above see no partial-cycle leakage.
inline void trimToWholePeriods(int from, int& to, double f0, double fs) {
  if (f0 <= 0.0) return;
  const double periodSamples = fs / f0;
  const int n = to - from;
  const int periods = static_cast<int>(static_cast<double>(n) / periodSamples);
  to = from + static_cast<int>(periods * periodSamples);
}

// ---- thdPercent ---------------------------------------------------------
// Total Harmonic Distortion of v as a percentage of the f0 fundamental:
// 100 * sqrt(sum(mag(h)^2 for h = 2..maxHarmonic)) / mag(f0).
// Harmonics are summed up to the highest one still comfortably under
// Nyquist (0.45*fs), capped at the 20th. (Unlike nonHarmonicEnergyDb below,
// capping here is fine: THD is defined as a FIXED, small set of low-order
// harmonics, not "everything that isn't the fundamental".)
inline double thdPercent(const std::vector<float>& v, double f0, double fs) {
  int from = 0;
  int to = static_cast<int>(v.size());
  trimToWholePeriods(from, to, f0, fs);
  if (to <= from || f0 <= 0.0) return 0.0;

  const double fund = goertzelMag(v, from, to, f0, fs);
  if (fund < 1e-12) return 0.0;

  const int maxHarmonic = std::max(2, std::min(20, static_cast<int>((0.45 * fs) / f0)));
  double sumSq = 0.0;
  for (int h = 2; h <= maxHarmonic; ++h) {
    const double m = goertzelMag(v, from, to, f0 * h, fs);
    sumSq += m * m;
  }
  return 100.0 * std::sqrt(sumSq) / fund;
}

// ---- harmonicDb -----------------------------------------------------------
// Level of the n-th harmonic of f0 (n=1 is the fundamental) in v, in dBFS
// (20*log10 of its Goertzel amplitude, amplitude 1.0 == 0 dBFS).
inline double harmonicDb(const std::vector<float>& v, double f0, int n, double fs) {
  int from = 0;
  int to = static_cast<int>(v.size());
  trimToWholePeriods(from, to, f0, fs);
  if (to <= from) return -300.0;
  const double mag = goertzelMag(v, from, to, f0 * n, fs);
  return 20.0 * std::log10(std::max(mag, 1e-15));
}

// ---- nonHarmonicEnergyDb ---------------------------------------------------
// Signal power in v NOT accounted for by f0's harmonic series (aliasing,
// intermodulation, noise floor, ...), in dB (10*log10 of the residual mean-
// square, i.e. an amplitude-equivalent dBFS figure — 20*log10(residualRMS)).
//
// *** MODIFIED from nassau-zermatt/Tests/test_util.h (docs/GATES.md, "Two
// things about nonHarmonicEnergyDb that invalidate every alias AC if
// missed", point (1)): Zermatt's version caps harmonic subtraction at
// std::min(40, ...) harmonics, which is fine for measuring an amp's alias
// suppression (40 harmonics is plenty there). Here it is fatal: a PERFECT
// 110 Hz saw at 48 kHz has 218 real harmonics below Nyquist. Stopping at 40
// leaves 178 real harmonics counted as "non-harmonic residual", which makes
// a flawless oscillator read about -17 dB — every G2 alias AC would then be
// measuring the oscillator's own harmonic series, not aliasing. This version
// subtracts EVERY harmonic up to Nyquist instead. ***
//
// The second harness trap noted in docs/GATES.md — probe frequencies must be
// non-commensurate with fs (round numbers like 110/1000/4000 Hz at
// fs=48000 put every alias image exactly on a harmonic bin, which then gets
// subtracted right along with the real harmonics and returns the numerical
// floor regardless of how bad the oscillator is) — is a caller
// responsibility, not something this function can detect or fix; callers
// use 109/997/3989 Hz, never round frequencies.
inline double nonHarmonicEnergyDb(const std::vector<float>& v, double f0, double fs) {
  int from = 0;
  int to = static_cast<int>(v.size());
  trimToWholePeriods(from, to, f0, fs);
  if (to <= from || f0 <= 0.0) return -300.0;
  const int n = to - from;

  double totalPower = 0.0;
  for (int i = from; i < to; ++i) {
    const double s = static_cast<double>(v[static_cast<size_t>(i)]);
    totalPower += s * s;
  }
  totalPower /= n;  // mean square

  // Every harmonic strictly below Nyquist (h=1 is the fundamental itself).
  const int maxHarmonic = std::max(1, static_cast<int>((0.5 * fs) / f0) - 1);
  double harmonicPower = 0.0;
  for (int h = 1; h <= maxHarmonic; ++h) {
    const double m = goertzelMag(v, from, to, f0 * h, fs);
    harmonicPower += (m * m) / 2.0;  // sinusoid amplitude -> mean-square power
  }

  const double residual = std::max(totalPower - harmonicPower, 1e-30);
  return 10.0 * std::log10(residual);
}

// ---- aliasFloorDb -----------------------------------------------------------
// Non-harmonic (alias) energy RELATIVE TO THE FUNDAMENTAL, in dB. Negative
// numbers; more negative is better.
//
// USE THIS, NOT nonHarmonicEnergyDb, FOR ANY ALIAS ACCEPTANCE CRITERION.
// GATES.md's alias ACs are all worded "non-harmonic energy below the
// FUNDAMENTAL by >= X dB", and nonHarmonicEnergyDb returns an ABSOLUTE
// dBFS-equivalent. The gap between them is the fundamental's own level, which
// is waveform-dependent and large:
//
//     +/-1 saw    fundamental amplitude 2/pi   -> -6.93 dB   power
//     50% pulse   fundamental amplitude 4/pi   -> -0.91 dB   power
//
// So an absolute measurement flatters a saw by 6.9 dB and a square by only
// 0.9 dB. G2 originally compared absolute readings against relative bounds and
// every AC "passed" -- including one that a correct oscillator actually misses.
// Measuring the wrong quantity is not caught by the test going green.
inline double aliasFloorDb(const std::vector<float>& v, double f0, double fs) {
  int from = 0;
  int to = static_cast<int>(v.size());
  trimToWholePeriods(from, to, f0, fs);
  if (to <= from || f0 <= 0.0) return 0.0;
  const double absDb = nonHarmonicEnergyDb(v, f0, fs);
  const double mag = goertzelMag(v, from, to, f0, fs);
  if (mag <= 0.0) return 0.0;
  const double fundDb = 10.0 * std::log10(mag * mag / 2.0);  // sinusoid amp -> mean-square
  return absDb - fundDb;
}

// ---- aliasFloorVsRmsDb ------------------------------------------------------
// Non-harmonic (alias) energy relative to the signal's TOTAL RMS, in dB.
//
// USE THIS, NOT aliasFloorDb, FOR HARD-SYNC WAVEFORMS.
// A synced slave's spectrum is not centred on the master fundamental, and at
// INTEGER sync ratios the master fundamental is not merely weak -- it is
// absent. Measured on this implementation at f1 = 219 Hz:
//
//     ratio 1.0   fundamental  -6.93 dB     vs-fund -36.21   vs-RMS -38.32
//     ratio 2.0   fundamental -244.72 dB    vs-fund +204.45  vs-RMS -35.39
//     ratio 3.7   fundamental -21.77 dB     vs-fund  -2.95   vs-RMS -19.56
//     ratio 4.0   fundamental -269.52 dB    vs-fund +231.99  vs-RMS -32.54
//
// Dividing by a -245 dB "fundamental" yields +204 dB of "alias", which is not a
// harsh measurement, it is a meaningless one. Total RMS is well behaved at
// every ratio and is what the sync AC actually intends.
inline double aliasFloorVsRmsDb(const std::vector<float>& v, double f0, double fs) {
  int from = 0;
  int to = static_cast<int>(v.size());
  trimToWholePeriods(from, to, f0, fs);
  if (to <= from || f0 <= 0.0) return 0.0;
  const double absDb = nonHarmonicEnergyDb(v, f0, fs);
  double tot = 0.0;
  for (int i = from; i < to; ++i) {
    const double x = static_cast<double>(v[static_cast<size_t>(i)]);
    tot += x * x;
  }
  tot /= static_cast<double>(to - from);
  if (tot <= 0.0) return 0.0;
  return absDb - 10.0 * std::log10(tot);
}

// ---- envelopeDb -------------------------------------------------------------
// RMS envelope of v in 1 ms frames, in dBFS. out[k] is the level of frame k
// (samples [k*frame, (k+1)*frame)); the last (partial) frame is included.
inline void envelopeDb(const std::vector<float>& v, double fs, std::vector<double>& out) {
  out.clear();
  const int frame = std::max(1, static_cast<int>(std::lround(fs * 0.001)));  // 1 ms
  for (size_t start = 0; start < v.size(); start += static_cast<size_t>(frame)) {
    const size_t end = std::min(v.size(), start + static_cast<size_t>(frame));
    double acc = 0.0;
    for (size_t i = start; i < end; ++i) {
      const double s = static_cast<double>(v[i]);
      acc += s * s;
    }
    const double r = std::sqrt(acc / static_cast<double>(end - start));
    out.push_back(20.0 * std::log10(std::max(r, 1e-12)));
  }
}

// ---- measuredF0 -------------------------------------------------------------
// Sub-cent-accurate fundamental frequency of v: linear-interpolated rising
// zero-crossing timestamps, first-to-last crossing divided by the whole
// number of periods spanned. Needs no FFT and no fixed period-length
// assumption, so it works directly on raw oscillator output (later gates'
// use case, DESIGN.md §2's "measuredF0 must be sub-cent accurate" — a bare
// FFT bin peak does not reach that, per docs/GATES.md's own warning).
// Self-checked in Tests/dsp_tests.cpp against a known-frequency sine.
inline double measuredF0(const std::vector<float>& v, double fs) {
  std::vector<double> crossings;
  for (size_t i = 1; i < v.size(); ++i) {
    const double y0 = static_cast<double>(v[i - 1]);
    const double y1 = static_cast<double>(v[i]);
    if (y0 < 0.0 && y1 >= 0.0) {
      const double frac = (y1 == y0) ? 0.0 : (-y0) / (y1 - y0);
      crossings.push_back(static_cast<double>(i - 1) + frac);
    }
  }
  if (crossings.size() < 2) return 0.0;
  const double totalSamples = crossings.back() - crossings.front();
  const double numPeriods = static_cast<double>(crossings.size() - 1);
  if (totalSamples <= 0.0) return 0.0;
  return numPeriods * fs / totalSamples;
}

// ---- minus3dBPoint -----------------------------------------------------------
// Log-domain bisection for the frequency in [loHz, hiHz] where
// `processBlock`'s magnitude response (measured via magDb, relative to the
// level at `refHz`) crosses -3 dB. `processBlock` has magDb's own signature:
// `void(const float* in, float* out, int n)`. Assumes the response is
// monotone between loHz and hiHz (callers choose a bracket that holds — this
// is true of every filter DESIGN.md defines). Used by later gates (G4.3,
// G5, G6.3) to locate a filter's ACTUAL corner, which DESIGN.md repeatedly
// warns is not at the nominal cutoff parameter for any structure in this
// plugin. Self-checked in Tests/dsp_tests.cpp against TptOnePole.
template <typename ProcessBlockFn>
inline double minus3dBPoint(ProcessBlockFn&& processBlock, double fs, double refHz, double loHz,
                             double hiHz, int iterations = 40) {
  const double refDb = magDb(processBlock, refHz, fs);
  double lo = loHz, hi = hiHz;
  for (int i = 0; i < iterations; ++i) {
    const double mid = std::sqrt(lo * hi);  // geometric midpoint (log-domain bisection)
    const double db = magDb(processBlock, mid, fs) - refDb;
    if (db > -3.0) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return std::sqrt(lo * hi);
}
