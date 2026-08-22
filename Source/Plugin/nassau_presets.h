#pragma once
// NassauAnalogue factory preset data (docs/GATES.md G9 deliverables).
//
// DEPENDENCY-FREE of iPlug2 (no SDK include) so the preset TABLE and its DSP
// effect can be exercised directly against a bare SynthCore from a plain DSP
// test binary (Tests/preset_tests.cpp), with no SDK link needed -- the same
// boundary nassau_state.h already established for the state-framing math
// (see that header's own file comment) and the same shape
// nassau-zermatt/Source/Plugin/nassau_presets.h uses (docs/GATES.md's own
// "copy the shape" instruction for this gate). Only includes synth_core.h
// (Source/DSP, itself SDK-free, R2).
//
// SINGLE SOURCE OF TRUTH: each preset is stored as a FULL 53-value array (one
// double per param, in enum order -- the exact positional layout a factory
// preset chunk has on disk, see nassau_state.h's "bad magic -> legacy
// fallback" path, which is what MakePresetFromChunk-built chunks actually
// are). NassauAnaloguePlugin.cpp builds its factory preset bank FROM THIS
// TABLE (via MakePresetFromChunk, so there is no separate, hand-typed copy of
// the numbers to drift out of sync -- see its ctor), and
// Tests/preset_tests.cpp applies the SAME table directly to a bare SynthCore
// (via ApplyPresetToSynthCore below) for G9.5's peak/finite measurement.
// There is exactly one place these numbers are written down.
//
// The param-index constants below MUST match NassauAnaloguePlugin.h's
// EParams enum exactly (value-for-value) -- NassauAnaloguePlugin.cpp
// static_asserts this immediately after including both headers, so any
// future accidental reorder (which R4 forbids anyway) is a compile error,
// not a silent preset corruption.
//
// VOICING (docs/GATES.md G9's own instruction: "the target is a Jupiter-6
// with a bit of Prophet-5", DESIGN.md §0.2): each of the 12 presets is a
// genuinely different use of the architecture -- see each preset's own
// comment for what it demonstrates. All 53 values are the exact stored
// param values DESIGN.md §11 ranges apply to (percent/Hz/ms/cents/semitones/
// enum-index/bool-as-0-or-1), never a scaled or clamped derivative -- G9.4
// checks exact recall against these same numbers.

#include "synth_core.h"

#include <array>
#include <cstddef>
#include <initializer_list>
#include <utility>

