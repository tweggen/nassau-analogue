// NassauAnalogue golden-reference parity harness (docs/GATES.md G5.7).
//
// Purpose: G5 is "the sound-defining core" gate -- the LPF is now genuinely
// wired into the per-voice audio path (mixer -> DC block -> LPF -> VCA,
// DESIGN.md §1), with cutoff modulation (ENV-F/key-follow/LFO, DESIGN.md
// §5.4) and the 20ms slope crossfade (§5.1) both landed. From here on, G6
// (drive/HPF/Poly-Mod/output stage), G7 (voice allocation) and G8 (stereo)
// all touch this exact signal path again, so a frozen reference of what it
// sounds like RIGHT NOW is what proves those later gates didn't silently
// change it. This file captures that reference and verifies against it.
//
// Modelled on nassau-zermatt/Tests/amp_golden.cpp (docs/GATES.md's own
// instruction): same generate|verify CLI, same fixture layout ([magic]
// [version][count][floats]), same odd-block-size processing to stress
// cross-block state handling. Unlike AmpCore (an audio-IN/audio-OUT
// effect), SynthCore is a pure generator (NoteEvent in, audio out,
// DESIGN.md §10.1) -- so a "Case" here bundles its OWN note-event sequence
// rather than pairing fixed params with a shared library of test signals.
//
//   synth_golden generate [path]
//   synth_golden verify   [path]
//
// Determinism (R8/R13): no time(), no rand(); the only randomness anywhere
// in SynthCore is Xorshift32, fixed-seeded per voice/LFO in init()/reset()
// (already proven by G0/G3's own determinism ACs) -- so two `generate` runs
// on an unmodified tree must byte-for-byte match, and this fixture, once
// captured, is checked in and never silently regenerated (regenerating it
// is a deliberate act: rerun `generate`, review the diff, recommit).

#include "synth_core.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef GOLDEN_FIXTURE_PATH_G5
#define GOLDEN_FIXTURE_PATH_G5 "fixtures/golden_g5.bin"
#endif

