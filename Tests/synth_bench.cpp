// NassauAnalogue G11 throughput benchmark (docs/GATES.md G11.4/G11.5/G11.6).
//
// NOT a ctest -- a manual benchmark, same reasoning nassau-zermatt's
// Tests/amp_bench.cpp gives (and nassau-eq's Tests/eq_bench.cpp before it): a
// benchmark inside ctest is load-sensitive (a wall-clock number is a bad
// pass/fail signal -- Zermatt's own G6.9 note measured 13.6-21.7x under
// concurrent builds vs 19.5-20.2x idle for the SAME binary).
//
// Five configs, all at fs = 48 kHz (DESIGN.md §12's own budget scenario),
// best-of-N trials each, matching G11.4's exact list:
//   idle        -- SynthCore initialised, NOT ONE NoteEvent ever sent. Every
//                  one of the 18 physical+fade slots stays Idle, so the
//                  per-voice audio-rate work is skipped entirely by the
//                  DESIGN.md §10.7 [PERF-7] silent-voice skip -- this is what
//                  G11.6 measures against the 8-voice-active cost.
//   1 voice     -- one held note, mono, Poly mode.
//   8 voices mono   -- 8 SIMULTANEOUS held notes (an 8-note cluster spanning
//                  most of the keyboard, so no two collide into the same
//                  physical slot), mono mode, kPolyphony=8 -- exactly the
//                  budget scenario DESIGN.md §12/docs/GATES.md G11.5 names.
//   8 voices stereo -- the SAME 8-note cluster, kStereoMode=on (real
//                  detune/spread, so chain 1 genuinely renders) -- DESIGN.md
//                  §12's "stereo <= 2.0x mono" ceiling.
//   16 voices unison -- kVoiceMode=Unison, kPolyphony=16, ONE held note.
//                  DESIGN.md §10.5: "all kPolyphony voices play the held
//                  note... this is the one mode that can approach the CPU
//                  budget, and G11 measures it" -- literally this config.
//
// Measurement hygiene (docs/GATES.md's own "what this project has learned"
// list, applied point by point):
//   - "8 voices" means 8 voices ACTUALLY SOUNDING, not 8 allocated and 7
//     silently skipped: every held-note config below asserts
//     getDebugActiveVoiceCount() against its expected count once, right
//     after warm-up, and prints a loud MISMATCH line (not a silent pass) if
//     it disagrees -- this is exactly the trap the gate brief calls out
//     ("make sure '8 voices' means 8 voices actually sounding").
//   - Notes are held with real DESIGN.md-default sustain levels (ENV-A
//     80%, ENV-F 30%, both nonzero) and NO note-off is ever sent during the
//     timed region, so every voice's peakPrevBlock genuinely stays above the
//     -100dBFS silent-voice-skip threshold for the whole measurement -- the
//     "8 voices" config cannot quietly decay into a cheaper "0 voices"
//     config partway through a trial.
//   - A 256-block warm-up (matching amp_bench.cpp's own convention) runs
//     BEFORE timing starts, so envelopes have left their attack phase, the
//     LFO has ramped past any delay, and every page of SynthCore's fixed-
//     size state has been touched at least once -- the timed region
//     measures steady-state throughput, not attack transients or first-
//     touch page faults.
//   - `volatile sink` accumulates a sample from every processed block, so
//     the optimizer cannot prove the render is dead and elide it.
//   - No NoteEvents are allocated/generated inside the timed loop itself
//     (process() is called with events=nullptr, numEvents=0 every timed
//     call) -- nothing but the DSP render is being timed.
//
// Determinism (R8): not load-bearing for a benchmark (nothing here asserts
// pass/fail), but every input this file drives SynthCore with is fully
// deterministic (fixed note numbers/velocities, no time()/rand()) so runs
// before and after an optimization are driven by literally the same work.

#include "synth_core.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double nanos(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration_cast<std::chrono::duration<double, std::nano>>(b - a).count();
}

struct Config {
  const char* name;
  // Notes to strike at t=0 before warm-up begins (empty for "idle").
  std::vector<int> notes;
  SynthCore::Polyphony polyphony;
  SynthCore::VoiceMode voiceMode;
  bool stereoMode;
  int expectedActiveVoices;  // getDebugActiveVoiceCount() sanity check, post warm-up
};

