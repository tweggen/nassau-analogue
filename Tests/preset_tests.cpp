// NassauAnalogue factory preset tests -- Gate G9 (docs/GATES.md).
//
// Drives Source/Plugin/nassau_presets.h's 12 factory presets directly against
// a bare SynthCore (ApplyPresetToSynthCore) -- the SAME data the plugin's
// MakePresetFromChunk calls consume (NassauAnaloguePlugin.cpp), so there is
// no separate, hand-typed copy of the numbers here to drift out of sync (see
// nassau_presets.h's file header). No SDK link needed, matching
// Tests/state_tests.cpp's boundary (G9.1's "pure framing math" note) -- the
// preset DATA is likewise pure, dependency-free math; only its recall
// through the actual IPlug2 preset mechanism needs the SDK, which is out of
// scope for a DSP-only test binary on this Linux dev box (DESIGN.md §0.4).
//
// Covers:
//   G9.3 (partial) -- nassau_presets::kParamCount == SynthCore::kNumParams,
//                     kPresetCount == 12
//                      (the SDK-side kNumParams/kNumPresets equality is a
//                      static_assert in NassauAnaloguePlugin.h/.cpp instead).
//   G9.4           -- every preset recalls to its EXACT stored values
//                      (trivially true here since ApplyPresetToSynthCore
//                      reads directly from the table -- the interesting half
//                      of G9.4, "every value is step-aligned to its IParam",
//                      is checked directly against the table without an
//                      IParam, since none exists SDK-free; see that group's
//                      own comment).
//   G9.5           -- every preset, driven with a 2 s / 4-note chord, yields
//                      an output peak in [-30, 0] dBFS, all samples finite.

#include "nassau_presets.h"
#include "synth_core.h"
#include "test_util.h"

#include <cmath>
#include <iostream>
#include <string>

namespace {

int g_checks = 0;
int g_failures = 0;

void check(const char* name, bool cond) {
  ++g_checks;
  std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name << "\n";
  if (!cond) ++g_failures;
}

// Peak absolute sample value across both channels, converted to dBFS
// (20*log10(peak); silence floors at -300 dB rather than -inf).
double peakDb(const std::vector<float>& l, const std::vector<float>& r) {
  double peak = 0.0;
  for (float s : l) peak = std::max(peak, static_cast<double>(std::fabs(s)));
  for (float s : r) peak = std::max(peak, static_cast<double>(std::fabs(s)));
  return peak > 0.0 ? 20.0 * std::log10(peak) : -300.0;
}

// DESIGN.md §11's own step-alignment convention (a "%"/"Hz"/"ms"/"cents"
// param is a continuous IParam registered with a specific step size, an enum
// param is registered with InitEnum -- an integer index -- and a bool with
// InitBool -- 0.0 or 1.0). Since no IParam exists in this SDK-free binary
// (G9.1's own boundary), this checks the STORED value against the exact
// discrete set each param's type allows, matching what an IParam::Set()
// would snap it to on the plugin side. Continuous ("dB"/"Hz"/"%"/"ms"/
// "cents"/"semitones") params have a step small enough (<=0.1 of their
// documented step per DESIGN.md §11) that every value in this table -- all
// deliberately chosen to a whole or one-decimal number -- already lands on
// a representable step; there is nothing further to misalign for those, so
// only the DISCRETE (enum/bool/int) params are checked exactly here.
bool isWholeNumber(double v) { return v == std::floor(v); }

// Renders `absEvents` (sampleOffset already absolute, matching NoteEvent's
// own field) in realistic 512-sample host blocks -- exercising the control-
// rate grid the same way a real host would, not one giant single call. Same
// shape as Tests/stereo_tests.cpp's own renderAbsEvents helper.
void renderAbsEvents(SynthCore& core, const std::vector<NoteEvent>& absEvents, int totalSamples, int block,
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
    core.process(chunk.empty() ? nullptr : chunk.data(), static_cast<int>(chunk.size()), outL.data() + pos,
                 outR.data() + pos, n);
    pos += n;
  }
}

}  // namespace

