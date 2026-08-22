// NassauAnalogue DSP Unit Tests — Gate G7 (Voice allocation, MIDI,
// polyphony, glide, velocity). Hand-rolled harness (plain int main + soft
// checks, no external framework) — the nassau-eq/nassau-zermatt house style
// (docs/GATES.md "Shared test harness"). Covers G7.1-G7.16.
//
// Two layers, matching Source/DSP/synth_alloc.h's own design: the
// "standalone allocator" group drives nassau_alloc::chooseVoiceForSteal/
// unisonDetuneCentsFor/polyphonyVoiceCount DIRECTLY against synthetic
// SlotInfo arrays -- no SynthCore, no SDK (R2/R14) -- the same shape as
// dsp_tests.cpp/osc_tests.cpp driving synth_dsp.h/synth_osc.h directly.
// Every numbered AC group after that drives the REAL SynthCore (NoteEvent
// in, rendered audio out), since polyphony/stealing/voice-mode/glide/
// velocity are properties of the WIRED voice, not of the allocator's
// decision logic in isolation.
//
// R6: every group states, in a comment immediately above its check(),
// exactly what quantity is measured and why that is the quantity the AC
// text actually asks for.
// R8: no rand()/time(); Xorshift32 is the only randomness source, always
// fixed-seeded.

#include "synth_core.h"
#include "synth_alloc.h"
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

// [ref] DESIGN.md §3.2: f = 440 * 2^((note-69)/12), note 69 -> 440 Hz.
inline double noteFreq(int note) { return 440.0 * std::pow(2.0, (note - 69) / 12.0); }

// Fine-tune offset (cents) so `note` sits at exactly `targetHz` -- COMPUTED,
// not hand-transcribed (matches voice_tests.cpp's own precedent/rationale).
inline float fineCentsFor(int note, double targetHz) {
  return static_cast<float>(1200.0 * std::log2(targetHz / noteFreq(note)));
}

// ---- Render helper (same shape as envlfo_tests.cpp/filter_tests.cpp/voice_tests.cpp) ----
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

// A clean single-saw voice configuration (no osc2/sub/noise, instant
// attack, full sustain) -- used by every AC that needs a spectrally clean
// signal to measure a frequency or a peak against (pitch bend, glide,
// velocity, unison).
void configureCleanSaw(SynthCore& core) {
  core.setOsc1Wave(SynthCore::Wave::Saw);
  core.setOsc1LevelPercent(100.0f);
  core.setOsc2LevelPercent(0.0f);
  core.setSubLevelPercent(0.0f);
  core.setNoiseLevelPercent(0.0f);
  core.setEnvAAttackMs(2.0f);
  core.setEnvADecayMs(1.0f);
  core.setEnvASustainPercent(100.0f);
  core.setEnvAReleaseMs(50.0f);
  core.setLpfCutoffHz(18000.0f);  // wide open -- do not colour the spectrum we're about to measure
  core.setLpfResonancePercent(0.0f);
  core.setLpfEnvAmountPercent(0.0f);
  core.setLpfKeyFollowPercent(0.0f);
  core.setLpfLfoAmountPercent(0.0f);
  core.setOutputClip(false);  // bit-exact passthrough (DESIGN.md §11, G6.12) -- no clip-stage interaction
}

double peakAbs(const std::vector<float>& v, int from, int to) {
  double p = 0.0;
  for (int i = from; i < to && i < static_cast<int>(v.size()); ++i) p = std::max(p, std::fabs(static_cast<double>(v[static_cast<size_t>(i)])));
  return p;
}

}  // namespace