void configureCore(SynthCore& core, const Config& cfg, float fs) {
  // DESIGN.md §11 real defaults throughout except the axes this config is
  // actually varying -- same convention as Tests/synth_golden.cpp's G11Params.
  core.setMasterVolumeDb(-6.0f);
  core.setOutputClip(true);
  core.setOsc1Wave(SynthCore::Wave::Saw);
  core.setOsc1Octave(SynthCore::Octave::Ft8);
  core.setOsc1LevelPercent(100.0f);
  core.setOsc2Wave(SynthCore::Wave::Saw);
  core.setOsc2Octave(SynthCore::Octave::Ft8);
  core.setOsc2FineCents(-7.0f);
  core.setOsc2LevelPercent(80.0f);
  core.setSubOctave(SynthCore::SubOctave::Minus1);
  core.setSubLevelPercent(0.0f);
  core.setNoiseColor(SynthCore::NoiseColor::White);
  core.setNoiseLevelPercent(0.0f);
  core.setEnvFAttackMs(2.0f);
  core.setEnvFDecayMs(400.0f);
  core.setEnvFSustainPercent(30.0f);   // nonzero: silent-voice skip never fires while held
  core.setEnvFReleaseMs(300.0f);
  core.setEnvAAttackMs(2.0f);
  core.setEnvADecayMs(800.0f);
  core.setEnvASustainPercent(80.0f);   // nonzero: same reason
  core.setEnvAReleaseMs(250.0f);
  core.setLfoWave(SynthCore::LfoWave::Tri);
  core.setLfoRateHz(5.0f);
  core.setLfoDelayMs(0.0f);
  core.setLpfSlope(SynthCore::LpfSlope::Db24);
  core.setLpfCutoffHz(2000.0f);
  core.setLpfResonancePercent(20.0f);
  core.setLpfEnvAmountPercent(40.0f);
  core.setLpfKeyFollowPercent(50.0f);
  core.setDrivePercent(15.0f);
  core.setHpfSlope(SynthCore::HpfSlope::Db12);
  core.setHpfCutoffHz(20.0f);          // bypassed -- the ordinary "no HPF work" case
  core.setPolyphony(cfg.polyphony);
  core.setVoiceMode(cfg.voiceMode);
  core.setGlideTimeMs(0.0f);
  core.setBendRangeSemitones(2);
  core.setVelToVcaPercent(40.0f);
  core.setVelToFilterPercent(20.0f);
  core.setStereoMode(cfg.stereoMode);
  core.setStereoDetuneCents(cfg.stereoMode ? 10.0f : 6.0f);
  core.setStereoSpreadPercent(cfg.stereoMode ? 80.0f : 70.0f);
  core.init(fs);
}

void runOne(const Config& cfg, long long totalSamples, int trials, int block) {
  const float fs = 48000.0f;

  std::vector<float> outL(static_cast<size_t>(block)), outR(static_cast<size_t>(block));

  SynthCore core;
  configureCore(core, cfg, fs);

  // Strike every note in the config at t=0, one process() call.
  std::vector<NoteEvent> onEvents;
  onEvents.reserve(cfg.notes.size());
  for (int note : cfg.notes) onEvents.push_back({0, NoteEvent::NoteOn, note, 0.9f});
  core.process(onEvents.empty() ? nullptr : onEvents.data(), static_cast<int>(onEvents.size()), outL.data(),
               outR.data(), block);

  // Warm-up: 256 further blocks with no new events, letting envelopes clear
  // attack/decay into sustain, the LFO clear any delay, and every page of
  // fixed-size state get first-touched -- matches amp_bench.cpp's own
  // 256-block convention.
  for (int w = 0; w < 256; ++w) core.process(nullptr, 0, outL.data(), outR.data(), block);

  const int activeVoices = core.getDebugActiveVoiceCount();
  if (activeVoices != cfg.expectedActiveVoices) {
    std::printf(
        "    *** MISMATCH: %s expected %d actively-sounding voices post warm-up, measured %d "
        "(getDebugActiveVoiceCount()) -- this config is NOT measuring what its name claims. ***\n",
        cfg.name, cfg.expectedActiveVoices, activeVoices);
  }

  volatile float sink = 0.0f;
  const long long blocks = totalSamples / block;
  const long long samples = blocks * block;

  double best = 1e300;
  for (int t = 0; t < trials; ++t) {
    auto t0 = Clock::now();
    for (long long b = 0; b < blocks; ++b) {
      core.process(nullptr, 0, outL.data(), outR.data(), block);
      sink += outL[0] + outR[static_cast<size_t>(block - 1)];
    }
    auto t1 = Clock::now();
    const double ns = nanos(t0, t1);
    if (ns < best) best = ns;
  }
  (void)sink;

  const double nsPerSample = best / static_cast<double>(samples);
  const double msamplesPerSec = static_cast<double>(samples) / (best / 1e9) / 1e6;
  const double xRealtime = (1e9 / nsPerSample) / static_cast<double>(fs);

  std::printf(
      "%-18s voices=%-3d best of %d:  %9.3f ns/sample  |  %7.2f Msamp/s  |  %9.1fx realtime@48k\n",
      cfg.name, activeVoices, trials, nsPerSample, msamplesPerSec, xRealtime);
}

}  // namespace

int main(int argc, char** argv) {
  long long totalSamples = 4LL * 1024 * 1024;
  if (argc > 1) totalSamples = atoll(argv[1]);
  int trials = 7;
  if (argc > 2) trials = atoi(argv[2]);
  const int block = 512;

  std::printf("NassauAnalogue G11 benchmark -- fs=48kHz, block=%d, %lld samples, best of %d\n\n", block,
              totalSamples, trials);

  // An 8-note cluster spanning most of the keyboard (never landing two notes
  // on the same MIDI note number, so all 8 genuinely allocate distinct
  // physical voices under kPolyphony=8 rather than one retriggering
  // another).
  const std::vector<int> eightNoteCluster = {36, 43, 48, 52, 55, 60, 64, 67};

  const Config configs[] = {
      {"idle", {}, SynthCore::Polyphony::Eight, SynthCore::VoiceMode::Poly, false, 0},
      {"1 voice", {60}, SynthCore::Polyphony::Eight, SynthCore::VoiceMode::Poly, false, 1},
      {"8 voices mono", eightNoteCluster, SynthCore::Polyphony::Eight, SynthCore::VoiceMode::Poly, false, 8},
      {"8 voices stereo", eightNoteCluster, SynthCore::Polyphony::Eight, SynthCore::VoiceMode::Poly, true, 8},
      {"16 voices unison", {60}, SynthCore::Polyphony::Sixteen, SynthCore::VoiceMode::Unison, false, 16},
  };

  for (const Config& cfg : configs) runOne(cfg, totalSamples, trials, block);

  return 0;
}