namespace {

// ---- Per-case parameter set --------------------------------------------
// Every SynthCore param 0-37 landed through G5 (DESIGN.md §11), defaulted
// to the §11 default column so a Case only overrides what it is actually
// testing. Params 38+ (G6 onward) do not exist yet and are not set here --
// SynthCore's own setters for them are no-ops on this gate's audible path
// (nothing downstream of the LPF reads them until G6 wires drive/HPF/
// Poly-Mod in), so leaving them at SynthCore's own atomic defaults is
// correct, not an oversight.
struct SynthParams {
  float masterVolumeDb = -6.0f;
  bool outputClip = true;
  SynthCore::Wave osc1Wave = SynthCore::Wave::Saw;
  SynthCore::Octave osc1Octave = SynthCore::Octave::Ft8;
  float osc1FineCents = 0.0f;
  float osc1PwPercent = 50.0f;
  float osc1LevelPercent = 100.0f;
  SynthCore::Wave osc2Wave = SynthCore::Wave::Saw;
  SynthCore::Octave osc2Octave = SynthCore::Octave::Ft8;
  int osc2Semi = 0;
  float osc2FineCents = -7.0f;
  float osc2PwPercent = 50.0f;
  float osc2LevelPercent = 80.0f;
  bool osc2Sync = false;
  bool osc2KeyTrack = true;
  SynthCore::SubOctave subOctave = SynthCore::SubOctave::Minus1;
  float subLevelPercent = 0.0f;
  SynthCore::NoiseColor noiseColor = SynthCore::NoiseColor::White;
  float noiseLevelPercent = 0.0f;
  float envFAttackMs = 2.0f;
  float envFDecayMs = 400.0f;
  float envFSustainPercent = 30.0f;
  float envFReleaseMs = 300.0f;
  float envAAttackMs = 2.0f;
  float envADecayMs = 800.0f;
  float envASustainPercent = 80.0f;
  float envAReleaseMs = 250.0f;
  SynthCore::LfoWave lfoWave = SynthCore::LfoWave::Tri;
  float lfoRateHz = 5.0f;
  float lfoDelayMs = 0.0f;
  float lfoPitchAmountPercent = 0.0f;
  float lfoPwmAmountPercent = 0.0f;
  SynthCore::LpfSlope lpfSlope = SynthCore::LpfSlope::Db24;
  float lpfCutoffHz = 2000.0f;
  float lpfResonancePercent = 20.0f;
  float lpfEnvAmountPercent = 40.0f;
  float lpfKeyFollowPercent = 50.0f;
  float lpfLfoAmountPercent = 0.0f;
};

void configure(SynthCore& core, const SynthParams& p) {
  core.setMasterVolumeDb(p.masterVolumeDb);
  core.setOutputClip(p.outputClip);
  core.setOsc1Wave(p.osc1Wave);
  core.setOsc1Octave(p.osc1Octave);
  core.setOsc1FineCents(p.osc1FineCents);
  core.setOsc1PwPercent(p.osc1PwPercent);
  core.setOsc1LevelPercent(p.osc1LevelPercent);
  core.setOsc2Wave(p.osc2Wave);
  core.setOsc2Octave(p.osc2Octave);
  core.setOsc2Semi(p.osc2Semi);
  core.setOsc2FineCents(p.osc2FineCents);
  core.setOsc2PwPercent(p.osc2PwPercent);
  core.setOsc2LevelPercent(p.osc2LevelPercent);
  core.setOsc2Sync(p.osc2Sync);
  core.setOsc2KeyTrack(p.osc2KeyTrack);
  core.setSubOctave(p.subOctave);
  core.setSubLevelPercent(p.subLevelPercent);
  core.setNoiseColor(p.noiseColor);
  core.setNoiseLevelPercent(p.noiseLevelPercent);
  core.setEnvFAttackMs(p.envFAttackMs);
  core.setEnvFDecayMs(p.envFDecayMs);
  core.setEnvFSustainPercent(p.envFSustainPercent);
  core.setEnvFReleaseMs(p.envFReleaseMs);
  core.setEnvAAttackMs(p.envAAttackMs);
  core.setEnvADecayMs(p.envADecayMs);
  core.setEnvASustainPercent(p.envASustainPercent);
  core.setEnvAReleaseMs(p.envAReleaseMs);
  core.setLfoWave(p.lfoWave);
  core.setLfoRateHz(p.lfoRateHz);
  core.setLfoDelayMs(p.lfoDelayMs);
  core.setLfoPitchAmountPercent(p.lfoPitchAmountPercent);
  core.setLfoPwmAmountPercent(p.lfoPwmAmountPercent);
  core.setLpfSlope(p.lpfSlope);
  core.setLpfCutoffHz(p.lpfCutoffHz);
  core.setLpfResonancePercent(p.lpfResonancePercent);
  core.setLpfEnvAmountPercent(p.lpfEnvAmountPercent);
  core.setLpfKeyFollowPercent(p.lpfKeyFollowPercent);
  core.setLpfLfoAmountPercent(p.lpfLfoAmountPercent);
  // Params set BEFORE init(): init()/reset() is what snaps every per-voice
  // filter/envelope/oscillator state to a deterministic start (R13); a
  // case's own init() call below happens AFTER configure() has already set
  // every atomic once, matching this project's own established ordering
  // (nassau-zermatt/Tests/amp_golden.cpp's configure()-then-init() comment).
}

// A live mid-render event: at `sampleOffset`, flip kLpfSlope to `toSlope`.
// Used by the crossfade cases (G5.4) to exercise the slope-switch path a
// plain NoteEvent sequence cannot express (kLpfSlope is a plain setter
// call, DESIGN.md §11, not a NoteEvent, DESIGN.md §10.1).
struct SlopeSwitchAt {
  int sampleOffset;
  SynthCore::LpfSlope toSlope;
};

struct Case {
  const char* name;
  float fs;
  SynthParams p;
  std::vector<NoteEvent> events;
  int totalSamples;
  std::vector<SlopeSwitchAt> slopeSwitches;  // usually empty
};

// ---- Case battery -----------------------------------------------------
constexpr int kBattN = 24000;  // 0.5s @ 48kHz -- see per-case fs for others

std::vector<Case> buildCases() {
  std::vector<Case> c;

  // ---- Defaults, at two sample rates (DESIGN.md §11 default column) ----
  {
    SynthParams p;
    c.push_back({"defaults_48k", 48000.0f, p, {{0, NoteEvent::NoteOn, 60, 1.0f}}, kBattN, {}});
  }
  {
    SynthParams p;
    c.push_back({"defaults_44k", 44100.0f, p, {{0, NoteEvent::NoteOn, 60, 0.8f}}, kBattN, {}});
  }

  // ---- Extreme modulation at the top of the keyboard, 96kHz ----
  {
    SynthParams p;
    p.lpfEnvAmountPercent = 100.0f;
    p.lpfKeyFollowPercent = 100.0f;
    p.lpfLfoAmountPercent = 100.0f;
    p.lfoRateHz = 5.0f;
    c.push_back({"extreme_mod_note108_96k", 96000.0f, p, {{0, NoteEvent::NoteOn, 108, 1.0f}}, kBattN, {}});
  }

  // ---- Low note, ENV-F pushing cutoff down into the 10Hz clamp floor ----
  {
    SynthParams p;
    p.lpfCutoffHz = 20.0f;
    p.lpfKeyFollowPercent = 100.0f;
    p.lpfEnvAmountPercent = -100.0f;
    c.push_back({"low_note_clamp_floor_48k", 48000.0f, p, {{0, NoteEvent::NoteOn, 21, 1.0f}}, kBattN, {}});
  }

  // ---- PWM at narrow duty cycles (DESIGN.md §4.1's DC-blocker case) ----
  {
    SynthParams p;
    p.osc1Wave = SynthCore::Wave::Pulse;
    p.osc1PwPercent = 10.0f;
    p.subLevelPercent = 30.0f;
    c.push_back({"pwm_10pct_48k", 48000.0f, p, {{0, NoteEvent::NoteOn, 60, 1.0f}}, kBattN, {}});
  }
  {
    SynthParams p;
    p.osc1Wave = SynthCore::Wave::Pulse;
    p.osc1PwPercent = 25.0f;
    c.push_back({"pwm_25pct_44k", 44100.0f, p, {{0, NoteEvent::NoteOn, 57, 1.0f}}, kBattN, {}});
  }

  // ---- SVF (12dB) slope, moderate and near-self-oscillation resonance ----
  {
    SynthParams p;
    p.lpfSlope = SynthCore::LpfSlope::Db12;
    p.lpfResonancePercent = 70.0f;
    c.push_back({"svf_res70_48k", 48000.0f, p, {{0, NoteEvent::NoteOn, 60, 1.0f}}, kBattN, {}});
  }
  {
    SynthParams p;
    p.lpfSlope = SynthCore::LpfSlope::Db12;
    p.lpfResonancePercent = 100.0f;
    p.lpfCutoffHz = 800.0f;
    c.push_back({"svf_selfosc_extreme_48k", 48000.0f, p, {{0, NoteEvent::NoteOn, 48, 1.0f}}, kBattN, {}});
  }

  // ---- Ladder (24dB), near-self-oscillation resonance ----
  {
    SynthParams p;
    p.lpfSlope = SynthCore::LpfSlope::Db24;
    p.lpfResonancePercent = 100.0f;
    p.lpfCutoffHz = 800.0f;
    c.push_back({"ladder_selfosc_extreme_48k", 48000.0f, p, {{0, NoteEvent::NoteOn, 48, 1.0f}}, kBattN, {}});
  }

  // ---- Full note-on/note-off cycle, exercising ENV-F/ENV-A release together ----
  {
    SynthParams p;
    const int off = kBattN / 2;
    c.push_back({"note_on_off_release_48k", 48000.0f, p,
                 {{0, NoteEvent::NoteOn, 64, 1.0f}, {off, NoteEvent::NoteOff, 64, 0.0f}}, kBattN, {}});
  }

  // ---- LFO -> cutoff, several cycles within the render ----
  {
    SynthParams p;
    p.lpfLfoAmountPercent = 100.0f;
    p.lfoRateHz = 8.0f;
    p.lfoWave = SynthCore::LfoWave::Square;
    c.push_back({"lfo_cutoff_square_48k", 48000.0f, p, {{0, NoteEvent::NoteOn, 60, 1.0f}}, kBattN, {}});
  }

  // ---- Key follow extreme, high note, 96kHz ----
  {
    SynthParams p;
    p.lpfKeyFollowPercent = 100.0f;
    p.lpfEnvAmountPercent = 0.0f;
    c.push_back({"key_follow_extreme_96k", 96000.0f, p, {{0, NoteEvent::NoteOn, 96, 1.0f}}, kBattN, {}});
  }

  // ---- A 3-note chord (the provisional G3 8-voice allocator, DESIGN.md
  // §10.3's real allocator is G7's job) -- multiple simultaneous filters. ----
  {
    SynthParams p;
    c.push_back({"chord_triad_48k", 48000.0f, p,
                 {{0, NoteEvent::NoteOn, 48, 0.9f},
                  {0, NoteEvent::NoteOn, 52, 0.8f},
                  {0, NoteEvent::NoteOn, 55, 0.85f}},
                 kBattN, {}});
  }

  // ---- Hard sync, both oscillators far apart, through the filter ----
  {
    SynthParams p;
    p.osc2Sync = true;
    p.osc2Octave = SynthCore::Octave::Ft4;
    p.osc2Semi = 7;
    c.push_back({"hardsync_48k", 48000.0f, p, {{0, NoteEvent::NoteOn, 45, 1.0f}}, kBattN, {}});
  }

  // ---- Triangle oscillators + sub, through the filter ----
  {
    SynthParams p;
    p.osc1Wave = SynthCore::Wave::Tri;
    p.osc2Wave = SynthCore::Wave::Tri;
    p.subLevelPercent = 50.0f;
    c.push_back({"tri_sub_48k", 48000.0f, p, {{0, NoteEvent::NoteOn, 50, 1.0f}}, kBattN, {}});
  }

  // ---- Noise (pink) through the filter, no tonal oscillators ----
  {
    SynthParams p;
    p.osc1LevelPercent = 0.0f;
    p.osc2LevelPercent = 0.0f;
    p.noiseColor = SynthCore::NoiseColor::Pink;
    p.noiseLevelPercent = 100.0f;
    p.lpfResonancePercent = 60.0f;
    c.push_back({"pink_noise_filtered_48k", 48000.0f, p, {{0, NoteEvent::NoteOn, 60, 1.0f}}, kBattN, {}});
  }

  // ---- The highest supported rate, moderate settings ----
  {
    SynthParams p;
    p.lpfResonancePercent = 40.0f;
    c.push_back({"192k_extreme_rate", 192000.0f, p, {{0, NoteEvent::NoteOn, 72, 1.0f}}, kBattN * 4, {}});
  }

  // ---- Bipolar ENV-F amount, negative, moderate resonance ----
  {
    SynthParams p;
    p.lpfEnvAmountPercent = -60.0f;
    p.lpfCutoffHz = 4000.0f;
    c.push_back({"env_amount_negative_48k", 48000.0f, p, {{0, NoteEvent::NoteOn, 60, 1.0f}}, kBattN, {}});
  }

  // ---- The slope crossfade itself (DESIGN.md §5.1/§5.4), both directions,
  // at the AC's own named test point (fc=2kHz, res=30). ----
  {
    SynthParams p;
    p.lpfCutoffHz = 2000.0f;
    p.lpfResonancePercent = 30.0f;
    p.lpfEnvAmountPercent = 0.0f;
    p.lpfKeyFollowPercent = 0.0f;
    p.lpfSlope = SynthCore::LpfSlope::Db24;
    c.push_back({"crossfade_24_to_12_48k", 48000.0f, p, {{0, NoteEvent::NoteOn, 55, 1.0f}}, kBattN,
                 {{kBattN / 2, SynthCore::LpfSlope::Db12}}});
  }
  {
    SynthParams p;
    p.lpfCutoffHz = 2000.0f;
    p.lpfResonancePercent = 30.0f;
    p.lpfEnvAmountPercent = 0.0f;
    p.lpfKeyFollowPercent = 0.0f;
    p.lpfSlope = SynthCore::LpfSlope::Db12;
    c.push_back({"crossfade_12_to_24_48k", 48000.0f, p, {{0, NoteEvent::NoteOn, 55, 1.0f}}, kBattN,
                 {{kBattN / 2, SynthCore::LpfSlope::Db24}}});
  }

  return c;
}

// Run one Case and append its L/R output to `out`. Odd, non-power-of-two
// block size (matching nassau-zermatt/Tests/amp_golden.cpp) to stress
// cross-block state handling (the control-rate grid remainder, DESIGN.md
// §2, G3.3). Live slope switches (mid-render setLpfSlope() calls) force a
// block boundary at their own sampleOffset so the switch lands exactly
// where the case specifies, matching Tests/filter_tests.cpp's own G5.4
// technique.
void runCase(const Case& c, std::vector<float>& out) {
  SynthCore core;
  configure(core, c.p);
  core.init(c.fs);

  std::vector<float> outL(static_cast<size_t>(c.totalSamples)), outR(static_cast<size_t>(c.totalSamples));
  const int block = 97;  // deliberately not a power of two
  int pos = 0;
  size_t evIdx = 0;
  size_t swIdx = 0;
  std::vector<NoteEvent> chunk;
  while (pos < c.totalSamples) {
    int n = std::min(block, c.totalSamples - pos);
    // Force a boundary exactly at the next pending slope switch, if one
    // falls inside what would otherwise be this chunk.
    if (swIdx < c.slopeSwitches.size()) {
      const int swAt = c.slopeSwitches[swIdx].sampleOffset;
      if (swAt > pos && swAt < pos + n) n = swAt - pos;
    }
    chunk.clear();
    while (evIdx < c.events.size() && c.events[evIdx].sampleOffset < pos + n) {
      NoteEvent e = c.events[evIdx];
      e.sampleOffset -= pos;
      if (e.sampleOffset < 0) e.sampleOffset = 0;
      chunk.push_back(e);
      ++evIdx;
    }
    core.process(chunk.empty() ? nullptr : chunk.data(), static_cast<int>(chunk.size()), outL.data() + pos,
                 outR.data() + pos, n);
    pos += n;
    if (swIdx < c.slopeSwitches.size() && c.slopeSwitches[swIdx].sampleOffset == pos) {
      core.setLpfSlope(c.slopeSwitches[swIdx].toSlope);
      ++swIdx;
    }
  }

  out.reserve(out.size() + outL.size() + outR.size());
  out.insert(out.end(), outL.begin(), outL.end());
  out.insert(out.end(), outR.begin(), outR.end());
}

std::vector<float> runAll() {
  const std::vector<Case> cases = buildCases();
  std::vector<float> out;
  for (const Case& c : cases) runCase(c, out);
  return out;
}

// ---- Fixture I/O --------------------------------------------------------
// File layout: [magic "NAAG"][u32 version][u32 count][count * float32].
constexpr char kMagicBytes[4] = {'N', 'A', 'A', 'G'};
constexpr uint32_t kVersion = 1;

bool writeFixture(const char* path, const std::vector<float>& data) {
  FILE* f = std::fopen(path, "wb");
  if (!f) {
    std::fprintf(stderr, "ERROR: cannot open %s for writing\n", path);
    return false;
  }
  uint32_t magic = 0;
  std::memcpy(&magic, kMagicBytes, 4);
  uint32_t hdr[3] = {magic, kVersion, static_cast<uint32_t>(data.size())};
  bool ok = std::fwrite(hdr, sizeof(uint32_t), 3, f) == 3;
  if (ok && !data.empty()) {
    ok = std::fwrite(data.data(), sizeof(float), data.size(), f) == data.size();
  }
  std::fclose(f);
  return ok;
}

bool readFixture(const char* path, std::vector<float>& data) {
  FILE* f = std::fopen(path, "rb");
  if (!f) {
    std::fprintf(stderr, "ERROR: cannot open %s for reading\n", path);
    return false;
  }
  uint32_t hdr[3] = {0, 0, 0};
  bool ok = std::fread(hdr, sizeof(uint32_t), 3, f) == 3;
  uint32_t expectedMagic = 0;
  std::memcpy(&expectedMagic, kMagicBytes, 4);
  if (!ok || hdr[0] != expectedMagic || hdr[1] != kVersion) {
    std::fprintf(stderr, "ERROR: bad fixture header in %s\n", path);
    std::fclose(f);
    return false;
  }
  data.resize(hdr[2]);
  if (hdr[2] > 0) {
    ok = std::fread(data.data(), sizeof(float), hdr[2], f) == hdr[2];
  }
  std::fclose(f);
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string mode = (argc > 1) ? argv[1] : "verify";
  const char* path = (argc > 2) ? argv[2] : GOLDEN_FIXTURE_PATH_G5;

  if (mode == "generate") {
    std::vector<float> data = runAll();
    if (!writeFixture(path, data)) return 2;
    std::printf("Generated golden fixture [G5]: %s (%zu values across %zu cases)\n", path, data.size(),
                buildCases().size());
    return 0;
  }

  if (mode == "verify") {
    std::vector<float> golden;
    if (!readFixture(path, golden)) {
      std::fprintf(stderr, "Run `synth_golden generate` first to create the fixture.\n");
      return 2;
    }
    std::vector<float> current = runAll();
    if (current.size() != golden.size()) {
      std::fprintf(stderr,
                   "ERROR: size mismatch (golden %zu vs current %zu) -- the case battery "
                   "changed; regenerate the golden.\n",
                   golden.size(), current.size());
      return 1;
    }

    // Parity tolerance: matches nassau-zermatt/Tests/amp_golden.cpp's own
    // reasoning -- float32 carries ~1e-7 relative precision near unity, so
    // 1e-6 absolute is ~10 ULP at full scale, tight enough to catch real
    // DSP drift, loose enough to absorb benign double->float rounding and
    // denormal flush-to-zero.
    const double kTol = 1e-6;
    double maxErr = 0.0;
    size_t maxIdx = 0;
    for (size_t i = 0; i < golden.size(); ++i) {
      const double e = std::fabs(static_cast<double>(current[i]) - static_cast<double>(golden[i]));
      if (e > maxErr) {
        maxErr = e;
        maxIdx = i;
      }
    }
    std::printf("Golden parity [G5]: %zu values, max abs error = %.3e (tol %.1e) at idx %zu\n", golden.size(),
                maxErr, kTol, maxIdx);
    if (maxErr > kTol) {
      std::fprintf(stderr, "FAIL: parity exceeded tolerance -- a change altered the audible output.\n");
      return 1;
    }
    std::printf("PASS: output matches golden within tolerance.\n");
    return 0;
  }

  std::fprintf(stderr, "Usage: %s [generate|verify] [fixture_path]\n", argv[0]);
  return 2;
}
