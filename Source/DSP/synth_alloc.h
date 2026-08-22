#pragma once

// synth_alloc.h — NassauAnalogue voice allocation policy (Gate G7,
// DESIGN.md §10.3-§10.5). Header-only, dependency-free (R2): the only
// include is <cstdint>. This file holds ONLY the allocation DECISION logic
// (which physical voice a NoteOn should take, given the current state of
// the pool; how a Unison group's detune spreads; how kPolyphony maps to an
// active-voice count) as pure, stateless free functions operating on a
// small POD SlotInfo array — deliberately decoupled from SynthCore::
// Voice's heavy per-sample DSP state (oscillators, filters, envelopes) so
// this is testable completely standalone, with no SDK and no SynthCore at
// all (R14, R2): Tests/alloc_tests.cpp drives these functions directly
// against synthetic arrays, the same shape as synth_dsp.h/synth_osc.h being
// driven directly by dsp_tests.cpp/osc_tests.cpp.
//
// SynthCore (synth_core.h/.cpp) is the ONLY caller: it fills a small
// on-stack SlotInfo[] from its own Voice array's allocation-relevant fields
// (state/startedAt/releasedAt) once per NoteOn, asks this header which
// index to (re)use, and does the actual DSP-state work (phase reset,
// envelope noteOn, copying a stolen voice into a fade slot, ...) itself —
// no allocation anywhere in this file or its caller (R3): every array here
// is caller-owned and fixed-size.

#include <cstdint>

namespace nassau_alloc {

// Mirrors DESIGN.md §10.3's voice lifecycle exactly:
//   Idle     — no note assigned, free to grab at zero cost.
//   Playing  — a key is currently held (Poly), or this physical voice is
//              part of the currently-sounding note (Unison/Mono).
//   Held     — note released while the sustain pedal (CC64) is down; still
//              sounding, releases when the pedal lifts (DESIGN.md §10.3).
//   Released — note released normally, running its envelope's Release
//              phase, decaying toward silence.
enum class SlotState : int { Idle = 0, Playing = 1, Held = 2, Released = 3 };

// The allocation-relevant subset of a voice slot's state — everything
// chooseVoiceForSteal() needs and nothing else (no oscillator/filter/
// envelope state, which is SynthCore::Voice's job, not this header's).
struct SlotInfo {
  SlotState state = SlotState::Idle;
  uint32_t startedAt = 0;   ///< set at note-on; breaks ties among Playing/Held voices (oldest wins)
  uint32_t releasedAt = 0;  ///< set at note-off; breaks ties among Released voices (oldest wins)
};

// DESIGN.md §10.3 allocation order: "first Idle voice; else the oldest
// Released voice; else the oldest Playing voice." Idle ties break to the
// LOWEST index (deterministic, and — not incidentally — exactly the G3-
// provisional allocator's own "first idle" policy, so a fresh instance's
// early note-ons land on the same physical slots they always have,
// docs/GATES.md's own "structural risk" note for this gate). Held is
// grouped with Playing for the "oldest Playing" bucket: DESIGN.md's own
// wording pairs "Playing" against "Released" as the two non-idle buckets,
// and a sustain-held voice is still musically "sounding under a still-down
// pedal" — the opposite of a Released voice's decaying-toward-silence
// state — so stealing FROM it should be no more eager than stealing from an
// ordinary playing note. Returns an index in [0, count), or -1 iff
// count <= 0 (defensive; SynthCore never calls this with count <= 0 since
// kPolyphony's minimum is 4).
inline int chooseVoiceForSteal(const SlotInfo* slots, int count) {
  if (count <= 0) return -1;

  for (int i = 0; i < count; ++i) {
    if (slots[i].state == SlotState::Idle) return i;
  }

  int bestReleased = -1;
  for (int i = 0; i < count; ++i) {
    if (slots[i].state != SlotState::Released) continue;
    if (bestReleased < 0 || slots[i].releasedAt < slots[bestReleased].releasedAt) bestReleased = i;
  }
  if (bestReleased >= 0) return bestReleased;

  int bestPlaying = 0;
  for (int i = 1; i < count; ++i) {
    if (slots[i].startedAt < slots[bestPlaying].startedAt) bestPlaying = i;
  }
  return bestPlaying;
}

// DESIGN.md §10.5 Unison: "all kPolyphony voices play the held note,
// detunes spread symmetrically across +-StereoDetune cents." `slot` is in
// [0, groupSize); the return is evenly spaced from -detuneCents (slot 0) to
// +detuneCents (slot groupSize-1) inclusive — symmetric about 0 for ANY
// groupSize, since slot i and slot (groupSize-1-i) always sum to exactly 0
// (including the reflection through the middle pair for an odd count, and
// the degenerate groupSize<=1 case, which returns 0 — a single "unison"
// voice has nothing to spread against).
inline double unisonDetuneCentsFor(int slot, int groupSize, double detuneCents) {
  if (groupSize <= 1) return 0.0;
  const double t = (2.0 * static_cast<double>(slot)) / static_cast<double>(groupSize - 1) - 1.0;  // [-1, 1]
  return t * detuneCents;
}

// DESIGN.md §10.3/§11: "kPolyphony selects 4/6/8/12/16 active voices" —
// maps SynthCore::Polyphony's underlying int (0..4, DESIGN.md §11's
// enumeration order) to the actual voice count. [ref] DESIGN.md §11's
// kPolyphony row.
inline int polyphonyVoiceCount(int polyphonyEnumValue) {
  static const int kCounts[5] = {4, 6, 8, 12, 16};  // [ref] DESIGN.md §11 kPolyphony
  if (polyphonyEnumValue < 0 || polyphonyEnumValue > 4) return 8;  // [voicing] defensive fallback (default Eight)
  return kCounts[polyphonyEnumValue];
}

}  // namespace nassau_alloc