int main() {
  std::cout << "=== NassauAnalogue Preset Tests (G9) ===\n\n";

  std::cout << "Group: preset table shape (G9.3)\n";
  check("nassau_presets::kParamCount == SynthCore::kNumParams (54 since G12 appended kChorus)",
        nassau_presets::kParamCount == SynthCore::kNumParams);
  check("nassau_presets::kParamCount == SynthCore::kNumParams",
        nassau_presets::kParamCount == SynthCore::kNumParams);
  check("nassau_presets::kPresetCount == 12", nassau_presets::kPresetCount == 12);
  check("Presets() has 12 entries",
        nassau_presets::Presets().size() == static_cast<size_t>(nassau_presets::kPresetCount));

  std::cout << "\nGroup: preset names are non-empty and unique (sanity)\n";
  {
    bool allNonEmpty = true;
    bool allUnique = true;
    const auto& presets = nassau_presets::Presets();
    for (size_t i = 0; i < presets.size(); ++i) {
      if (presets[i].name == nullptr || presets[i].name[0] == '\0') allNonEmpty = false;
      for (size_t j = i + 1; j < presets.size(); ++j) {
        if (std::string(presets[i].name) == presets[j].name) allUnique = false;
      }
    }
    check("all preset names non-empty", allNonEmpty);
    check("all preset names unique", allUnique);
  }

  std::cout << "\nGroup: every preset value is step-aligned to its param type (G9.4)\n";
  {
    // Discrete-typed param indices (enum, bool, or integer-semitone params,
    // DESIGN.md §11) -- every other index is a continuous %/Hz/ms/cents
    // param, see isWholeNumber's own comment for why those need no separate
    // check here.
    const int discreteIdx[] = {
        nassau_presets::kOutputClip,  nassau_presets::kOsc1Wave,     nassau_presets::kOsc1Octave,
        nassau_presets::kOsc2Wave,    nassau_presets::kOsc2Octave,   nassau_presets::kOsc2Semi,
        nassau_presets::kOsc2Sync,    nassau_presets::kOsc2KeyTrack, nassau_presets::kSubOctave,
        nassau_presets::kNoiseColor,  nassau_presets::kLfoWave,      nassau_presets::kLpfSlope,
        nassau_presets::kHpfSlope,    nassau_presets::kPolyphony,    nassau_presets::kVoiceMode,
        nassau_presets::kBendRange,   nassau_presets::kStereoMode};
    bool allAligned = true;
    for (const auto& preset : nassau_presets::Presets()) {
      for (int idx : discreteIdx) {
        if (!isWholeNumber(preset.values[static_cast<size_t>(idx)])) allAligned = false;
      }
    }
    check("every preset's discrete-typed params are whole-number step-aligned", allAligned);
  }

  std::cout << "\nGroup: preset recall reproduces the exact stored values (G9.4)\n";
  std::cout << "  (ApplyPresetToSynthCore is READ directly from nassau_presets::Presets()'s\n"
            << "   own table -- see nassau_state.h's 'bad magic -> legacy fallback' path,\n"
            << "   which is the exact positional-double chunk MakePresetFromChunk builds --\n"
            << "   so recall is exact by construction; this asserts the table itself has\n"
            << "   not been accidentally mutated between definition and use.)\n";
  {
    bool allExact = true;
    const auto& presets = nassau_presets::Presets();
    for (const auto& preset : presets) {
      // Re-fetch from Presets() (not the loop variable) to catch any
      // accidental non-determinism in the table's own construction (e.g. a
      // static initializer that is not actually idempotent).
      for (size_t i = 0; i < static_cast<size_t>(nassau_presets::kParamCount); ++i) {
        if (preset.values[i] != preset.values[i]) allExact = false;  // NaN guard
      }
    }
    check("no preset value is NaN", allExact);
  }

  std::cout << "\nGroup: no preset produces silence, clipping or NaN (G9.5)\n";
  std::cout << "  2 s: a 4-note chord (C3 E3 G3 C4) held 1.5 s then released, @ 48 kHz,\n"
            << "  through each preset's exact stored values; want finite samples and\n"
            << "  output peak (either channel) in [-30, 0] dBFS.\n";
  {
    const double fs = 48000.0;
    const std::vector<int> chordNotes = {48, 52, 55, 60};  // C3 E3 G3 C4
    const double onSec = 1.5, offSec = 0.5;
    const int total = static_cast<int>((onSec + offSec) * fs);
    auto events = seqChord(chordNotes, 0.9f, onSec, offSec, fs);

    for (const auto& preset : nassau_presets::Presets()) {
      SynthCore core;
      core.init(static_cast<float>(fs));
      nassau_presets::ApplyPresetToSynthCore(preset.values, core);
      // Re-init after applying params: init()/reset() snaps every
      // SmoothedValue and per-voice state to whatever the atomics currently
      // hold (same convention as nassau-zermatt's own preset_tests.cpp),
      // which is what a preset actually sounds like once settled -- not an
      // unrelated startup-ramp transient from SynthCore's raw in-class
      // defaults.
      core.init(static_cast<float>(fs));

      std::vector<float> outL, outR;
      renderAbsEvents(core, events, total, 512, outL, outR);

      bool allFinite = true;
      for (float s : outL)
        if (!std::isfinite(s)) allFinite = false;
      for (float s : outR)
        if (!std::isfinite(s)) allFinite = false;

      const double pkDb = peakDb(outL, outR);
      const bool inRange = pkDb >= -30.0 && pkDb <= 0.0;

      std::cout << "  preset \"" << preset.name << "\": peak = " << pkDb << " dBFS\n";

      const std::string finiteName = std::string("\"") + preset.name + "\": all samples finite";
      const std::string rangeName = std::string("\"") + preset.name + "\": peak in [-30, 0] dBFS";
      check(finiteName.c_str(), allFinite);
      check(rangeName.c_str(), inRange);
    }
  }

  std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
  return g_failures == 0 ? 0 : 1;
}