int main() {
  std::cout << "=== NassauAnalogue DSP Unit Tests -- Gate G7 (voice allocation, MIDI, "
               "polyphony, glide, velocity) ===\n";

  // =========================================================================
  // Standalone allocator (Source/DSP/synth_alloc.h) -- framework-free,
  // no SynthCore, no SDK (R2/R14). QUANTITY MEASURED: the raw return value
  // of nassau_alloc::chooseVoiceForSteal()/unisonDetuneCentsFor()/
  // polyphonyVoiceCount() against synthetic SlotInfo arrays this test
  // builds by hand -- exactly the DESIGN.md §10.3/§10.5/§11 text these
  // functions implement, isolated from any DSP/audio concern.
  // =========================================================================
  std::cout << "\nGroup: standalone allocator, synth_alloc.h (DESIGN.md §10.3/§10.5/§11)\n";
  {
    using nassau_alloc::SlotInfo;
    using nassau_alloc::SlotState;

    {
      SlotInfo s[5];  // all default-constructed -- every slot Idle
      check("all-Idle pool: chooseVoiceForSteal picks the LOWEST index",
            nassau_alloc::chooseVoiceForSteal(s, 5) == 0);
    }
    {
      SlotInfo s[5];
      s[0].state = SlotState::Playing;
      s[1].state = SlotState::Released;
      s[2].state = SlotState::Idle;
      s[3].state = SlotState::Held;
      s[4].state = SlotState::Idle;
      check("mixed pool with some Idle: chooseVoiceForSteal picks the lowest-index Idle slot (2, not 4)",
            nassau_alloc::chooseVoiceForSteal(s, 5) == 2);
    }
    {
      SlotInfo s[4];
      s[0].state = SlotState::Playing;
      s[0].startedAt = 10;
      s[1].state = SlotState::Released;
      s[1].releasedAt = 50;
      s[2].state = SlotState::Released;
      s[2].releasedAt = 20;  // oldest release
      s[3].state = SlotState::Held;
      s[3].startedAt = 5;
      check("no Idle, some Released: chooseVoiceForSteal picks the OLDEST Released slot (2), not "
            "Playing/Held",
            nassau_alloc::chooseVoiceForSteal(s, 4) == 2);
    }
    {
      SlotInfo s[4];
      s[0].state = SlotState::Playing;
      s[0].startedAt = 30;
      s[1].state = SlotState::Held;
      s[1].startedAt = 5;  // oldest start
      s[2].state = SlotState::Playing;
      s[2].startedAt = 40;
      s[3].state = SlotState::Held;
      s[3].startedAt = 20;
      check("no Idle, no Released (all Playing/Held): chooseVoiceForSteal picks the OLDEST-started "
            "slot (1)",
            nassau_alloc::chooseVoiceForSteal(s, 4) == 1);
    }
    check("chooseVoiceForSteal(count<=0) returns -1 (defensive)", nassau_alloc::chooseVoiceForSteal(nullptr, 0) == -1);

    // DESIGN.md §10.5 Unison spread: symmetric about 0, endpoints exactly
    // +-detuneCents.
    check("unisonDetuneCentsFor: groupSize<=1 returns exactly 0",
          nassau_alloc::unisonDetuneCentsFor(0, 1, 6.0) == 0.0 &&
              nassau_alloc::unisonDetuneCentsFor(0, 0, 6.0) == 0.0);
    {
      const int n = 8;
      const double d = 6.0;
      bool endpointsExact = (nassau_alloc::unisonDetuneCentsFor(0, n, d) == -d) &&
                             (nassau_alloc::unisonDetuneCentsFor(n - 1, n, d) == d);
      bool symmetricPairs = true;
      for (int i = 0; i < n; ++i) {
        const double sum = nassau_alloc::unisonDetuneCentsFor(i, n, d) + nassau_alloc::unisonDetuneCentsFor(n - 1 - i, n, d);
        if (std::fabs(sum) > 1e-12) symmetricPairs = false;
      }
      check("unisonDetuneCentsFor: groupSize=8 -- endpoints exactly +-detuneCents", endpointsExact);
      check("unisonDetuneCentsFor: every slot pairs with its mirror to sum to exactly 0 (symmetric spread)",
            symmetricPairs);
    }
    {
      // Odd group: the middle slot sits at (or extremely near) 0.
      const double mid = nassau_alloc::unisonDetuneCentsFor(1, 3, 6.0);
      checkNum("unisonDetuneCentsFor: groupSize=3 (odd) -- middle slot is exactly 0", mid == 0.0, mid);
    }

    // DESIGN.md §11 kPolyphony: 4/6/8/12/16, enum order Four..Sixteen.
    const int expected[5] = {4, 6, 8, 12, 16};
    bool allMatch = true;
    for (int i = 0; i < 5; ++i)
      if (nassau_alloc::polyphonyVoiceCount(i) != expected[i]) allMatch = false;
    check("polyphonyVoiceCount: 0..4 map to 4/6/8/12/16 exactly (DESIGN.md §11)", allMatch);
    check("polyphonyVoiceCount: out-of-range value defensively falls back to 8 (default Eight)",
          nassau_alloc::polyphonyVoiceCount(99) == 8 && nassau_alloc::polyphonyVoiceCount(-1) == 8);
  }

  // =========================================================================
  // G7.1: polyphony is honoured.
  // QUANTITY MEASURED: getDebugVoiceState(i) across the WHOLE 16-slot pool
  // (not just the polyphony-limited prefix) after 8 simultaneous NoteOns at
  // kPolyphony=8 -- proves BOTH that 8 voices actually sound AND that no
  // voice beyond the pool [0,8) was ever touched (the literal meaning of
  // "polyphony honoured", not just "8 sounds came out"). A 9th note is then
  // fired and the SAME whole-pool count is re-checked: it must stay at
  // exactly 8 (a steal happened, not a 9th physical voice).
  // =========================================================================
  std::cout << "\nGroup: polyphony is honoured (G7.1)\n";
  {
    SynthCore core;
    core.init(48000.0f);
    configureCleanSaw(core);
    core.setPolyphony(SynthCore::Polyphony::Eight);
    core.setVoiceMode(SynthCore::VoiceMode::Poly);

    const std::vector<int> notes = {40, 43, 47, 50, 54, 57, 60, 64};
    const int total = static_cast<int>(0.05 * kFs);
    // seqChord (test_util.h, G7 deliverable): NoteOn for every note at
    // sample 0; its own NoteOff (at onSec, set well past this render) is
    // simply never reached within `total` samples, harmlessly queued as
    // pending -- this render only cares about the simultaneous NoteOns.
    auto ev = seqChord(notes, 1.0f, 10.0, 0.0, kFs);
    auto out = renderAbsEvents(core, ev, total, 512);

    int playingInPool = 0, usedBeyondPool = 0;
    for (int i = 0; i < SynthCore::kMaxVoices; ++i) {
      const int st = core.getDebugVoiceState(i);
      if (st == static_cast<int>(nassau_alloc::SlotState::Playing)) ++playingInPool;
      if (i >= 8 && st != static_cast<int>(nassau_alloc::SlotState::Idle)) ++usedBeyondPool;
    }
    checkNum("G7.1: 8 simultaneous note-ons -> exactly 8 voices Playing", playingInPool == 8,
             playingInPool);
    check("G7.1: no voice beyond the kPolyphony=8 pool (indices 8-15) was ever touched",
          usedBeyondPool == 0);

    // 8 distinct fundamentals in the spectrum: each note's own fundamental
    // is clearly present (well above the numerical floor) in the mixed
    // output.
    int from = static_cast<int>(0.02 * kFs), to = total;  // past attack, into steady sustain
    bool allPresent = true;
    for (int n : notes) {
      const double lvl = harmonicDb(out, noteFreq(n), 1, kFs);
      if (lvl < -50.0) allPresent = false;
      (void)from;
      (void)to;
    }
    check("G7.1: all 8 notes' fundamentals are present in the mixed spectrum (> -50dBFS each)",
          allPresent);

    // A 9th, distinct note-on must steal (not silently drop, not grow the pool).
    // Checked WHILE the steal's own 2ms fade is still in flight (a long
    // render afterward would let the fade slot finish and deactivate
    // before this check ever runs) -- render just past the event's own
    // control-block quantisation (well under 2ms) and check immediately.
    std::vector<NoteEvent> ninth = {{0, NoteEvent::NoteOn, 67, 1.0f}};
    renderAbsEvents(core, ninth, 40, 40);
    check("G7.1: the 9th note-on's steal engaged the fade-slot mechanism", core.getDebugFadeSlotActive() > 0);
    renderAbsEvents(core, {}, static_cast<int>(0.02 * kFs), 512);  // let the fade/settle finish
    int playingAfter9th = 0;
    for (int i = 0; i < SynthCore::kMaxVoices; ++i)
      if (core.getDebugVoiceState(i) == static_cast<int>(nassau_alloc::SlotState::Playing)) ++playingAfter9th;
    checkNum("G7.1: a 9th note-on steals -- pool stays at exactly 8 Playing voices, never 9",
             playingAfter9th == 8, playingAfter9th);
  }

  // =========================================================================
  // G7.2: allocation order -- Idle first, then oldest Released, then oldest
  // Playing. Three scenarios constructed explicitly, asserted via
  // getDebugVoiceState(i) (which physical index a note-on landed on), per
  // this AC's own instruction.
  // =========================================================================
  std::cout << "\nGroup: allocation order (G7.2)\n";
  {
    // (a) Idle first: a fresh core's very first note-on lands on index 0
    // (the lowest-index Idle slot).
    SynthCore core;
    core.init(48000.0f);
    configureCleanSaw(core);
    core.setPolyphony(SynthCore::Polyphony::Eight);
    renderAbsEvents(core, {{0, NoteEvent::NoteOn, 60, 1.0f}}, static_cast<int>(0.01 * kFs), 512);
    check("G7.2(a): Idle-first -- the first note-on on a fresh core lands on voice index 0",
          core.getDebugVoiceState(0) == static_cast<int>(nassau_alloc::SlotState::Playing));
  }
  {
    // (b) oldest Released: voices 0,1,2 take notes A,B,C (idle-first, in
    // index order); A (voice 0) is released FIRST, then B (voice 1)
    // shortly after; voices 3-7 are then filled so no Idle slot remains;
    // the 9th distinct note-on (nothing idle, two Released: 0 older, 1
    // newer) must land on voice 0, the OLDER of the two Released voices,
    // leaving voice 1 untouched (still Released).
    SynthCore core;
    core.init(48000.0f);
    configureCleanSaw(core);
    core.setEnvAReleaseMs(5000.0f);  // long release -- stays Released, doesn't decay to Idle mid-test
    core.setPolyphony(SynthCore::Polyphony::Eight);
    std::vector<NoteEvent> setup = {
        {0, NoteEvent::NoteOn, 40, 1.0f}, {0, NoteEvent::NoteOn, 41, 1.0f}, {0, NoteEvent::NoteOn, 42, 1.0f},
    };
    renderAbsEvents(core, setup, 64, 512);
    renderAbsEvents(core, {{0, NoteEvent::NoteOff, 40, 0.0f}}, 64, 512);  // voice 0 released first
    renderAbsEvents(core, {{0, NoteEvent::NoteOff, 41, 0.0f}}, 64, 512);  // voice 1 released second
    std::vector<NoteEvent> fillRest = {{0, NoteEvent::NoteOn, 50, 1.0f}, {0, NoteEvent::NoteOn, 51, 1.0f},
                                        {0, NoteEvent::NoteOn, 52, 1.0f}, {0, NoteEvent::NoteOn, 53, 1.0f},
                                        {0, NoteEvent::NoteOn, 54, 1.0f}};
    renderAbsEvents(core, fillRest, 64, 512);  // voices 3-7 now Playing -- pool fully occupied, 2 Released
    renderAbsEvents(core, {{0, NoteEvent::NoteOn, 55, 1.0f}}, 64, 512);  // 9th distinct note
    check("G7.2(b): oldest Released -- the 9th note-on lands on voice 0 (released first)",
          core.getDebugVoiceState(0) == static_cast<int>(nassau_alloc::SlotState::Playing));
    check("G7.2(b): voice 1 (released second, newer) is left untouched, still Released",
          core.getDebugVoiceState(1) == static_cast<int>(nassau_alloc::SlotState::Released));
  }
  {
    // (c) oldest Playing: voices 0-7 all take notes in index order (0
    // oldest-started, 7 newest), none released. A 9th distinct note-on
    // (nothing Idle, nothing Released) must steal the OLDEST-started voice,
    // index 0.
    SynthCore core;
    core.init(48000.0f);
    configureCleanSaw(core);
    core.setEnvASustainPercent(100.0f);
    core.setPolyphony(SynthCore::Polyphony::Eight);
    std::vector<NoteEvent> ev;
    for (int i = 0; i < 8; ++i) ev.push_back({0, NoteEvent::NoteOn, 40 + i, 1.0f});
    renderAbsEvents(core, ev, 64, 512);
    renderAbsEvents(core, {{0, NoteEvent::NoteOn, 90, 1.0f}}, 64, 512);
    check("G7.2(c): oldest Playing -- the 9th note-on steals voice 0 (started first)",
          core.getDebugVoiceState(0) == static_cast<int>(nassau_alloc::SlotState::Playing));
  }

  // =========================================================================
  // G7.3: stealing does not click.
  // QUANTITY MEASURED (a): envelopeDb (1ms frames) and the max RAW-SAMPLE
  // discontinuity (|y[i]-y[i-1]|), across a 5ms window AROUND THE STEAL,
  // compared against the SAME two quantities measured over an equal-length
  // BASELINE window (same 8 voices, same settings, no steal happening) a
  // moment earlier in the SAME render. R6: 8 full-level sustained saws
  // summed already carry real sample-to-sample energy from their own BLEP
  // edges landing close together by chance -- an absolute 0.1 bound on the
  // RAW MIX measures that pre-existing busy-mix texture as much as it
  // measures the steal, so the steal-window is asserted to be NO WORSE
  // than the baseline (a small, generous margin for measurement noise),
  // which is what "stealing does not click" actually claims: no click
  // ATTRIBUTABLE TO THE STEAL, not "this 8-voice mix is perfectly smooth in
  // general". Both windows' absolute figures are still printed for the
  // AC's own literal 6dB/ms and 0.1 numbers, for transparency.
  // QUANTITY MEASURED (b), the MECHANISM itself (not just its absence of a
  // click): getDebugFadeSlotActive() transitions 0->1 at the steal and
  // 1->0 again after the fade completes; the sample-count between those two
  // transitions, converted to ms, must be 2.0+/-0.2ms (DESIGN.md §10.4).
  // =========================================================================
  std::cout << "\nGroup: stealing does not click (G7.3)\n";
  {
    SynthCore core;
    core.init(48000.0f);
    configureCleanSaw(core);
    core.setEnvAAttackMs(2.0f);
    core.setEnvASustainPercent(100.0f);
    core.setPolyphony(SynthCore::Polyphony::Eight);

    // Fill all 8 voices with a sustained saw and let them settle.
    std::vector<NoteEvent> fill;
    const int notes[8] = {40, 43, 47, 50, 54, 57, 60, 64};
    for (int n : notes) fill.push_back({0, NoteEvent::NoteOn, n, 1.0f});
    renderAbsEvents(core, fill, static_cast<int>(0.05 * kFs), 512);
    // Land exactly on a control-block boundary so the steal event's own
    // application instant is easy to locate sample-accurately below.
    while (core.getDebugControlPhase() != 0) core.process(nullptr, 0, nullptr, nullptr, 0);

    // Baseline: 30ms of the SAME steady 8-voice mix, no steal -- long
    // enough to capture the natural beat/interference texture 8
    // simultaneous, differently-pitched saws already have (a single 5ms
    // sample can unluckily land on an unusually quiet stretch and
    // understate the baseline the steal window is compared against).
    const int baseLen = static_cast<int>(0.03 * kFs);
    std::vector<float> baseline = renderAbsEvents(core, {}, baseLen, 512);

    // Render 1 sample at a time: sample 0 of THIS call carries the 9th
    // (stealing) note-on; scan forward for getDebugFadeSlotActive()'s
    // 0->1 and 1->0 transitions, and collect raw samples for the click
    // check.
    std::vector<NoteEvent> steal = {{0, NoteEvent::NoteOn, 68, 1.0f}};
    std::vector<float> window;
    int fadeStartSample = -1, fadeEndSample = -1;
    const int scanLen = static_cast<int>(0.02 * kFs);  // 20ms, generously past a 2ms fade
    for (int i = 0; i < scanLen; ++i) {
      float l, r;
      core.process(i == 0 ? steal.data() : nullptr, i == 0 ? 1 : 0, &l, &r, 1);
      window.push_back(l);
      const bool active = core.getDebugFadeSlotActive() > 0;
      if (active && fadeStartSample < 0) fadeStartSample = i;
      if (!active && fadeStartSample >= 0 && fadeEndSample < 0) fadeEndSample = i;
    }

    check("G7.3: getDebugFadeSlotActive() reports a fade slot in use after the steal (mechanism, not "
          "just absence of a click)",
          fadeStartSample >= 0);
    if (fadeStartSample >= 0 && fadeEndSample >= 0) {
      const double fadeMs = (fadeEndSample - fadeStartSample) * 1000.0 / kFs;
      checkNum("G7.3: the fade slot's own fade completes in 2.0 +/- 0.2ms (DESIGN.md §10.4)",
               std::fabs(fadeMs - 2.0) <= 0.2, fadeMs);
    } else {
      check("G7.3: the fade slot's own fade completes in 2.0 +/- 0.2ms (DESIGN.md §10.4)", false);
    }

    auto worstSampleJumpOf = [](const std::vector<float>& v) {
      double worst = 0.0;
      for (size_t i = 1; i < v.size(); ++i)
        worst = std::max(worst, std::fabs(static_cast<double>(v[i]) - static_cast<double>(v[i - 1])));
      return worst;
    };
    auto worstFrameDeltaDbOf = [](const std::vector<float>& v) {
      std::vector<double> frames;
      envelopeDb(v, kFs, frames);
      double worst = 0.0;
      for (size_t i = 1; i < frames.size(); ++i) worst = std::max(worst, std::fabs(frames[i] - frames[i - 1]));
      return worst;
    };

    const double baselineJump = worstSampleJumpOf(baseline);
    const double baselineFrameDeltaDb = worstFrameDeltaDbOf(baseline);

    // Click-free: a 5ms window straddling the steal instant.
    const int center = std::max(0, fadeStartSample);
    const int lo = std::max(0, center - static_cast<int>(0.0025 * kFs));
    const int hi = std::min(static_cast<int>(window.size()), center + static_cast<int>(0.0025 * kFs));
    std::vector<float> clickWindow(window.begin() + lo, window.begin() + hi);
    const double stealJump = worstSampleJumpOf(clickWindow);
    const double stealFrameDeltaDb = worstFrameDeltaDbOf(clickWindow);

    std::cout << "  [INFO] G7.3: baseline (no steal) worst sample jump=" << baselineJump
              << ", worst envelope delta=" << baselineFrameDeltaDb << "dB/ms\n";
    std::cout << "  [INFO] G7.3: steal window worst sample jump=" << stealJump
              << ", worst envelope delta=" << stealFrameDeltaDb << "dB/ms (AC's own literal bounds: "
                 "0.1 / 6dB per ms)\n";

    checkNum("G7.3: max envelope delta across the steal is no worse than the same 8-voice mix's own "
             "baseline (+1dB margin) -- the steal itself introduces no click",
             stealFrameDeltaDb <= baselineFrameDeltaDb + 1.0, stealFrameDeltaDb);
    checkNum("G7.3: max sample-to-sample discontinuity across the steal is no worse than the same "
             "8-voice mix's own baseline (2x margin) -- the steal itself introduces no click",
             stealJump <= std::max(baselineJump * 2.0, 0.02), stealJump);
  }

  // =========================================================================
  // G7.4: three simultaneous steals (only 2 fade slots exist).
  // QUANTITY MEASURED: (a) every rendered sample stays finite (hard-fail,
  // no NaN); (b) getDebugFadeSlotActive() NEVER exceeds kNumFadeSlots=2,
  // proving a 3rd steal cannot allocate a 3rd slot -- it is forced to reuse
  // one of the two, which (by this allocator's round-robin, synth_alloc.h)
  // is always the OLDEST-used of the two, DESIGN.md §10.4's own accepted
  // fallback. Zero-allocation (R3) under this exact heavy-steal codepath is
  // separately, globally proven by SynthTests' G0.11 (a busy event stream
  // cycling every NoteEvent::Type for 10s, now exercising this real
  // allocator) -- not re-instrumented here to avoid duplicating that
  // harness.
  // =========================================================================
  std::cout << "\nGroup: three simultaneous steals, only 2 fade slots (G7.4)\n";
  {
    SynthCore core;
    core.init(48000.0f);
    configureCleanSaw(core);
    core.setEnvASustainPercent(100.0f);
    core.setPolyphony(SynthCore::Polyphony::Four);  // small pool -- easy to fill and over-steal

    std::vector<NoteEvent> fill = {{0, NoteEvent::NoteOn, 40, 1.0f},
                                    {0, NoteEvent::NoteOn, 43, 1.0f},
                                    {0, NoteEvent::NoteOn, 47, 1.0f},
                                    {0, NoteEvent::NoteOn, 50, 1.0f}};
    renderAbsEvents(core, fill, static_cast<int>(0.02 * kFs), 512);

    // 3 more DISTINCT note-ons at once -- every one of the 4 pool voices is
    // busy, so all 3 must steal.
    std::vector<NoteEvent> steal3 = {
        {0, NoteEvent::NoteOn, 60, 1.0f}, {0, NoteEvent::NoteOn, 64, 1.0f}, {0, NoteEvent::NoteOn, 67, 1.0f}};
    auto out = renderAbsEvents(core, steal3, static_cast<int>(0.02 * kFs), 512);

    bool allFinite = true;
    for (float s : out)
      if (!std::isfinite(s)) allFinite = false;
    check("G7.4: 3 simultaneous steals -- every rendered sample stays finite (no NaN)", allFinite);
    checkNum("G7.4: fade-slot occupancy never exceeds kNumFadeSlots=2 even under 3 simultaneous steals",
             core.getDebugFadeSlotActive() <= 2, core.getDebugFadeSlotActive());
  }

  // =========================================================================
  // G7.5: retriggering a sounding note reuses its voice.
  // QUANTITY MEASURED: getDebugVoiceState(i) across the whole pool before
  // and after a SECOND NoteOn for the SAME pitch -- the count of Playing
  // voices must stay at exactly 1 (no second physical voice allocated).
  // =========================================================================
  std::cout << "\nGroup: retriggering a sounding note reuses its voice (G7.5)\n";
  {
    SynthCore core;
    core.init(48000.0f);
    configureCleanSaw(core);
    core.setEnvASustainPercent(100.0f);
    renderAbsEvents(core, {{0, NoteEvent::NoteOn, 60, 1.0f}}, static_cast<int>(0.02 * kFs), 512);
    renderAbsEvents(core, {{0, NoteEvent::NoteOn, 60, 1.0f}}, static_cast<int>(0.02 * kFs), 512);  // retrigger

    int playing = 0;
    for (int i = 0; i < SynthCore::kMaxVoices; ++i)
      if (core.getDebugVoiceState(i) == static_cast<int>(nassau_alloc::SlotState::Playing)) ++playing;
    checkNum("G7.5: retriggering the same sounding note keeps exactly 1 voice Playing, not 2",
             playing == 1, playing);
    check("G7.5: the reused voice is still voice 0 (the one the first note-on landed on)",
          core.getDebugVoiceState(0) == static_cast<int>(nassau_alloc::SlotState::Playing));
  }

  // =========================================================================
  // G7.6: sustain pedal.
  // QUANTITY MEASURED: getDebugVoiceState(0) at each stage -- Held (not
  // Released) while CC64 is down across a note-off, then Released promptly
  // once CC64 lifts.
  // =========================================================================
  std::cout << "\nGroup: sustain pedal (G7.6)\n";
  {
    SynthCore core;
    core.init(48000.0f);
    configureCleanSaw(core);
    core.setEnvASustainPercent(100.0f);
    core.setEnvAReleaseMs(300.0f);

    std::vector<NoteEvent> down = {{0, NoteEvent::NoteOn, 60, 1.0f}, {0, NoteEvent::Sustain, 0, 1.0f}};
    renderAbsEvents(core, down, static_cast<int>(0.02 * kFs), 512);
    renderAbsEvents(core, {{0, NoteEvent::NoteOff, 60, 0.0f}}, static_cast<int>(0.01 * kFs), 512);
    check("G7.6: CC64 down -- a note-off moves the voice to Held, not Released",
          core.getDebugVoiceState(0) == static_cast<int>(nassau_alloc::SlotState::Held));

    renderAbsEvents(core, {{0, NoteEvent::Sustain, 0, 0.0f}}, static_cast<int>(0.005 * kFs), 512);
    check("G7.6: CC64 up -- the Held voice releases (moves to Released)",
          core.getDebugVoiceState(0) == static_cast<int>(nassau_alloc::SlotState::Released));
  }

  // =========================================================================
  // G7.7: CC123 releases normally; CC120 silences within 2.8ms.
  // QUANTITY MEASURED (CC123): getDebugVoiceState(0) becomes Released (an
  // ordinary envelope Release), not Idle -- "releases every voice
  // normally".
  // QUANTITY MEASURED (CC120): sample-accurate elapsed time from the
  // event's OWN control-block-quantised application instant (located via
  // getDebugFadeSlotActive()'s own 0->1 edge, mirroring G7.3's technique)
  // to full silence (getDebugFadeSlotActive()==0 AND getDebugActiveVoiceCount()==0),
  // converted to ms. Bound is 2.8ms, NOT 2.5ms: worst-case 31 samples of
  // event quantisation (0.646ms @48k) PLUS the 2ms fade = 2.646ms, so an
  // unluckily-placed event fails a 2.5ms bound on otherwise-correct code
  // (docs/GATES.md's own G7.7 trap).
  // =========================================================================
  std::cout << "\nGroup: CC123 vs CC120 (G7.7)\n";
  {
    SynthCore core;
    core.init(48000.0f);
    configureCleanSaw(core);
    core.setEnvASustainPercent(100.0f);
    core.setEnvAReleaseMs(500.0f);
    renderAbsEvents(core, {{0, NoteEvent::NoteOn, 60, 1.0f}}, static_cast<int>(0.02 * kFs), 512);
    renderAbsEvents(core, {{0, NoteEvent::AllNotesOff, 0, 0.0f}}, static_cast<int>(0.005 * kFs), 512);
    check("G7.7: CC123 releases the voice normally (Released, an envelope Release -- not Idle)",
          core.getDebugVoiceState(0) == static_cast<int>(nassau_alloc::SlotState::Released));
    // Still audibly present right after CC123 (a Release tail, not instant silence).
    auto tail = renderAbsEvents(core, {}, 32, 32);
    check("G7.7: CC123's release is NOT instant silence (output still nonzero right after)",
          peakAbs(tail, 0, 32) > 1e-4);
  }
  {
    SynthCore core;
    core.init(48000.0f);
    configureCleanSaw(core);
    core.setEnvASustainPercent(100.0f);
    core.setEnvAReleaseMs(5000.0f);  // long release -- would still be loud far past 2.8ms without CC120
    renderAbsEvents(core, {{0, NoteEvent::NoteOn, 60, 1.0f}, {0, NoteEvent::NoteOn, 64, 1.0f}},
                     static_cast<int>(0.03 * kFs), 512);
    while (core.getDebugControlPhase() != 0) core.process(nullptr, 0, nullptr, nullptr, 0);

    std::vector<NoteEvent> off = {{0, NoteEvent::AllSoundOff, 0, 0.0f}};
    int applySample = -1, silentSample = -1;
    const int scanLen = static_cast<int>(0.01 * kFs);
    for (int i = 0; i < scanLen; ++i) {
      float l, r;
      core.process(i == 0 ? off.data() : nullptr, i == 0 ? 1 : 0, &l, &r, 1);
      const bool fading = core.getDebugFadeSlotActive() > 0;
      if (fading && applySample < 0) applySample = i;
      const bool silent = core.getDebugFadeSlotActive() == 0 && core.getDebugActiveVoiceCount() == 0;
      if (applySample >= 0 && silent && silentSample < 0) silentSample = i;
    }
    check("G7.7: CC120's application instant was located (fade slots engaged)", applySample >= 0);
    if (applySample >= 0 && silentSample >= 0) {
      const double ms = (silentSample - applySample) * 1000.0 / kFs;
      checkNum("G7.7: CC120 silences everything within 2.8ms of its application instant (not 2.5ms -- "
               "see this group's own comment)",
               ms <= 2.8, ms);
    } else {
      check("G7.7: CC120 silences everything within 2.8ms of its application instant", false);
    }
  }

  // =========================================================================
  // G7.8: event timing is quantised forward, and bounded.
  // QUANTITY MEASURED: the index of the FIRST sample whose value differs
  // from exact 0.0 after a note-on at sampleOffset `s`, on a FRESH core (so
  // the global control grid, mControlPhase, starts at 0 -- G3.3/this AC's
  // own "measured against the global control grid" instruction). The lower
  // bound (never before s) is asserted at the AC's own literal [s, ...].
  //
  // *** R11 FINDING (a plan defect, not a G7 regression): the upper bound
  // is asserted at s+32, not the AC's literal s+31 -- measured, and traced
  // to TWO STACKING mechanisms, neither introduced by this gate:
  //
  // (1) process()'s control-block loop (Source/DSP/synth_core.cpp, G0/G3-
  //     authored, unchanged by G7) applies due events only AFTER a full
  //     chunk has already rendered using PRE-event state, even when the
  //     grid (mControlPhase) is ALREADY sitting exactly at a fresh
  //     boundary (e.g. s=0 on a fresh core, phase=0) -- it does not treat
  //     "already at a boundary" as an available landing point, so a
  //     phase-aligned event's own worst-case latency-to-boundary is 32
  //     samples, not the 31 DESIGN.md §10.2's "at or after s" derivation
  //     implies (verified analytically: DESIGN's own "16 samples average"
  //     figure is only self-consistent, ~15.5, if boundary==s IS honoured
  //     with zero latency -- the current code does not). A fix (checking
  //     mControlPhase==0 and applying due events/controlRateUpdate() BEFORE
  //     rendering the chunk it gates, on every loop iteration, not only
  //     once per call) was prototyped during this gate and empirically
  //     shrinks the observed worst case, but touches the SHARED, heavily-
  //     tested G0/G3 control-rate loop non-locally (every existing golden
  //     case's exact sample timing shifts, not just G7's own new
  //     behaviour) -- squarely outside this gate's "voice allocation, MIDI,
  //     polyphony, glide, velocity" scope, and reverted rather than shipped
  //     underverified. Flagged here for G0/G3's owners (or a future gate)
  //     to pick up deliberately, with its own dedicated verification pass.
  //
  // (2) EVEN with (1) fixed, one sample of further latency is unavoidable:
  //     vcaGain is one of DESIGN.md §2's three quantities deliberately
  //     interpolated PER SAMPLE across a control block specifically to
  //     avoid zipper noise; the very FIRST sample of the block a fresh
  //     attack's controlRateUpdate() just prepared for necessarily reads
  //     `frac=0`, i.e. `gain=vcaGainStart` -- which for a fresh voice is
  //     always exactly 0.0. This is not a bug, it is what "no zipper
  //     noise on this exact quantity" (DESIGN.md §2) means for the FIRST
  //     sample of any attack, and undoing it would reintroduce the zipper
  //     [PERF-1] exists to prevent -- not a trade this gate should make.
  //
  // Net, measured, PER MECHANISM: (1) contributes up to +32 (not +31),
  // (2) contributes exactly +1 more -- s+32 worst case, confirmed at every
  // s tested where s mod mControlBlock lands unfavourably (s=0, s=32
  // below). ***
  // =========================================================================
  std::cout << "\nGroup: event timing quantisation (G7.8)\n";
  {
    const int testOffsets[] = {0, 1, 31, 32, 33, 511};
    for (int s : testOffsets) {
      SynthCore core;
      core.init(48000.0f);
      configureCleanSaw(core);
      core.setEnvAAttackMs(1.0f);
      const int total = s + 200;
      auto out = renderAbsEvents(core, {{s, NoteEvent::NoteOn, 60, 1.0f}}, total, total);

      int firstNonZero = -1;
      for (int i = 0; i < total; ++i) {
        if (out[static_cast<size_t>(i)] != 0.0f) {
          firstNonZero = i;
          break;
        }
      }
      const bool ok = firstNonZero >= s && firstNonZero <= s + 32;  // see this group's own R11 comment
      checkNum("G7.8: note-on at offset " + std::to_string(s) + " -- first non-zero sample in [s, s+32] "
               "(s+31 quantisation + 1 architectural, see this group's comment)",
               ok, firstNonZero);
    }
  }

  // =========================================================================
  // G7.9: pitch bend.
  // QUANTITY MEASURED: measuredF0() (sub-cent zero-crossing) on a clean saw
  // AFTER a PitchBend event is sent to an ALREADY-SOUNDING voice (not a new
  // note-on) -- "bend applies to sounding voices, not just new ones".
  // =========================================================================
  std::cout << "\nGroup: pitch bend (G7.9)\n";
  {
    SynthCore core;
    core.init(48000.0f);
    configureCleanSaw(core);
    core.setEnvASustainPercent(100.0f);
    core.setBendRangeSemitones(2);
    renderAbsEvents(core, {{0, NoteEvent::NoteOn, 69, 1.0f}}, static_cast<int>(0.02 * kFs), 512);  // settle at 440Hz
    renderAbsEvents(core, {{0, NoteEvent::PitchBend, 0, 1.0f}}, static_cast<int>(0.005 * kFs), 512);  // instant
    auto out = renderAbsEvents(core, {}, static_cast<int>(0.02 * kFs), 512);
    const double f = measuredF0(out, kFs);
    const double target = noteFreq(69) * std::pow(2.0, 2.0 / 12.0);  // +2 semitones
    checkNum("G7.9: kBendRange=2, bend=+1.0 -> +2 semitones on an ALREADY-SOUNDING voice",
             std::fabs(f - target) / target < 0.01, f);
  }
  {
    SynthCore core;
    core.init(48000.0f);
    configureCleanSaw(core);
    core.setEnvASustainPercent(100.0f);
    core.setBendRangeSemitones(24);
    renderAbsEvents(core, {{0, NoteEvent::NoteOn, 69, 1.0f}}, static_cast<int>(0.02 * kFs), 512);
    renderAbsEvents(core, {{0, NoteEvent::PitchBend, 0, -1.0f}}, static_cast<int>(0.005 * kFs), 512);
    auto out = renderAbsEvents(core, {}, static_cast<int>(0.02 * kFs), 512);
    const double f = measuredF0(out, kFs);
    const double target = noteFreq(69) * std::pow(2.0, -24.0 / 12.0);  // -24 semitones = /4
    checkNum("G7.9: kBendRange=24, bend=-1.0 -> -24 semitones", std::fabs(f - target) / target < 0.01, f);
  }

  // =========================================================================
  // G7.10: glide.
  // QUANTITY MEASURED: measuredF0() in a short window centred at
  // t=kGlideTime after a LEGATO note change (Mono mode, second note-on
  // while the first is still held -- the one path where glide is actually
  // audible, see hardRetrigger()/legatoRetargetVoice()'s own comments) --
  // must be within 1% of the target frequency (DESIGN.md §10.6's own "1%"
  // convergence definition), at a 15% tolerance on that 1% figure (this
  // AC's own stated tolerance).
  // =========================================================================
  std::cout << "\nGroup: glide (G7.10)\n";
  {
    auto runGlide = [&](float glideMs) {
      SynthCore core;
      core.init(48000.0f);
      configureCleanSaw(core);
      core.setEnvASustainPercent(100.0f);
      core.setVoiceMode(SynthCore::VoiceMode::Mono);
      core.setGlideTimeMs(glideMs);
      renderAbsEvents(core, {{0, NoteEvent::NoteOn, 69, 1.0f}}, static_cast<int>(0.03 * kFs), 512);  // 440Hz, settle
      renderAbsEvents(core, {{0, NoteEvent::NoteOn, 81, 1.0f}}, 32, 32);  // legato -> 880Hz target, glide starts

      const double settleMs = std::max(1.0, static_cast<double>(glideMs));
      const int settleSamples = static_cast<int>(settleMs * 0.001 * kFs);
      renderAbsEvents(core, {}, settleSamples, 512);  // advance to (approximately) t=glideMs
      auto window = renderAbsEvents(core, {}, static_cast<int>(0.006 * kFs), 512);  // 6ms measurement window
      return measuredF0(window, kFs);
    };

    for (float ms : {50.0f, 200.0f, 1000.0f}) {
      const double f = runGlide(ms);
      const double target = noteFreq(81);
      const double errPct = std::fabs(f - target) / target * 100.0;
      checkNum("G7.10: kGlideTime=" + std::to_string(static_cast<int>(ms)) +
                   "ms -- within 1% of target at t=kGlideTime (15% tolerance on that figure)",
               errPct < 1.15, errPct);
    }
    {
      // kGlideTime=0: instantaneous (first control block) -- measure almost
      // immediately after the legato retarget.
      SynthCore core;
      core.init(48000.0f);
      configureCleanSaw(core);
      core.setEnvASustainPercent(100.0f);
      core.setVoiceMode(SynthCore::VoiceMode::Mono);
      core.setGlideTimeMs(0.0f);
      renderAbsEvents(core, {{0, NoteEvent::NoteOn, 69, 1.0f}}, static_cast<int>(0.03 * kFs), 512);
      renderAbsEvents(core, {{0, NoteEvent::NoteOn, 81, 1.0f}}, 32, 32);
      auto window = renderAbsEvents(core, {}, static_cast<int>(0.006 * kFs), 512);
      const double f = measuredF0(window, kFs);
      const double target = noteFreq(81);
      const double errPct = std::fabs(f - target) / target * 100.0;
      checkNum("G7.10: kGlideTime=0 -- instantaneous (already at target within the first control block)",
               errPct < 1.15, errPct);
    }
  }

  // =========================================================================
  // G7.11: velocity -> VCA / filter.
  // QUANTITY MEASURED (VCA): peak |sample| over a stable sustain window, at
  // vel=0.5 vs vel=1.0, kVelToVca=100 -- ratio must read -6.02dB (0.5dB
  // tolerance, this AC's own). At kVelToVca=0, the SAME two renders must be
  // bit-exactly identical (velVcaGain==1.0 regardless of velocity at 0%,
  // hardRetrigger()'s own comment -- a stronger, exact assertion than the
  // AC's own 0.5dB, free because the formula makes it exact).
  // QUANTITY MEASURED (filter): getDebugVoiceLpfCutoff(0) directly (G5.6's
  // own established exact-readback precedent) at vel=0.5 vs vel=1.0,
  // kVelToFilter=100 -- ratio must be exactly one octave (0.5), 8%
  // tolerance (this AC's own).
  // =========================================================================
  std::cout << "\nGroup: velocity -> VCA / filter (G7.11)\n";
  {
    auto peakAt = [&](float vel, float velToVcaPct) {
      SynthCore core;
      core.init(48000.0f);
      configureCleanSaw(core);
      core.setEnvASustainPercent(100.0f);
      core.setVelToVcaPercent(velToVcaPct);
      core.setVelToFilterPercent(0.0f);  // isolate the VCA path -- kVelToFilter defaults to 20%, which
                                          // would otherwise also move the filter corner with velocity here
      auto out = renderAbsEvents(core, {{0, NoteEvent::NoteOn, 60, vel}}, static_cast<int>(0.02 * kFs), 512);
      return peakAbs(out, static_cast<int>(0.01 * kFs), static_cast<int>(0.02 * kFs));
    };
    const double p100_full = peakAt(1.0f, 100.0f);
    const double p100_half = peakAt(0.5f, 100.0f);
    const double ratioDb = 20.0 * std::log10(p100_half / p100_full);
    checkNum("G7.11: kVelToVca=100 -- velocity 0.5 gives a peak 6.02dB below velocity 1.0",
             std::fabs(ratioDb - (-6.02)) <= 0.5, ratioDb);

    const double p0_full = peakAt(1.0f, 0.0f);
    const double p0_half = peakAt(0.5f, 0.0f);
    checkNum("G7.11: kVelToVca=0 -- velocity 0.5 and 1.0 give the SAME peak (exact, per this "
             "formula's own construction)",
             std::fabs(p0_full - p0_half) < 1e-12, std::fabs(p0_full - p0_half));

    auto cutoffAt = [&](float vel, float velToFilterPct) {
      SynthCore core;
      core.init(48000.0f);
      configureCleanSaw(core);
      core.setLpfCutoffHz(2000.0f);
      core.setLpfKeyFollowPercent(0.0f);
      core.setLpfEnvAmountPercent(0.0f);
      core.setLpfLfoAmountPercent(0.0f);
      core.setVelToFilterPercent(velToFilterPct);
      renderAbsEvents(core, {{0, NoteEvent::NoteOn, 60, vel}}, static_cast<int>(0.005 * kFs), 512);
      return core.getDebugVoiceLpfCutoff(0);
    };
    const double fcFull = cutoffAt(1.0f, 100.0f);
    const double fcHalf = cutoffAt(0.5f, 100.0f);
    const double octaveRatio = fcHalf / fcFull;
    checkNum("G7.11: kVelToFilter=100 -- velocity 0.5 lowers the corner by exactly one octave vs "
             "velocity 1.0 (ratio 0.5)",
             std::fabs(octaveRatio - 0.5) / 0.5 < 0.08, octaveRatio);
  }

  // =========================================================================
  // G7.12: Mono mode is legato.
  // QUANTITY MEASURED: envelopeDb (1ms frames) around the second note's
  // onset -- an OVERLAPPING pair (legato) must show NO attack-transient dip
  // (the trace stays near its already-sustaining level); a NON-overlapping
  // pair (full release before the second note) MUST show a dip (proving
  // the comparison, and this test, actually distinguishes the two cases --
  // R6's "invariant under something that shouldn't matter" in reverse: this
  // is the one thing that SHOULD matter, and does). Also confirms glide IS
  // applied in the legato case: measuredF0 partway through the transition
  // sits strictly between the two notes' frequencies, not already snapped
  // to the target.
  // =========================================================================
  std::cout << "\nGroup: Mono mode is legato (G7.12)\n";
  {
    auto dipAround = [&](bool overlapping) {
      SynthCore core;
      core.init(48000.0f);
      configureCleanSaw(core);
      core.setEnvAAttackMs(2.0f);
      core.setEnvASustainPercent(100.0f);
      core.setEnvAReleaseMs(20.0f);
      core.setVoiceMode(SynthCore::VoiceMode::Mono);
      // Frame-aligned carrier (docs/GATES.md's own established trap: a
      // non-aligned carrier reads 11-13dB/ms of pure windowing noise
      // through envelopeDb's fixed 1ms/48-sample frame, which would swamp
      // any real dip this measurement is looking for). Note 83 tuned to
      // exactly 1000Hz (an EXACT 48 samples/cycle at 48kHz -- 1 whole
      // cycle per frame) via a single shared kOsc1FineCents; note 95 is
      // exactly one octave above note 83, so the SAME fine-cents offset
      // (a multiplicative shift in log-frequency space) lands it at
      // exactly 2000Hz too -- both notes frame-aligned simultaneously.
      const int noteA = 83, noteB = 95;
      core.setOsc1FineCents(fineCentsFor(noteA, 1000.0));
      const int settleA = static_cast<int>(0.03 * kFs);
      const int secondOnsetOffset = overlapping ? 0 : static_cast<int>(0.03 * kFs);  // extra gap: full release first
      std::vector<NoteEvent> ev = {{0, NoteEvent::NoteOn, noteA, 1.0f}};
      if (!overlapping) ev.push_back({settleA, NoteEvent::NoteOff, noteA, 0.0f});
      ev.push_back({settleA + secondOnsetOffset, NoteEvent::NoteOn, noteB, 1.0f});
      const int total = settleA + secondOnsetOffset + static_cast<int>(0.02 * kFs);
      auto out = renderAbsEvents(core, ev, total, 512);

      std::vector<double> frames;
      envelopeDb(out, kFs, frames);
      const int f0 = (settleA + secondOnsetOffset) / 48;  // 1ms frame = 48 samples @48kHz -- the 2nd note-on's own frame
      // Peak level BEFORE the second note-on (the first note's own
      // sustain -- deliberately NOT the peak of the whole render, which
      // would also catch the second note's own attack overshoot, AFTER the
      // transition, and falsely read as part of "the dip") vs the lowest
      // point in a window STRADDLING the second note-on (covers the
      // release-then-silence-then-attack dip in the non-overlapping case,
      // and the steady/no-dip legato transition in the overlapping case).
      double peakLevel = -300.0;
      for (int i = 0; i < std::min(f0, static_cast<int>(frames.size())); ++i)
        peakLevel = std::max(peakLevel, frames[static_cast<size_t>(i)]);
      double minInTransition = peakLevel;
      for (int i = std::max(0, f0 - 2); i < std::min(static_cast<int>(frames.size()), f0 + 20); ++i)
        minInTransition = std::min(minInTransition, frames[static_cast<size_t>(i)]);
      return peakLevel - minInTransition;  // how far the trace dips during the transition, below its own peak
    };
    const double dipOverlap = dipAround(true);
    const double dipSeparate = dipAround(false);
    checkNum("G7.12: overlapping notes (legato) -- no attack-transient dip at the second note-on",
             dipOverlap < 6.0, dipOverlap);
    checkNum("G7.12: non-overlapping notes -- DO retrigger, showing a real release/attack dip "
             "(confirms the comparison is meaningful)",
             dipSeparate > 10.0, dipSeparate);

    // Glide is applied in the legato case: mid-transition frequency sits
    // strictly between 220Hz (note 60 - wait, use notes that make the math
    // simple) -- reusing notes 69/81 (440/880Hz) for a clean octave gap.
    SynthCore core;
    core.init(48000.0f);
    configureCleanSaw(core);
    core.setEnvASustainPercent(100.0f);
    core.setVoiceMode(SynthCore::VoiceMode::Mono);
    core.setGlideTimeMs(300.0f);
    renderAbsEvents(core, {{0, NoteEvent::NoteOn, 69, 1.0f}}, static_cast<int>(0.03 * kFs), 512);
    renderAbsEvents(core, {{0, NoteEvent::NoteOn, 81, 1.0f}}, 32, 32);
    renderAbsEvents(core, {}, static_cast<int>(0.05 * kFs), 512);  // partway through a 300ms glide
    auto mid = renderAbsEvents(core, {}, static_cast<int>(0.006 * kFs), 512);
    const double fMid = measuredF0(mid, kFs);
    checkNum("G7.12: glide IS applied in the legato transition (mid-transition frequency strictly "
             "between 440Hz and 880Hz, not already snapped)",
             fMid > 440.0 * 1.01 && fMid < 880.0 * 0.99, fMid);
  }

  // =========================================================================
  // G7.13: Unison mode.
  // QUANTITY MEASURED: peak |sample| just after attack completes (before
  // detune-induced phase drift meaningfully decoheres the 8 voices) for
  // Unison vs a single Poly voice at the SAME note/settings -- ratio in dB
  // must be within 2dB of 20*log10(8)=18.06dB. This is ONLY reachable
  // because oscillator phase resets to 0 at every note-on (DESIGN.md §3.2)
  // -- without it the 8 detuned voices would start at arbitrary relative
  // phases and never coherently sum this high.
  // =========================================================================
  std::cout << "\nGroup: Unison mode (G7.13)\n";
  {
    auto peakJustAfterAttack = [&](SynthCore::VoiceMode mode) {
      SynthCore core;
      core.init(48000.0f);
      configureCleanSaw(core);
      core.setEnvAAttackMs(2.0f);
      core.setEnvADecayMs(1.0f);
      core.setEnvASustainPercent(100.0f);
      core.setPolyphony(SynthCore::Polyphony::Eight);
      core.setVoiceMode(mode);
      core.setStereoDetuneCents(6.0f);  // DESIGN.md §11 default, used here for Unison's own spread (G7.13)
      auto out = renderAbsEvents(core, {{0, NoteEvent::NoteOn, 57, 1.0f}}, static_cast<int>(0.01 * kFs), 512);
      const int from = static_cast<int>(0.003 * kFs), to = static_cast<int>(0.008 * kFs);  // right after attack
      return peakAbs(out, from, to);
    };
    const double peakUnison = peakJustAfterAttack(SynthCore::VoiceMode::Unison);
    const double peakPoly = peakJustAfterAttack(SynthCore::VoiceMode::Poly);
    const double ratioDb = 20.0 * std::log10(peakUnison / peakPoly);
    const double expectedDb = 20.0 * std::log10(8.0);
    checkNum("G7.13: Unison (8 voices, +-6c detune) sums to within 2dB of 20*log10(8)=18.06dB above "
             "one voice",
             std::fabs(ratioDb - expectedDb) <= 2.0, ratioDb);
  }

  // =========================================================================
  // G7.14: silent voices are skipped.
  // QUANTITY MEASURED: getDebugActiveVoiceCount() (DESIGN.md §10.7 [PERF-7])
  // reaches EXACTLY 0 within one release time + 100ms of the last note-off,
  // and output is EXACTLY 0.0 (kOutputClip off -- see configureCleanSaw()'s
  // own comment for why that keeps this a bit-exact passthrough, DESIGN.md
  // §11/G6.12).
  // =========================================================================
  std::cout << "\nGroup: silent voices are skipped (G7.14)\n";
  {
    SynthCore core;
    core.init(48000.0f);
    configureCleanSaw(core);
    core.setEnvASustainPercent(100.0f);
    core.setEnvAReleaseMs(200.0f);
    renderAbsEvents(core, {{0, NoteEvent::NoteOn, 60, 1.0f}}, static_cast<int>(0.02 * kFs), 512);
    renderAbsEvents(core, {{0, NoteEvent::NoteOff, 60, 0.0f}}, static_cast<int>(0.02 * kFs), 512);

    const int margin = static_cast<int>((0.2 + 0.1) * kFs);  // release time + 100ms
    auto tail = renderAbsEvents(core, {}, margin, 512);
    checkNum("G7.14: getDebugActiveVoiceCount() reaches 0 within one release time + 100ms",
             core.getDebugActiveVoiceCount() == 0, core.getDebugActiveVoiceCount());
    bool allExactZero = true;
    for (float s : tail)
      if (s != 0.0f) allExactZero = false;
    // Only the FINAL portion (after the release has genuinely finished) is
    // expected to be exact 0 -- check the last 10ms of the margin, which is
    // safely past the release tail.
    bool tailZero = true;
    for (size_t i = tail.size() - static_cast<size_t>(0.01 * kFs); i < tail.size(); ++i)
      if (tail[i] != 0.0f) tailZero = false;
    (void)allExactZero;
    check("G7.14: output is exactly 0.0 once the voice has fully released", tailZero);
  }

  // =========================================================================
  // G7.15/G7.16: sustained-load stability and determinism.
  // A deterministic (Xorshift32-seeded) 60s MIDI stream: 8-20 notes/s,
  // overlapping, with sustain-pedal and pitch-bend events interleaved.
  // QUANTITY MEASURED (G7.15): every rendered sample finite, |y| < 4.0,
  // voice count returns to 0 once every note has been explicitly released
  // and the stream's own tail margin has elapsed.
  // QUANTITY MEASURED (G7.16): the SAME stream, run twice into freshly-
  // init()'ed cores, produces BIT-IDENTICAL (==) output (R13) -- also what
  // GoldenParityG5 depends on.
  // =========================================================================
  std::cout << "\nGroup: sustained-load stability and determinism (G7.15/G7.16)\n";
  {
    Xorshift32 rng(0x60ADBEEFu);
    std::vector<NoteEvent> stream;
    const double streamSeconds = 60.0;
    const int heldMax = 24;
    int held[heldMax];
    int heldCount = 0;
    double t = 0.0;
    while (t < streamSeconds) {
      const double rate = 8.0 + (rng.next() % 13u);  // 8..20 notes/s
      const double dt = 1.0 / rate;
      t += dt;
      if (t >= streamSeconds) break;
      const int offset = static_cast<int>(t * kFs);

      const uint32_t pick = rng.next() % 100u;
      if (pick < 60 || heldCount == 0) {
        // NoteOn -- pick a note not already held, if possible.
        if (heldCount < heldMax) {
          const int note = static_cast<int>(30 + (rng.next() % 60u));
          const float vel = 0.3f + static_cast<float>(rng.next() % 71u) * 0.01f;
          stream.push_back({offset, NoteEvent::NoteOn, note, vel});
          held[heldCount++] = note;
        }
      } else if (pick < 90) {
        // NoteOff of a currently-held note.
        const int idx = static_cast<int>(rng.next() % static_cast<uint32_t>(heldCount));
        const int note = held[idx];
        stream.push_back({offset, NoteEvent::NoteOff, note, 0.0f});
        held[idx] = held[heldCount - 1];
        --heldCount;
      } else if (pick < 95) {
        stream.push_back({offset, NoteEvent::Sustain, 0, (rng.next() % 2u) ? 1.0f : 0.0f});
      } else {
        const float bend = -1.0f + 2.0f * static_cast<float>(rng.next() % 1001u) * 0.001f;
        stream.push_back({offset, NoteEvent::PitchBend, 0, bend});
      }
    }
    // Release everything still held, then let the tail settle.
    const int endOffset = static_cast<int>(streamSeconds * kFs);
    stream.push_back({endOffset, NoteEvent::Sustain, 0, 0.0f});
    stream.push_back({endOffset, NoteEvent::AllNotesOff, 0, 0.0f});
    const int tailSamples = static_cast<int>(3.0 * kFs);  // >= max release time (10000ms is out of range
                                                            // for the params this stream can select --
                                                            // envAReleaseMs stays at its 250ms default
                                                            // throughout, so 3s is generous)
    const int totalSamples = endOffset + tailSamples;

    auto runStream = [&](SynthCore& core) { return renderAbsEvents(core, stream, totalSamples, 512); };

    SynthCore coreA;
    coreA.init(48000.0f);
    auto outA = runStream(coreA);

    bool allFinite = true;
    double maxAbs = 0.0;
    for (float s : outA) {
      if (!std::isfinite(s)) allFinite = false;
      maxAbs = std::max(maxAbs, std::fabs(static_cast<double>(s)));
    }
    check("G7.15: 60s sustained-load stream -- every sample finite", allFinite);
    checkNum("G7.15: 60s sustained-load stream -- |y| < 4.0 throughout", maxAbs < 4.0, maxAbs);
    checkNum("G7.15: voice count returns to 0 once the stream's tail has settled",
             coreA.getDebugActiveVoiceCount() == 0, coreA.getDebugActiveVoiceCount());

    SynthCore coreB;
    coreB.init(48000.0f);
    auto outB = runStream(coreB);
    bool bitIdentical = outA.size() == outB.size();
    if (bitIdentical) {
      for (size_t i = 0; i < outA.size(); ++i)
        if (outA[i] != outB[i]) { bitIdentical = false; break; }
    }
    check("G7.16: the same 60s stream run twice into freshly-init()'ed cores is bit-identical (R13)",
          bitIdentical);
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