namespace nassau_presets {

// ===== Param indices (DESIGN.md §11) -- must match NassauAnaloguePlugin.h's
// EParams enum value-for-value; see the static_assert block in
// NassauAnaloguePlugin.cpp. =====
constexpr int kMasterVolume = 0;
constexpr int kOutputClip = 1;
constexpr int kOsc1Wave = 2;
constexpr int kOsc1Octave = 3;
constexpr int kOsc1Fine = 4;
constexpr int kOsc1PW = 5;
constexpr int kOsc1Level = 6;
constexpr int kOsc2Wave = 7;
constexpr int kOsc2Octave = 8;
constexpr int kOsc2Semi = 9;
constexpr int kOsc2Fine = 10;
constexpr int kOsc2PW = 11;
constexpr int kOsc2Level = 12;
constexpr int kOsc2Sync = 13;
constexpr int kOsc2KeyTrack = 14;
constexpr int kSubOctave = 15;
constexpr int kSubLevel = 16;
constexpr int kNoiseColor = 17;
constexpr int kNoiseLevel = 18;
constexpr int kEnvFAttack = 19;
constexpr int kEnvFDecay = 20;
constexpr int kEnvFSustain = 21;
constexpr int kEnvFRelease = 22;
constexpr int kEnvAAttack = 23;
constexpr int kEnvADecay = 24;
constexpr int kEnvASustain = 25;
constexpr int kEnvARelease = 26;
constexpr int kLfoWave = 27;
constexpr int kLfoRate = 28;
constexpr int kLfoDelay = 29;
constexpr int kLfoPitchAmount = 30;
constexpr int kLfoPwmAmount = 31;
constexpr int kLpfSlope = 32;
constexpr int kLpfCutoff = 33;
constexpr int kLpfResonance = 34;
constexpr int kLpfEnvAmount = 35;
constexpr int kLpfKeyFollow = 36;
constexpr int kLpfLfoAmount = 37;
constexpr int kDrive = 38;
constexpr int kHpfSlope = 39;
constexpr int kHpfCutoff = 40;
constexpr int kHpfKeyFollow = 41;
constexpr int kPmEnvFToOsc2 = 42;
constexpr int kPmEnvFToPw = 43;
constexpr int kPolyphony = 44;
constexpr int kVoiceMode = 45;
constexpr int kGlideTime = 46;
constexpr int kBendRange = 47;
constexpr int kVelToVca = 48;
constexpr int kVelToFilter = 49;
constexpr int kStereoMode = 50;
constexpr int kStereoDetune = 51;
constexpr int kStereoSpread = 52;

constexpr int kParamCount = 53;
constexpr int kPresetCount = 12;

using ParamArray = std::array<double, static_cast<size_t>(kParamCount)>;

// DESIGN.md §11's default column, in enum order, exactly. Wave enum:
// Saw=0/Pulse=1/Tri=2. Octave enum: 16'=0/8'=1/4'=2/2'=3. SubOctave enum:
// -1=0/-2=1. NoiseColor enum: White=0/Pink=1. LfoWave enum:
// Tri=0/Saw=1/Ramp=2/Square=3/S&H=4. LpfSlope enum: 24dB=0/12dB=1. HpfSlope
// enum: 12dB=0/24dB=1 (DESIGN.md §11 lists "12 / 24 dB" for this param, the
// opposite order from kLpfSlope -- kept as documented, matching
// synth_core.h's own HpfSlope enum comment). Polyphony enum:
// 4=0/6=1/8=2/12=3/16=4. VoiceMode enum: Poly=0/Unison=1/Mono=2. Bools stored
// as 0.0/1.0.
inline ParamArray Defaults() {
  ParamArray v{};
  v[static_cast<size_t>(kMasterVolume)] = -6.0;
  v[static_cast<size_t>(kOutputClip)] = 1.0;  // on
  v[static_cast<size_t>(kOsc1Wave)] = 0.0;    // Saw
  v[static_cast<size_t>(kOsc1Octave)] = 1.0;  // 8'
  v[static_cast<size_t>(kOsc1Fine)] = 0.0;
  v[static_cast<size_t>(kOsc1PW)] = 50.0;
  v[static_cast<size_t>(kOsc1Level)] = 100.0;
  v[static_cast<size_t>(kOsc2Wave)] = 0.0;  // Saw
  v[static_cast<size_t>(kOsc2Octave)] = 1.0;  // 8'
  v[static_cast<size_t>(kOsc2Semi)] = 0.0;
  v[static_cast<size_t>(kOsc2Fine)] = -7.0;
  v[static_cast<size_t>(kOsc2PW)] = 50.0;
  v[static_cast<size_t>(kOsc2Level)] = 80.0;
  v[static_cast<size_t>(kOsc2Sync)] = 0.0;      // off
  v[static_cast<size_t>(kOsc2KeyTrack)] = 1.0;  // on
  v[static_cast<size_t>(kSubOctave)] = 0.0;     // -1
  v[static_cast<size_t>(kSubLevel)] = 0.0;
  v[static_cast<size_t>(kNoiseColor)] = 0.0;  // White
  v[static_cast<size_t>(kNoiseLevel)] = 0.0;
  v[static_cast<size_t>(kEnvFAttack)] = 2.0;
  v[static_cast<size_t>(kEnvFDecay)] = 400.0;
  v[static_cast<size_t>(kEnvFSustain)] = 30.0;
  v[static_cast<size_t>(kEnvFRelease)] = 300.0;
  v[static_cast<size_t>(kEnvAAttack)] = 2.0;
  v[static_cast<size_t>(kEnvADecay)] = 800.0;
  v[static_cast<size_t>(kEnvASustain)] = 80.0;
  v[static_cast<size_t>(kEnvARelease)] = 250.0;
  v[static_cast<size_t>(kLfoWave)] = 0.0;  // Tri
  v[static_cast<size_t>(kLfoRate)] = 5.0;
  v[static_cast<size_t>(kLfoDelay)] = 0.0;
  v[static_cast<size_t>(kLfoPitchAmount)] = 0.0;
  v[static_cast<size_t>(kLfoPwmAmount)] = 0.0;
  v[static_cast<size_t>(kLpfSlope)] = 0.0;  // 24 dB
  v[static_cast<size_t>(kLpfCutoff)] = 2000.0;
  v[static_cast<size_t>(kLpfResonance)] = 20.0;
  v[static_cast<size_t>(kLpfEnvAmount)] = 40.0;
  v[static_cast<size_t>(kLpfKeyFollow)] = 50.0;
  v[static_cast<size_t>(kLpfLfoAmount)] = 0.0;
  v[static_cast<size_t>(kDrive)] = 15.0;
  v[static_cast<size_t>(kHpfSlope)] = 0.0;  // 12 dB
  v[static_cast<size_t>(kHpfCutoff)] = 20.0;  // = hard bypass
  v[static_cast<size_t>(kHpfKeyFollow)] = 0.0;
  v[static_cast<size_t>(kPmEnvFToOsc2)] = 0.0;
  v[static_cast<size_t>(kPmEnvFToPw)] = 0.0;
  v[static_cast<size_t>(kPolyphony)] = 2.0;  // Eight
  v[static_cast<size_t>(kVoiceMode)] = 0.0;  // Poly
  v[static_cast<size_t>(kGlideTime)] = 0.0;
  v[static_cast<size_t>(kBendRange)] = 2.0;
  v[static_cast<size_t>(kVelToVca)] = 40.0;
  v[static_cast<size_t>(kVelToFilter)] = 20.0;
  v[static_cast<size_t>(kStereoMode)] = 0.0;  // off
  v[static_cast<size_t>(kStereoDetune)] = 6.0;
  v[static_cast<size_t>(kStereoSpread)] = 70.0;
  return v;
}

// Build a preset's full array from the defaults plus a sparse list of
// (paramIndex, value) overrides -- mirrors MakePresetFromNamedParams' own
// "unnamed params fall back to the default" semantics (DESIGN.md/GATES.md),
// but keeps the values in ONE place (see file header).
inline ParamArray WithOverrides(std::initializer_list<std::pair<int, double>> overrides) {
  ParamArray v = Defaults();
  for (const auto& o : overrides) v[static_cast<size_t>(o.first)] = o.second;
  return v;
}

struct PresetSpec {
  const char* name;
  ParamArray values;
};

// ===== The 12 factory presets (docs/GATES.md G9 deliverable list) =====
//
// Each is a genuinely different corner of the architecture (docs/GATES.md
// G9's own instruction: "a bank where twelve presets differ only in cutoff
// is a failed deliverable"). Numbers were tuned against
// Tests/preset_tests.cpp's own measured peaks (G9.5: 2 s of a 4-note chord
// must land in [-30, 0] dBFS, all samples finite) -- see that test's output
// for the final per-preset dBFS.
inline const std::array<PresetSpec, static_cast<size_t>(kPresetCount)>& Presets() {
  static const std::array<PresetSpec, static_cast<size_t>(kPresetCount)> kTable = {{
      // 0 - Init Poly: the instrument's own DESIGN.md §11 defaults, with only
      // kMasterVolume pulled down for headroom (a 4-note chord at full
      // per-voice level otherwise pushes the raw default -6 dB past 0 dBFS
      // before the output clip's soft knee, G9.5's own measured finding).
      // The "clean slate" every real synth ships as its first patch.
      {"Init Poly", WithOverrides({{kMasterVolume, -12.0}})},

      // 1 - Jupiter Brass: classic Jupiter-6 brass swell. Both saws, 24 dB
      // ladder opened by a fast-attack filter envelope with high key
      // follow, a touch of drive for edge, a slow-delayed vibrato, and a
      // subtle Prophet-flavoured Poly-Mod (ENV-F -> VCO2 pitch) for growl.
      // Stereo mode widens it.
      {"Jupiter Brass",
       WithOverrides({{kOsc1Level, 100.0},
                       {kOsc2Level, 90.0},
                       {kEnvFAttack, 8.0},
                       {kEnvFDecay, 650.0},
                       {kEnvFSustain, 45.0},
                       {kEnvFRelease, 300.0},
                       {kEnvAAttack, 6.0},
                       {kEnvADecay, 500.0},
                       {kEnvASustain, 85.0},
                       {kEnvARelease, 280.0},
                       {kLpfSlope, 0.0},  // 24 dB
                       {kLpfCutoff, 950.0},
                       {kLpfResonance, 28.0},
                       {kLpfEnvAmount, 55.0},
                       {kLpfKeyFollow, 60.0},
                       {kDrive, 20.0},
                       {kPmEnvFToOsc2, 8.0},
                       {kLfoRate, 5.5},
                       {kLfoDelay, 350.0},
                       {kLfoPitchAmount, 8.0},
                       {kStereoMode, 1.0},
                       {kStereoDetune, 8.0},
                       {kStereoSpread, 60.0}})},

      // 2 - Prophet Strings: lush detuned ensemble pad. 12 dB SVF for a
      // softer top, slow envelopes, LFO->PWM for a chorus-like shimmer
      // instead of an audio-rate chorus effect (none exists, DESIGN.md §13),
      // wide stereo.
      {"Prophet Strings",
       WithOverrides({{kMasterVolume, -14.0},  // headroom: wide detuned stereo ensemble
                       {kOsc1Level, 95.0},
                       {kOsc2Fine, 9.0},
                       {kOsc2Level, 90.0},
                       {kEnvFAttack, 450.0},
                       {kEnvFDecay, 2000.0},
                       {kEnvFSustain, 60.0},
                       {kEnvFRelease, 1200.0},
                       {kEnvAAttack, 500.0},
                       {kEnvADecay, 1800.0},
                       {kEnvASustain, 75.0},
                       {kEnvARelease, 1400.0},
                       {kLpfSlope, 1.0},  // 12 dB
                       {kLpfCutoff, 1500.0},
                       {kLpfResonance, 15.0},
                       {kLpfEnvAmount, 30.0},
                       {kLpfKeyFollow, 40.0},
                       {kDrive, 5.0},
                       {kLfoWave, 0.0},  // Tri
                       {kLfoRate, 0.4},
                       {kLfoPwmAmount, 30.0},
                       {kStereoMode, 1.0},
                       {kStereoDetune, 12.0},
                       {kStereoSpread, 90.0}})},

      // 3 - Sub Bass: Mono legato bass built around the sub-oscillator
      // (SubLevel 70 -- the whole point of the patch), 16' fundamental, a
      // second saw for growl, high key follow so the low-passed bass tracks
      // the keyboard, a little drive. Uses Mono voice mode + glide.
      {"Sub Bass",
       WithOverrides({{kOsc1Octave, 0.0},  // 16'
                       {kOsc1Wave, 1.0},   // Pulse
                       {kOsc1PW, 50.0},
                       {kOsc1Level, 95.0},
                       {kOsc2Level, 25.0},
                       {kSubOctave, 0.0},  // -1 (from an already-16' VCO1)
                       {kSubLevel, 70.0},
                       {kEnvFAttack, 1.0},
                       {kEnvFDecay, 250.0},
                       {kEnvFSustain, 55.0},
                       {kEnvFRelease, 150.0},
                       {kEnvAAttack, 1.0},
                       {kEnvADecay, 300.0},
                       {kEnvASustain, 90.0},
                       {kEnvARelease, 120.0},
                       {kLpfSlope, 0.0},  // 24 dB
                       {kLpfCutoff, 320.0},
                       {kLpfResonance, 30.0},
                       {kLpfEnvAmount, 25.0},
                       {kLpfKeyFollow, 80.0},
                       {kDrive, 25.0},
                       {kVoiceMode, 2.0},  // Mono
                       {kGlideTime, 35.0}})},

      // 4 - PWM Pad: both oscillators pulse waves, PW swept slowly by the
      // LFO (LfoPwmAmount 60%) rather than held static -- the PWM sound
      // DESIGN.md §4.1 calls out as one of this instrument's core timbres.
      // 12 dB SVF, wide stereo.
      {"PWM Pad",
       WithOverrides({{kMasterVolume, -15.0},  // headroom: wide detuned stereo pulse pad
                       {kOsc1Wave, 1.0},  // Pulse
                       {kOsc1PW, 50.0},
                       {kOsc2Wave, 1.0},  // Pulse
                       {kOsc2Fine, 6.0},
                       {kOsc2PW, 50.0},
                       {kOsc2Level, 85.0},
                       {kEnvFAttack, 300.0},
                       {kEnvFDecay, 1200.0},
                       {kEnvFSustain, 55.0},
                       {kEnvFRelease, 900.0},
                       {kEnvAAttack, 250.0},
                       {kEnvADecay, 1000.0},
                       {kEnvASustain, 80.0},
                       {kEnvARelease, 800.0},
                       {kLfoWave, 0.0},  // Tri
                       {kLfoRate, 0.3},
                       {kLfoPwmAmount, 60.0},
                       {kLpfSlope, 1.0},  // 12 dB
                       {kLpfCutoff, 1800.0},
                       {kLpfResonance, 18.0},
                       {kLpfEnvAmount, 20.0},
                       {kStereoMode, 1.0},
                       {kStereoDetune, 10.0},
                       {kStereoSpread, 75.0}})},

      // 5 - Sync Lead: VCO2 hard-synced to VCO1, swept by the Prophet-
      // flavoured Poly-Mod (ENV-F -> VCO2 pitch, 60% -- a large bipolar
      // sweep as ENV-F decays) for the classic sync-sweep lead. Mono, short
      // legato glide.
      {"Sync Lead",
       WithOverrides({{kOsc2Sync, 1.0},  // on
                       {kOsc2Semi, 7.0},
                       {kOsc2Level, 70.0},
                       {kEnvFAttack, 1.0},
                       {kEnvFDecay, 350.0},
                       {kEnvFSustain, 15.0},
                       {kEnvFRelease, 200.0},
                       {kEnvAAttack, 3.0},
                       {kEnvADecay, 400.0},
                       {kEnvASustain, 85.0},
                       {kEnvARelease, 180.0},
                       {kLpfSlope, 0.0},  // 24 dB
                       {kLpfCutoff, 4500.0},
                       {kLpfResonance, 22.0},
                       {kLpfEnvAmount, 15.0},
                       {kDrive, 20.0},
                       {kPmEnvFToOsc2, 60.0},
                       {kVoiceMode, 2.0},  // Mono
                       {kGlideTime, 40.0}})},

      // 6 - Noise Sweep: pure noise (Osc1/Osc2/Sub all silent), a slow,
      // highly resonant filter-envelope sweep, and a non-bypassed 24 dB HPF
      // to trim rumble. Unison mode -- all polyphony voices retrigger
      // together (DESIGN.md §10.5) -- for a big, wide whoosh.
      {"Noise Sweep",
       WithOverrides({{kMasterVolume, -14.0},  // headroom: near-self-oscillating resonant sweep
                       {kOsc1Level, 0.0},
                       {kOsc2Level, 0.0},
                       {kNoiseColor, 0.0},  // White
                       {kNoiseLevel, 100.0},
                       {kEnvFAttack, 250.0},
                       {kEnvFDecay, 1600.0},
                       {kEnvFSustain, 20.0},
                       {kEnvFRelease, 700.0},
                       {kEnvAAttack, 200.0},
                       {kEnvADecay, 1400.0},
                       {kEnvASustain, 70.0},
                       {kEnvARelease, 600.0},
                       {kLpfSlope, 0.0},  // 24 dB
                       {kLpfCutoff, 500.0},
                       {kLpfResonance, 60.0},
                       {kLpfEnvAmount, 80.0},
                       {kLpfKeyFollow, 20.0},
                       {kHpfSlope, 1.0},  // 24 dB
                       {kHpfCutoff, 100.0},
                       {kVoiceMode, 1.0},  // Unison
                       {kStereoMode, 1.0},
                       {kStereoDetune, 20.0},
                       {kStereoSpread, 100.0}})},

      // 7 - Fat Stack: both oscillators + a moderate sub for low-end
      // thickness, a wide stereo detune spread, and enough drive to add
      // harmonic weight -- the "wall of Jupiter" unison-adjacent sound, but
      // built from Poly + the stereo dual chain (DESIGN.md §9) rather than
      // the Unison voice mode. A subtle 12 dB HPF keeps the low end tight
      // under the sub.
      {"Fat Stack",
       WithOverrides({{kMasterVolume, -13.0},  // headroom: 4-voice-chord + sub + wide stereo detune
                       {kOsc1Level, 100.0},
                       {kOsc2Level, 90.0},
                       {kSubLevel, 30.0},
                       {kEnvFAttack, 4.0},
                       {kEnvFDecay, 500.0},
                       {kEnvFSustain, 50.0},
                       {kEnvFRelease, 350.0},
                       {kEnvAAttack, 3.0},
                       {kEnvADecay, 600.0},
                       {kEnvASustain, 85.0},
                       {kEnvARelease, 300.0},
                       {kLpfSlope, 0.0},  // 24 dB
                       {kLpfCutoff, 1400.0},
                       {kLpfResonance, 25.0},
                       {kLpfEnvAmount, 35.0},
                       {kLpfKeyFollow, 55.0},
                       {kDrive, 30.0},
                       {kHpfSlope, 0.0},  // 12 dB
                       {kHpfCutoff, 45.0},
                       {kStereoMode, 1.0},
                       {kStereoDetune, 20.0},
                       {kStereoSpread, 100.0}})},

      // 8 - Bell Keys: Prophet-style "bell" patch -- a fast filter-envelope
      // decay drives a LARGE bipolar Poly-Mod sweep (ENV-F -> VCO2 pitch,
      // 45%) plus a touch of ENV-F -> PW, producing an inharmonic
      // pitch/timbre "ping" no audio-rate FM is needed for (DESIGN.md §8,
      // [PERF-6]). Triangle VCO1 for a soft body under the shimmer, 12 dB
      // SVF with a resonant peak.
      {"Bell Keys",
       WithOverrides({{kMasterVolume, -16.0},  // headroom: resonant Poly-Mod bell ping, 4-note chord
                       {kOsc1Wave, 2.0},  // Tri
                       {kOsc1Level, 90.0},
                       {kOsc2Level, 75.0},
                       {kEnvFAttack, 1.0},
                       {kEnvFDecay, 180.0},
                       {kEnvFSustain, 5.0},
                       {kEnvFRelease, 400.0},
                       {kEnvAAttack, 1.0},
                       {kEnvADecay, 1300.0},
                       {kEnvASustain, 10.0},
                       {kEnvARelease, 900.0},
                       {kLpfSlope, 1.0},  // 12 dB
                       {kLpfCutoff, 2600.0},
                       {kLpfResonance, 55.0},
                       {kLpfEnvAmount, 25.0},
                       {kPmEnvFToOsc2, 45.0},
                       {kPmEnvFToPw, 15.0},
                       {kStereoMode, 1.0},
                       {kStereoDetune, 4.0},
                       {kStereoSpread, 50.0}})},

      // 9 - Resonant Pluck: a snappy filter envelope (instant attack, fast
      // decay to zero sustain) slamming a NEARLY self-oscillating 24 dB
      // ladder (resonance 85%) shut -- the classic plucked-analogue sound. A
      // touch of noise adds transient bite.
      {"Resonant Pluck",
       WithOverrides({{kOsc1Level, 100.0},
                       {kOsc2Fine, -5.0},
                       {kOsc2Level, 75.0},
                       {kNoiseLevel, 5.0},
                       {kEnvFAttack, 1.0},
                       {kEnvFDecay, 220.0},
                       {kEnvFSustain, 0.0},
                       {kEnvFRelease, 150.0},
                       {kEnvAAttack, 1.0},
                       {kEnvADecay, 320.0},
                       {kEnvASustain, 0.0},
                       {kEnvARelease, 150.0},
                       {kLpfSlope, 0.0},  // 24 dB
                       {kLpfCutoff, 420.0},
                       {kLpfResonance, 85.0},
                       {kLpfEnvAmount, 90.0},
                       {kLpfKeyFollow, 70.0},
                       {kDrive, 10.0}})},

      // 10 - HPF Clav: percussive clavinet-style pulses, brightened by a
      // non-bypassed 24 dB high-pass (key-followed) that carves out the low
      // end DESIGN.md §1's ordering constraint says the HPF removes before
      // the resonant LPF ever sees it, plus a fast filter/amp envelope and
      // real drive bite.
      {"HPF Clav",
       WithOverrides({{kOsc1Wave, 1.0},  // Pulse
                       {kOsc1PW, 30.0},
                       {kOsc1Level, 100.0},
                       {kOsc2Wave, 1.0},  // Pulse
                       {kOsc2Fine, 4.0},
                       {kOsc2PW, 35.0},
                       {kOsc2Level, 85.0},
                       {kEnvFAttack, 1.0},
                       {kEnvFDecay, 120.0},
                       {kEnvFSustain, 10.0},
                       {kEnvFRelease, 90.0},
                       {kEnvAAttack, 1.0},
                       {kEnvADecay, 180.0},
                       {kEnvASustain, 20.0},
                       {kEnvARelease, 100.0},
                       {kLpfSlope, 1.0},  // 12 dB
                       {kLpfCutoff, 4000.0},
                       {kLpfResonance, 25.0},
                       {kLpfEnvAmount, 30.0},
                       {kDrive, 35.0},
                       {kHpfSlope, 1.0},  // 24 dB
                       {kHpfCutoff, 400.0},
                       {kHpfKeyFollow, 50.0}})},

      // 11 - Stereo Wash: slow ambient pad showcasing the full stereo dual
      // chain at maximum detune/spread, LFO->LPF cutoff movement
      // (LpfLfoAmount) alongside the envelope, and a hint of pink noise for
      // air. The single widest, slowest preset in the bank.
      {"Stereo Wash",
       WithOverrides({{kMasterVolume, -16.0},  // headroom: widest/slowest pad, 4-note chord
                       {kOsc2Fine, 11.0},
                       {kOsc2Level, 85.0},
                       {kNoiseColor, 1.0},  // Pink
                       {kNoiseLevel, 8.0},
                       {kEnvFAttack, 1500.0},
                       {kEnvFDecay, 3000.0},
                       {kEnvFSustain, 70.0},
                       {kEnvFRelease, 2500.0},
                       {kEnvAAttack, 1200.0},
                       {kEnvADecay, 2600.0},
                       {kEnvASustain, 75.0},
                       {kEnvARelease, 2200.0},
                       {kLfoWave, 0.0},  // Tri
                       {kLfoRate, 0.2},
                       {kLfoPitchAmount, 5.0},
                       {kLfoPwmAmount, 20.0},
                       {kLpfSlope, 1.0},  // 12 dB
                       {kLpfCutoff, 1200.0},
                       {kLpfResonance, 20.0},
                       {kLpfEnvAmount, 20.0},
                       {kLpfLfoAmount, 40.0},
                       {kStereoMode, 1.0},
                       {kStereoDetune, 25.0},
                       {kStereoSpread, 100.0}})},
  }};
  return kTable;
}

// Apply a preset's full param array directly to a bare SynthCore -- the same
// index-to-setter mapping NassauAnaloguePlugin::OnParamChange uses, kept here
// so Tests/preset_tests.cpp can drive SynthCore without linking the SDK
// (G9.5).
inline void ApplyPresetToSynthCore(const ParamArray& v, SynthCore& core) {
  core.setMasterVolumeDb(static_cast<float>(v[static_cast<size_t>(kMasterVolume)]));
  core.setOutputClip(v[static_cast<size_t>(kOutputClip)] != 0.0);
  core.setOsc1Wave(static_cast<SynthCore::Wave>(static_cast<int>(v[static_cast<size_t>(kOsc1Wave)])));
  core.setOsc1Octave(
      static_cast<SynthCore::Octave>(static_cast<int>(v[static_cast<size_t>(kOsc1Octave)])));
  core.setOsc1FineCents(static_cast<float>(v[static_cast<size_t>(kOsc1Fine)]));
  core.setOsc1PwPercent(static_cast<float>(v[static_cast<size_t>(kOsc1PW)]));
  core.setOsc1LevelPercent(static_cast<float>(v[static_cast<size_t>(kOsc1Level)]));
  core.setOsc2Wave(static_cast<SynthCore::Wave>(static_cast<int>(v[static_cast<size_t>(kOsc2Wave)])));
  core.setOsc2Octave(
      static_cast<SynthCore::Octave>(static_cast<int>(v[static_cast<size_t>(kOsc2Octave)])));
  core.setOsc2Semi(static_cast<int>(v[static_cast<size_t>(kOsc2Semi)]));
  core.setOsc2FineCents(static_cast<float>(v[static_cast<size_t>(kOsc2Fine)]));
  core.setOsc2PwPercent(static_cast<float>(v[static_cast<size_t>(kOsc2PW)]));
  core.setOsc2LevelPercent(static_cast<float>(v[static_cast<size_t>(kOsc2Level)]));
  core.setOsc2Sync(v[static_cast<size_t>(kOsc2Sync)] != 0.0);
  core.setOsc2KeyTrack(v[static_cast<size_t>(kOsc2KeyTrack)] != 0.0);
  core.setSubOctave(
      static_cast<SynthCore::SubOctave>(static_cast<int>(v[static_cast<size_t>(kSubOctave)])));
  core.setSubLevelPercent(static_cast<float>(v[static_cast<size_t>(kSubLevel)]));
  core.setNoiseColor(
      static_cast<SynthCore::NoiseColor>(static_cast<int>(v[static_cast<size_t>(kNoiseColor)])));
  core.setNoiseLevelPercent(static_cast<float>(v[static_cast<size_t>(kNoiseLevel)]));
  core.setEnvFAttackMs(static_cast<float>(v[static_cast<size_t>(kEnvFAttack)]));
  core.setEnvFDecayMs(static_cast<float>(v[static_cast<size_t>(kEnvFDecay)]));
  core.setEnvFSustainPercent(static_cast<float>(v[static_cast<size_t>(kEnvFSustain)]));
  core.setEnvFReleaseMs(static_cast<float>(v[static_cast<size_t>(kEnvFRelease)]));
  core.setEnvAAttackMs(static_cast<float>(v[static_cast<size_t>(kEnvAAttack)]));
  core.setEnvADecayMs(static_cast<float>(v[static_cast<size_t>(kEnvADecay)]));
  core.setEnvASustainPercent(static_cast<float>(v[static_cast<size_t>(kEnvASustain)]));
  core.setEnvAReleaseMs(static_cast<float>(v[static_cast<size_t>(kEnvARelease)]));
  core.setLfoWave(
      static_cast<SynthCore::LfoWave>(static_cast<int>(v[static_cast<size_t>(kLfoWave)])));
  core.setLfoRateHz(static_cast<float>(v[static_cast<size_t>(kLfoRate)]));
  core.setLfoDelayMs(static_cast<float>(v[static_cast<size_t>(kLfoDelay)]));
  core.setLfoPitchAmountPercent(static_cast<float>(v[static_cast<size_t>(kLfoPitchAmount)]));
  core.setLfoPwmAmountPercent(static_cast<float>(v[static_cast<size_t>(kLfoPwmAmount)]));
  core.setLpfSlope(
      static_cast<SynthCore::LpfSlope>(static_cast<int>(v[static_cast<size_t>(kLpfSlope)])));
  core.setLpfCutoffHz(static_cast<float>(v[static_cast<size_t>(kLpfCutoff)]));
  core.setLpfResonancePercent(static_cast<float>(v[static_cast<size_t>(kLpfResonance)]));
  core.setLpfEnvAmountPercent(static_cast<float>(v[static_cast<size_t>(kLpfEnvAmount)]));
  core.setLpfKeyFollowPercent(static_cast<float>(v[static_cast<size_t>(kLpfKeyFollow)]));
  core.setLpfLfoAmountPercent(static_cast<float>(v[static_cast<size_t>(kLpfLfoAmount)]));
  core.setDrivePercent(static_cast<float>(v[static_cast<size_t>(kDrive)]));
  core.setHpfSlope(
      static_cast<SynthCore::HpfSlope>(static_cast<int>(v[static_cast<size_t>(kHpfSlope)])));
  core.setHpfCutoffHz(static_cast<float>(v[static_cast<size_t>(kHpfCutoff)]));
  core.setHpfKeyFollowPercent(static_cast<float>(v[static_cast<size_t>(kHpfKeyFollow)]));
  core.setPmEnvFToOsc2Percent(static_cast<float>(v[static_cast<size_t>(kPmEnvFToOsc2)]));
  core.setPmEnvFToPwPercent(static_cast<float>(v[static_cast<size_t>(kPmEnvFToPw)]));
  core.setPolyphony(
      static_cast<SynthCore::Polyphony>(static_cast<int>(v[static_cast<size_t>(kPolyphony)])));
  core.setVoiceMode(
      static_cast<SynthCore::VoiceMode>(static_cast<int>(v[static_cast<size_t>(kVoiceMode)])));
  core.setGlideTimeMs(static_cast<float>(v[static_cast<size_t>(kGlideTime)]));
  core.setBendRangeSemitones(static_cast<int>(v[static_cast<size_t>(kBendRange)]));
  core.setVelToVcaPercent(static_cast<float>(v[static_cast<size_t>(kVelToVca)]));
  core.setVelToFilterPercent(static_cast<float>(v[static_cast<size_t>(kVelToFilter)]));
  core.setStereoMode(v[static_cast<size_t>(kStereoMode)] != 0.0);
  core.setStereoDetuneCents(static_cast<float>(v[static_cast<size_t>(kStereoDetune)]));
  core.setStereoSpreadPercent(static_cast<float>(v[static_cast<size_t>(kStereoSpread)]));
}

}  // namespace nassau_presets
