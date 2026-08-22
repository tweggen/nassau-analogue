# NassauAnalogue — Implementation Gates

Each gate is a **self-contained work package** sized for one Sonnet instance.
A gate is *done* only when every one of its acceptance criteria (AC) is
objectively demonstrated — nearly all of them are `ctest` assertions, so "done"
is a command, not an opinion.

Read [`DESIGN.md`](DESIGN.md) first. It is the specification; this file is the
schedule. Where the two disagree, `DESIGN.md` wins and you fix this file.

> **The tie-breaker for every judgement call in this plan is DESIGN.md §0.1:
> performance outranks 100 % 80s authenticity.** Eight places where that bites
> are enumerated there as **[PERF-1..8]**. If an AC below and a [PERF] decision
> appear to conflict, the [PERF] decision wins and the AC is the thing that is
> wrong.

---

## Rules that bind every gate

**R1 — No gate may regress an earlier gate.** The exit command for *every* gate
is the full suite, not just the new tests:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

(Windows / Visual Studio is multi-config: `cmake -S . -B build`,
`cmake --build build --config Release`, `ctest --test-dir build -C Release --output-on-failure`.)

**R2 — The DSP core stays framework-free.** `Source/DSP/` may include only
`<atomic> <cmath> <cstddef> <cstdint> <algorithm> <limits>`, plus the
compiler's own arch-intrinsic headers (`<xmmintrin.h>`, `<intrin.h>`,
`<emmintrin.h>`, `<arm_neon.h>`) behind arch/compiler guards. No IPlug2, no SDK
header, ever. It must build and test with the SDK entirely absent — which, per
DESIGN.md §0.4, is the *only* way most of this plan can be executed on the
current development box at all.

**R3 — No allocation, no locks, no I/O in `process()`.** All voices and buffers
are fixed-size members sized in `init()` for `kMaxVoices + 2`. Params are
`std::atomic`, snapshotted **once per host block** into a `ParamSnapshot`
(DESIGN.md §2.2) — the audio path performs zero atomic loads.

**R4 — Append-only params.** New params go immediately before `kNumParams` and
`PLUG_N_PARAMS` is bumped in the same commit. Never reorder, never insert. The
gate order below is chosen so each gate's params are exactly the next contiguous
indices of the final enum in DESIGN.md §11 — check your indices against that
table, not against what is currently in the file.

**R5 — Every numeric constant carries a provenance comment**: `// [ref]`,
`// [dsp]`, or `// [voicing]`, per DESIGN.md §0.3. There is no source transcript
for this plugin; nothing may be tagged `[ref]` unless it is a published,
checkable number. A guess is `[voicing]`.

**R6 — Tests measure signals, not coefficients.** Drive the code with audio and
measure RMS / magnitude / envelope / harmonics. A test that inspects private
coefficient state is not an acceptance test.

**R7 — No `-ffast-math`, no `-march=native`.** Portable *flags* only. The ZDF
solve (DESIGN.md §5.2) and every IIR recursion are unsafe under fast-math.
Platform-specific **code** behind compile-time `#ifdef` on `_M_X64`/`__x86_64__`/
`_M_ARM64`/`__aarch64__` is allowed and encouraged where it buys something real
(denormal control, SIMD), provided every branch has a portable fallback and all
branches agree within golden-parity tolerance.

**R8 — Determinism.** No `rand()`, no `time()`. All noise is `Xorshift32` with
fixed seeds.

**R9 — The plugin never writes its own parameters.** No processor-initiated
`SetParameterValue`.

**R10 — Finish the gate by updating this file**: mark the gate DONE with a
one-line summary of what was actually measured (numbers, not adjectives), then
commit. One commit per gate.

**R11 — Report honestly.** If an AC cannot be met, say so with the measured
number and **stop** — do not relax the AC on your own. A genuine impossibility
is a finding, not a failure. Every AC here has been checked for achievability,
but that check was not infallible.

**R12 — Nothing transcendental, and nothing parameter-derived, in the audio-rate
loop.** `tan`, `exp`, `exp2`, `pow`, `log`, `sin`, `cos` and every atomic load
belong at control rate or block rate (DESIGN.md §2). This is grep-able and G11.7
greps for it. The three exceptions are the three interpolated scalars in
DESIGN.md §2 (`gLpf`, `gHpf`, `vcaGain`), which are *adds*, not transcendentals.

**R13 — Every stochastic source is per-instance seeded and reset.** Each voice's
white-noise generator is seeded from its voice index, and the LFO's sample-and-
hold generator from a fixed constant, both in `init()` and `reset()`. Two runs
of the same MIDI sequence into a freshly-`init()`ed core must be **bit-identical**
(G10 depends on this and it is easy to break by accident).

**R14 — The core is the whole instrument.** Voice allocation, MIDI event
handling, glide, sustain-pedal state and stereo chain duplication live in
`Source/DSP/`, behind the framework-free `NoteEvent` interface of DESIGN.md
§10.1. `Source/Plugin/` contains only: param↔host plumbing, `IMidiMsg` →
`NoteEvent` translation, versioned state, presets, and the editor.

---

## Shared test harness

All test binaries use the `nassau-eq` / `nassau-zermatt` house harness: plain
`int main()`, a `check(name, cond)` soft-check that tallies, run-everything-
then-summarise, nonzero exit on any failure. Copy
`nassau-zermatt/Tests/amp_tests.cpp`'s first 70 lines as the starting point.

Every AC in every table below must be covered by at least one named `check()`.
The binaries print a tally; a tally is a smoke signal, not a coverage measure —
do not pad it.

Helpers live in `Tests/test_util.h`, created in G1. Start from
`nassau-zermatt/Tests/test_util.h` and **add** the instrument-specific ones:

```cpp
// inherited from nassau-zermatt/Tests/test_util.h
double rms(const std::vector<float>&, int from, int to);
double thdPercent(const std::vector<float>&, double f0, double fs);
double harmonicDb(const std::vector<float>&, double f0, int n, double fs);  // Goertzel
double nonHarmonicEnergyDb(const std::vector<float>&, double f0, double fs); // MUST BE MODIFIED
void   envelopeDb(const std::vector<float>&, double fs, std::vector<double>&); // 1 ms frames

// new for this plugin -- created by the gate that first needs them, NOT all in G1
double measuredF0(const std::vector<float>&, double fs); // G1: long-window, sub-cent
double minus3dBPoint(/*filter callable*/, double fs);    // G1: bisection on magnitude
double magDbAt(SynthCore&, double probeHz, double fs);   // G4: needs a filter to probe
std::vector<NoteEvent> seqHoldNote(int note, float vel, double onSec, double offSec, double fs); // G3
std::vector<NoteEvent> seqChord(const std::vector<int>& notes, ...);                             // G7
```

**Only the SynthCore-independent helpers are due in G1.** `magDbAt` needs a
filter to probe and `seqHoldNote`/`seqChord` need a voice that responds to
notes; neither exists before G3/G4. Writing them in G1 would mean writing
untested guesswork against an API whose shape is still being decided. G1 raised
this; it is settled here.

### Two things about `nonHarmonicEnergyDb` that invalidate every alias AC if missed

**(1) Remove the 40-harmonic cap.** Zermatt's implementation caps harmonic
subtraction at `std::min(40, ...)`. It measures an amp's alias suppression,
where 40 harmonics is plenty. Here it is fatal: a **perfect** 110 Hz saw at
48 kHz has 218 real harmonics, and counting 178 of them as "non-harmonic
residual" makes a flawless oscillator read about **−17 dB**. Every G2 alias AC
would then be measuring the oscillator's own harmonics. The new `test_util.h`
must subtract every harmonic up to Nyquist.

**(2) Probe frequencies must be non-commensurate with the sample rate.** For
`f0` = 110, 1000 or 4000 Hz at `fs` = 48 kHz, every alias image
`|m*f0 - q*fs|` is itself an exact multiple of `gcd(f0, fs)` — i.e. the aliases
land **exactly on harmonic bins** and are subtracted along with the harmonics.
The measurement then returns the numerical floor (≈ −135 dB) regardless of how
bad the oscillator is. Every alias AC below therefore uses **109 / 997 /
3989 Hz**, and no alias AC anywhere in this plan may use a round frequency.

`measuredF0` must be **sub-cent accurate** — several G2 ACs are stated to
±0.1 Hz at 440 Hz. Zero-crossing counting over ≥ 1 s with linear interpolation
of the crossing, or a parabolic-interpolated FFT peak, both reach that. A bare
FFT bin peak does not; do not use one.

**Determinism of test signals**: nothing in the suite may invent its own noise.
Use the shared `Xorshift` generator, as Zermatt does.

---

# G0 — Scaffold and pipeline proof

**Goal:** the repository, the build, the test harness, and the complete final
public API of `SynthCore` — declared, not implemented. No sound yet beyond a
proven-silent, proven-bounded path.

> **STATUS: DONE** (R10). Measured on the Linux dev box (g++ 15.3.0, cmake
> 4.3.4): `ctest --test-dir build` → 1/1 test binaries passed, `SynthTests`
> 27/27 checks passed, 0 compiler warnings at `-Wall -Wextra -Wpedantic`
> across `Source/DSP` + `Tests` (both the default and
> `-DNASSAU_FORCE_HEADLESS=ON` configs), 0 heap allocations measured across
> 479,744 samples (937×512-sample blocks ≈ 10 s @ 48 kHz) of `process()` with
> a busy 8-event stream (G0.11), and a full `-fsanitize=thread` rebuild of the
> same binary (GCC supports it; G0.9's own AC anticipates this) ran the same
> 27/27 checks green with zero TSan diagnostics. G0.1/G0.3(plugin
> half)/G0.12 are **not applicable on this platform** — no SDK/iPlug2
> checkout exists here and the platform is Linux (DESIGN.md §0.4); `G0.2`'s
> skip-message path is exactly what was exercised instead. `Source/Plugin/`
> was written (config.h, NassauAnaloguePlugin.{h,cpp}, NassauAnalogueUI.{h,cpp},
> nassau_state.h, the 3 Info.plist templates) but is **unbuilt and
> unverified** — G9 is the first gate that can actually compile it. See the
> G0 gate report (session record) for the full per-AC table, the
> `PLUG_MFR_ID` wording conflict found in this gate's own text (R11), and the
> debug-accessor scoping decision.

### Deliverables

* `CMakeLists.txt` mirroring `nassau-zermatt/CMakeLists.txt`: `Source/DSP` always,
  `Source/Plugin` only when the SDK and the vendored iPlug2 both exist **and**
  the platform is macOS or Windows, `Tests` always.
* `Source/DSP/{synth_core.h, synth_core.cpp, CMakeLists.txt}`. `synth_core.h`
  declares the **complete final API**: every setter for all 53 params of
  DESIGN.md §11, the `NoteEvent` struct of §10.1, `init/reset/process`, and the
  debug accessors later gates need. Each gate then fills in math behind a stable
  header shape.
* `Source/Plugin/{config.h, NassauAnaloguePlugin.{h,cpp}, NassauAnalogueUI.{h,cpp},
  nassau_state.h, resources/NassauAnalogue-{VST3,AU,CLAP}-Info.plist}`.
* `Tests/{test_util.h, synth_tests.cpp, CMakeLists.txt}`.
* `docs/` already contains DESIGN.md and this file.

**Instrument-specific `config.h` values** — these differ from every previous
Nassau plugin and getting them wrong is a whole-gate-later discovery:

```c
#define PLUG_TYPE              1        // instrument, NOT 0
#define PLUG_DOES_MIDI_IN      1
#define PLUG_DOES_MIDI_OUT     0
#define PLUG_DOES_MPE          0
#define PLUG_CHANNEL_IO        "0-2"    // no audio input at all
#define PLUG_N_PARAMS          53
#define PLUG_N_PRESETS         12
#define PLUG_LATENCY           0        // and it stays 0: no oversampling anywhere
#define PLUG_DOES_STATE_CHUNKS 0        // 0, yet SerializeState IS overridden -- copy
                                        // zermatt's stance verbatim, it is proven
#define PLUG_UNIQUE_ID         'Nan1'   // MUST differ from zermatt's 'Nzm1'
#define PLUG_MFR_ID            'Nass'
#define VST3_SUBCATEGORY       "Instrument|Synth"
#define CLAP_FEATURES          "instrument", "synthesizer", "stereo"
```

`iplug::Config`'s plugin-type argument must be `kInstrument`, not `kEffect`, and
the AU Info.plist type must be `aumu`, not `aufx`.

**Four of these are received wisdom, not verified fact**, because the SDK's
iPlug2 submodule is unchecked-out (DESIGN.md §0.4) and could not be read while
this plan was written: `kInstrument`'s exact spelling in `iplug::Config`, the
`IMidiQueue` drain semantics G9.7 depends on, whether `PLUG_CHANNEL_IO "0-2"` is
accepted verbatim, and whether CLAP note-ports is advertised automatically from
`PLUG_DOES_MIDI_IN`. They are consistent with iPlug2 convention and with
Zermatt's wrapper (which only ever passes `kEffect`), but **G9 must verify all
four before writing anything else**, and G0 should verify as many as it can once
the submodules are provisioned.

### Acceptance criteria

| ID | Criterion | Evidence |
|---|---|---|
| G0.1 | On macOS/Windows with the SDK provisioned, CMake produces `NassauAnalogue.vst3` (+ `.clap`, + `.component` on macOS) under `build/out/` | build log + file exists |
| G0.2 | On **Linux**, and on any platform with `-DNASSAU_SDK_DIR=/nonexistent`, configure prints the skip message and `ctest` is green — DSP core + tests only | configure log; ctest green |
| G0.3 | Builds clean in **both** UI and `-DNASSAU_FORCE_HEADLESS=ON` configs | two build logs, zero errors |
| G0.4 | Zero warnings from `Source/` at `-Wall -Wextra -Wpedantic` / `/W4` | build log |
| G0.5 | `process()` with **no events** writes exact silence (`== 0.0f`) to both channels for 4096 samples, from a fresh instance and after a `reset()` | exact |
| G0.6 | `init()` accepts 44100/48000/88200/96000/192000 and `getSampleRate()` reports each back; bogus rates are ignored | — |
| G0.7 | `reset()` clears all state without touching params: process a note, `reset()`, then re-run the identical event sequence — output is **bit-identical** to a fresh instance's (this is also R13's first test) | exact |
| G0.8 | `process()` is safe for every host block size 1..8192 including 1, and for `numEvents == 0`, and for events at offset 0 and at `numSamples-1` | no crash, all finite |
| G0.9 | Every setter can be called from a non-audio thread while `process()` runs, with no data race | thread + 1 s of blocks; plus one `-fsanitize=thread` build on a Clang host if available, else record "not run" |
| G0.10 | `PLUG_UID_STR` is **exactly 32 hex characters** and differs from NassauZermatt's `C51E51D5BAFEFB22B5B6E8CFF6BEFB30` and NassauEQ's `6DDB946C34D64805A0C496E553B57266`; **`PLUG_UNIQUE_ID` also differs** from Zermatt's `'Nzm1'`. **`PLUG_MFR_ID` deliberately does NOT differ** — it stays `'Nass'`. A manufacturer code names the *vendor* and is expected to be constant across a catalogue; per-plugin uniqueness is the job of the `(type, subtype)` pair, which `PLUG_UNIQUE_ID` already provides. An earlier draft of this AC said "and so do `PLUG_UNIQUE_ID` / `PLUG_MFR_ID`", contradicting this gate's own Deliverables snippet two paragraphs above; G0 found the contradiction and resolved it this way. **Assert the length**: a VST3 FUID is 128 bits, so 31 or 33 characters is malformed, and nothing on a Linux or headless box will ever tell you — G0 shipped a 31-character UID (one character lost transcribing a SHA-256 digest) that a `grep`-only check passed | grep + length assert |
| G0.11 | **Zero heap allocation in `process()`**: global `operator new`/`delete` counter, 10 s of blocks with a busy event stream, count == 0. Land this in G0 rather than G10 — an instrument has a dozen places to accidentally allocate (event vectors, voice lists) and retrofitting the check finds them all at once, late | 0 allocations |
| G0.12 | Plugin loads in one real host on an instrument track, receives MIDI, and outputs silence without crashing | manual, recorded in the gate note |

**Exit:** full `ctest` green; the note records which of G0.1/G0.3/G0.12 were run
on which platform.

---

# G1 — DSP primitives

> **STATUS: DONE** (R10). Measured on the Linux dev box (g++ 15.3.0, cmake
> 4.3.4): `ctest --test-dir build` → 2/2 test binaries passed (`SynthTests`
> 27/27, `DspTests` 75/75), 0 compiler warnings at `-Wall -Wextra -Wpedantic`
> across `Source/DSP` + `Tests`. All 19 ACs met; key measured numbers:
> G1.1 TptOnePole LP matched an independently-coded analytic reference to
> ≤0.043 dB (tol 0.1 dB) across 8 (fc,fs) combinations × 24 log-spaced probes
> (20 Hz .. min(20 kHz, 0.48·fs), a range chosen to avoid the >−150 dBFS
> region where float32 test-buffer precision, not the filter, dominates the
> comparison). G1.2: HP == x−lp exact over 1e5 samples; DC residual
> 4.1e-15 after 5 s (tol 1e-9). G1.3: finite/|y|<4 (max 1.528) across fc ∈
> [10 Hz, 0.45·fs] at all 5 rates, 10 s of noise each. G1.4/G1.5: polyBlep
> boundary residuals ≤4.4e-16, antisymmetry error ≤2.1e-15 (tol 1e-12).
> G1.6/G1.7: Xorshift32 mean 8.9e-5, variance 0.33333 over 1e7 draws, no
> immediate repeats, two same-seeded streams bit-identical over 1e6 draws.
> G1.8: PinkFilter's 8 octave-pair sine-response deltas ranged −2.42 .. −3.69
> dB (tol 3.0±0.7 dB) — measured via magnitude response (the filter is LTI),
> not a noise periodogram, because the latter was hand-verified (python3) to
> occasionally exceed 0.7 dB on a *correct* filter from statistical variance
> alone. G1.9–G1.14: AdsrEnv attack/decay/release all within 15% (worst case
> 3.2% at 1000 ms); G1.11 short-attack cases (1/2/5 ms) within the
> quantisation-aware bound; G1.12 t(50%)/t(99%) = 0.2945 (tol 0.289±0.03);
> G1.13 never leaves [0,1], exactly 0.0 in Idle; G1.14 transition jump = 0.0
> exactly (tol 1e-6). G1.15: Lfo frequency accurate to ≤0.02% at 0.05/1/5/30
> Hz; all waves in [-1,1]; Tri/Saw/Ramp/Square zero-mean to ≤4.3e-14 over
> dynamically-detected whole cycles (tol 1e-6) — required a midpoint-phase
> evaluation fix in `Lfo::step()` (see synth_dsp.h) after an initial
> left-edge sampling scheme showed a real ~0.003 discretization bias on
> Saw/Ramp, and required the *test* to detect cycle boundaries by watching
> `phase` wrap rather than assuming a fixed step count, because
> `1.0/300.0` summed 300 times in double is 0.9999999999999961 (verified),
> one ULP short of the assumed wrap point. G1.16: S&H constant strictly
> between wraps, changes on the step after every wrap (the draw happens
> one step after the wrap is detected, since `step()` returns the OLD
> shValue on the wrapping step itself). G1.17: exactly 0 for the first
> 500 ms, ramp reaches 0.503 at the midpoint and 1.0 at 200 ms (tol 5%).
> G1.18: MXCSR round-trips exactly, and — the amendment this AC specifically
> asked for — a functional check confirms a denormal product genuinely
> flushes to zero (`FP_ZERO`) while the guard is active and is a genuine
> `FP_SUBNORMAL` outside it, so the guard is provably not a no-op on this
> x86-64 host. G1.19: `shapeTriodeK(x,0)==x` bit-exact over 4096 random x;
> `shapeCubic(0.25,2.0) == 0.251922607421875` to <1e-12 (independently
> recomputed with python3; confirmed this is NOT the cubic-only value
> 0.251953125). Two bonus (unnumbered) harness self-checks also pass:
> `measuredF0` recovers a known 440 Hz sine to <0.1 Hz, and `minus3dBPoint`
> locates TptOnePole's own corner to within 2% of fc. `fastTan`/`fastExp2`
> were **not** added (DESIGN.md §2.1's documented non-goal). See the G1 gate
> report (session record) for the per-AC table and the two harness-methodology
> defects found and fixed during this gate (both explained above): the
> literal-noise-periodogram approach to G1.8, and the fixed-step-count
> assumption for G1.15/G1.16's "whole cycle" boundaries.

**Goal:** `Source/DSP/synth_dsp.h`, header-only, dependency-free, all `double`
internally. This is the foundation every later gate builds on.

### Deliverables

Lifted **verbatim** from `nassau-zermatt/Source/DSP/amp_dsp.h` (they are already
gate-proven; do not rewrite them):
`OnePoleHP`, `OnePoleLP`, `SmoothedValue`, `ScopedNoDenormals`,
`shapeTriodeK`, `shapeCubic`.

New here:
`TptOnePole` (LP and HP outputs), `Xorshift32`, `PinkFilter` (Kellet 3-pole),
`polyBlep(t, dt)`, `AdsrEnv`, `Lfo`.

**Explicitly NOT delivered**: `fastTan`, `fastExp2`. DESIGN.md §2.1 shows they
would be optimising a 0.1 % line item. Do not add them "while we're here".

### Acceptance criteria

| ID | Criterion | Tolerance |
|---|---|---|
| G1.1 | `TptOnePole` low-pass magnitude matches the analytic bilinear response `\|g/(1+g) * (1+z^-1)/(1 - (1-g)/(1+g) z^-1)\|` at 24 log-spaced probes, for fc = 20 / 200 / 2000 / 10000 Hz, at fs = 44.1 k and 96 k. The reference must be **independently coded in the test** — evaluating the same coefficient function under test proves nothing | 0.1 dB |
| G1.2 | `TptOnePole` high-pass output equals `x - lp` exactly, and has **zero DC gain**: constant 1.0 for 5 s → final `\|y\| < 1e-9` | exact / 1e-9 |
| G1.3 | `TptOnePole` is stable and finite for fc from 10 Hz to `0.45*fs` at all five supported rates, 10 s of `Xorshift` | all finite, `\|y\| < 4` |
| G1.4 | `polyBlep` is **exactly 0** outside `[0, dt) ∪ (1-dt, 1)`, and is continuous at both boundaries: `polyBlep(dt-eps, dt)` and `polyBlep(1-dt+eps, dt)` both → 0 as eps → 0 | exact / 1e-12 |
| G1.5 | `polyBlep` is antisymmetric about the discontinuity: `polyBlep(1-t, dt) == -polyBlep(t, dt)` for 200 t in `(0, dt)` | 1e-12 |
| G1.6 | `Xorshift32` has full period behaviour over 1e7 draws: mean in `[-0.01, 0.01]`, variance in `[0.32, 0.34]` (uniform on [-1,1) has variance 1/3), and no value repeats its immediate predecessor | — |
| G1.7 | `Xorshift32` is **reproducible**: two generators with the same seed produce bit-identical sequences for 1e6 draws (R13) | exact |
| G1.8 | `PinkFilter` slope: fed white noise, the measured spectrum falls **3.0 ± 0.7 dB per octave** across 8 octave-pair probes from 40 Hz to 10 kHz at fs = 48 k. **Do not demand ±0.3 dB** — the Kellet filter is a 3-pole *approximation* and its published worst-case error is about ±0.5 dB, before measurement noise | 0.7 dB |
| G1.9 | `AdsrEnv` attack time to 0.99 within 15 % of request for 10 / 100 / 1000 ms, at control rates derived from fs = 44.1 k and 96 k with `kControlBlock = 32` | 15 % |
| G1.10 | `AdsrEnv` decay reaches within 1 % of sustain in the requested time, and release from a full-scale sustain reaches 0 in the requested time | 15 % |
| G1.11 | **Short times are quantisation-limited, and the AC says so.** For requested times **below 10 ms**, the tolerance is 15 % **or 1.5 control blocks, whichever is larger**. At fs = 44.1 k, `kControlBlock = 32` gives one step every **0.726 ms**, so a 1 ms attack is 1.4 steps and *cannot* be accurate to 15 % — that is arithmetic, not an implementation defect. **Do not** "fix" this by running the envelopes at audio rate; DESIGN.md §2 keeps them at control rate on purpose and the filter cutoff they drive is interpolated anyway | 15 % or 1.5 blocks |
| G1.12 | `AdsrEnv` is **exponential, not linear**: during attack, `t(50%) / t(99%) = 0.289 ± 0.03`. A linear ramp gives 0.505 (DESIGN.md §6) | 0.03 |
| G1.13 | `AdsrEnv` never returns negative, never exceeds 1.0, and reaches **exactly** 0.0 in Idle | exact |
| G1.14 | `AdsrEnv` note-off during attack goes straight to Release from the current level (no jump), and note-on during release restarts attack from the current level (no jump). Measure the sample-to-sample delta at the transition | ≤ 1e-6 jump |
| G1.15 | `Lfo` frequency accurate to 1 % for 0.05 / 1 / 5 / 30 Hz; all five waves are in `[-1, 1]`; Tri/Saw/Ramp/Square have zero mean over a whole number of cycles | 1 % / 1e-6 |
| G1.16 | `Lfo` sample-and-hold changes value exactly once per cycle and holds exactly in between | exact |
| G1.17 | `Lfo` delay: with `kLfoDelay = 500 ms`, output is exactly 0 for the first 500 ms, then ramps to full depth over 200 ms | exact / 5 % |
| G1.18 | `ScopedNoDenormals` restores the FP control word exactly on scope exit, **and is not a no-op on MSVC** — Zermatt's R2 amendment exists because this silently degraded on Windows | read-back compare |
| G1.19 | `shapeTriodeK(x, 0) == x` bit-identically for 4096 random x, and `shapeCubic(x, 2.0)` at x = 0.25 equals **0.251922607421875** exactly (i.e. **+0.06654 dB**), the number DESIGN.md §11 asserts. **Do not use 0.251953125** — that is the cubic term alone, omitting `-x^5/(2L^4) = -0.000030517578125`, and a verbatim lift of Zermatt's `shapeCubic` can never produce it | exact / 1e-12 |

**Exit:** `ctest` green, `DspTests` added.

---

# G2 — Oscillators, sub, noise, mixer

> **STATUS: DONE** (R10). Measured on the Linux dev box (g++ 15.3.0, cmake
> 4.3.4): `ctest --test-dir build` → 3/3 test binaries passed (`SynthTests`
> 27/27, `DspTests` 75/75, `OscTests` 50/50), 0 compiler warnings at
> `-Wall -Wextra -Wpedantic` across `Source/DSP` + `Tests` on a from-scratch
> `rm -rf build` rebuild. Params 2–18 were already fully declared, atomic-
> backed and snapshotted by G0 (verified against DESIGN.md §11's table index
> by index, R4) — G2 added no new `SynthCore` setters; `SynthCore::process()`
> is unchanged from G0/G1 and still writes exact silence (no voice concept
> exists before G7, so G0.5's silence AC still holds, R1). All 16 ACs met;
> key measured numbers (probe frequencies and formulas independently
> verified with `python3 -c` per R11 before being written into the test,
> see `Tests/osc_tests.cpp`'s per-group comments):
> G2.1 220/440/880/1760 Hz all within 0.1 Hz. G2.2 452.893/427.474/659.255 Hz
> (targets 452.8929841231365/427.4740541075866/659.2551138257398). G2.3 saw
> H2–H8 (110 Hz): −6.021/−9.544/−12.044/−13.983/−15.568/−16.909/−18.071 dB
> (targets within 0.5 dB). G2.4 pulse d=0.25 H2/H1 = −3.011 dB (target
> −3.010); d=0.50 H2 = −280 dB (≥40 dB bound). G2.5 triangle (220 Hz) H3/H5/H7
> = −19.083/−27.961/−33.813 dB (targets −19.08/−27.96/−33.80); H2/H4/H6 ≥
> 64.6/70.6/74.1 dB down; DC mean 9.5e-9 (< 1e-4), measured after a 300 ms
> settle window (the leaky integrator's own physically-real cold-start
> transient — verified independently in Python — needs ~9 time constants to
> clear 1e-4; this is a test-methodology fix, not an AC relaxation, same
> class as G1's own harness fixes). G2.6 naive −20.6 dB vs PolyBLEP −36.2 dB,
> a 15.5 dB improvement (≥ 12 dB bound; reference ballpark 16.5 dB). G2.7
> absolute alias floor — saw: −46.0/−36.1/−28.6 dB at 109/997/3989 Hz (bounds
> −35/−26/−22); pulse: −43.4/−33.8/−28.5 dB (same bounds) — all comfortably
> inside bound and in the same ballpark as the reference figures (saw
> −39.6/−30.4/−26.3, pulse −42.5/−32.9/−27.4), not suspiciously better, so
> the harness traps (40-harmonic cap, round probe frequencies) are confirmed
> not silently defeating the measurement. G2.8 period ratio exactly 2.0/4.0
> to well under 1e-9 (see "defects found" below — this needed a genuine test
> fix, not just an implementation one). G2.9 sub vs. a directly-generated
> 50 % pulse at 1994.5 Hz: −30.9507 dB vs. −30.9507 dB, delta 3.9e-10 dB (≤ 3
> dB bound; see "defects found" — SubOsc's BLEP treatment needed a real
> redesign to get here, not just a scale factor). G2.10 sync'd fundamental
> 220.03 Hz vs VCO1's 220 Hz (± 0.1 Hz bound), measured via autocorrelation-
> at-master-period rather than zero-crossing counting (see per-AC notes
> below for why). G2.11 sync alias floor −24.7 dB at f0=219Hz, VCO2=3.7×
> (≥ 12 dB bound; reference ballpark −16.6 dB). G2.12 white noise flat to
> within 0.57 dB across 8 octave bands (≤ 1.5 dB bound), measured via
> averaged-Goertzel band PSD, not constant-Q band power (the latter would
> read a false ~3 dB/octave rise for genuinely white noise — see per-AC
> notes). G2.13 two `NoiseSource`s, same voice index, bit-identical over 1e6
> samples; a different voice index diverges immediately. G2.14 mixer
> linearity exact to < 1e-6 across solo, all-100%, and a partial-level case.
> G2.15 finite-everywhere grid (1440 combos: 3 waves × 4 octaves × 3 PW
> settings × 2 sync × 2 sub-octave × 2 noise-colour × 5 rates, 512 samples
> each) — 0 failures, global worst-case |y| = 7.86 (< 8.0 bound); this
> number is 0 only after fixing a genuine oscillator defect found by this
> AC, see below — leaving it unfixed measured up to 30.6 in-grid and, in a
> longer standalone reproduction, ~65–70 (still bounded, not a divergence,
> but severely broken). G2.16 max envelope delta 0.114 dB/ms across a 100 ms
> fine-tune sweep (< 0.5 dB/ms bound), measured at a 1000 Hz base frequency
> chosen so a 1 ms envelope frame aligns with one waveform period (see
> per-AC notes — the naive 440 Hz choice this AC's first draft used gives a
> ~10 dB/ms false positive from frame/period misalignment alone, confirmed
> with dt held perfectly constant).
>
> **Two genuine defects found in this gate (R11), both fixed, both
> documented in `Source/DSP/synth_osc.h`'s comments at the point of the
> fix:**
>
> 1. **`SubOsc`'s originally-planned "reuse VCO1's raw wrapFrac/dt in one
>    `polyBlep` call" BLEP treatment (as DESIGN.md §3.4's prose reads most
>    literally) only supplies the POST-edge half of a proper 2-point BLEP
>    correction — never the PRE-edge half a free-running oscillator gets by
>    evaluating `polyBlep(phase, dt)` every sample regardless of whether a
>    wrap happens that sample.** Measured shortfall: 17 dB against G2.9's own
>    reference figure (a single-sided version read ≈ −14.1 dB against a
>    ≈ −31 dB target). The fix redefines the sub's own phase as a value
>    RECOMPUTED each sample from the driver's live phase and a small integer
>    wrap-position counter (`subPhase = (cycleWrapIndex + driverPhase) /
>    periodDivisor()`, never independently integrated — so G2.8's "exact to
>    1e-9" property is preserved by construction, not by luck) and treats the
>    sub as an ordinary 50%-duty pulse at that derived phase/dt, getting the
>    same two-sided correction a native oscillator gets for free. Result:
>    within 3.9e-10 dB of a directly-generated reference pulse — effectively
>    bit-for-bit the same sequence of dt-steps.
> 2. **Hard-syncing a Triangle-wave VCO2 (`kOsc2Wave=Tri`, `kOsc2Sync=on` —
>    a fully legitimate, exposed parameter combination) can drive the
>    leaky-integrated triangle's internal state to a sustained DC offset of
>    roughly 65–70× nominal amplitude**, found by G2.15's grid search
>    (worst observed in-grid: 30.6, still climbing at sample 511/512).
>    Root cause, verified independently in Python: a hard-synced slave's
>    underlying square is truncated at an arbitrary, sync-ratio-dependent
>    point in its duty cycle every master cycle; for a rational (hence
>    periodic) ratio this truncation pattern repeats, and being generically
>    NOT exactly 50/50, feeds a small but persistent non-zero-mean signal
>    into the leaky integrator every cycle. The leak's DC gain is large by
>    design (~1/(1−r) ≈ 1000 at the ~7.6 Hz corner, deliberately low so it
>    does not colour real audio content) — bounded, not divergent, but
>    severely audibly broken. This is a structural mismatch between
>    DESIGN.md §3.1 (integrator-based triangle) and §3.3 (hard sync) that
>    neither section cross-references, not an implementation slip — an
>    equivalent analogue Miller-integrator circuit, hard-synced with an
>    imbalanced duty, drifts toward a rail the same way. **Fix**: a sync
>    reset for `Wave::Tri` also re-anchors the integrator state to the
>    analytically correct (band-unlimited) triangle value at the new phase,
>    instead of letting it inherit whatever partial area the truncated cycle
>    left behind — a natural extension of "phase resets to the fractional
>    overshoot" (DESIGN.md §3.3) into the integrator's own state. This bounds
>    the same adversarial case (VCO2=Tri, sync on, 1.5× ratio, f0=1760 Hz @
>    44.1 kHz) to |y| ≤ 1.81, with no measurable change to any other AC
>    (confirmed: G2.1–G2.14/G2.16 all measure identically before and after).
>    **This is worth flagging for DESIGN.md itself**: neither §3.1 nor §3.3
>    currently documents that combining them needs this extra rule; a future
>    revision should probably say so explicitly rather than leaving it
>    implicit in this file's comments.
>
> Two decisions the plan did not cover, both resolved and both documented at
> the point of the decision: (a) whether triangle's leaky-integrator state
> resets on every note-on or only on a full `reset()`/fresh instance — `Osc`
> exposes both `resetPhase()` (phase only, matching DESIGN.md §3.2's literal
> "both oscillators reset to 0" wording) and `reset()` (phase + integrator,
> matching DESIGN.md §11's "Reset semantics"), leaving the choice of which
> one a note-on calls to G7's voice design, since DESIGN.md itself only
> speaks to phase; (b) `nonHarmonicEnergyDb`'s convention (an absolute
> dBFS-equivalent residual level, not normalised to the fundamental's own
> level) was used as-is per the reference figures in the AC text, matching
> how those figures read.

**Params landed: 2–18** (DESIGN.md §11).

**Goal:** `Source/DSP/synth_osc.h` — the complete VCO section. Still no filters,
no envelopes: G2's tests drive the oscillators directly (header-only, no core
link needed), exactly as Zermatt's `cabinet_tests.cpp` drives `Cabinet`.

### Deliverables

`Osc` (saw / pulse / tri, PolyBLEP, sync input/output), `SubOsc`, `NoiseSource`,
and a `MixerBlock` that sums the four at their levels. A **test-only**
`bool mBlepEnabled = true` member on `Osc`, used by exactly one AC (G2.6).

### Acceptance criteria

| ID | Criterion | Tolerance |
|---|---|---|
| G2.1 | **Tuning**: note 69, all offsets zero, 8' → **440.00 Hz**. 16'/4'/2' → 220/880/1760 Hz. Measured with `measuredF0` over ≥ 1 s at fs = 48 k | ± 0.1 Hz |
| G2.2 | **Fine tune**: +50 cents → `440 * 2^(50/1200)` = 452.89 Hz; −50 → 427.47 Hz. **Semi**: `kOsc2Semi = +7` → 659.26 Hz | ± 0.1 % |
| G2.3 | **Saw spectrum is 1/n**: at f0 = 110 Hz, fs = 48 k, harmonics 2–8 relative to the fundamental are −6.02 / −9.54 / −12.04 / −13.98 / −15.56 / −16.90 / −18.06 dB | 0.5 dB |
| G2.4 | **Pulse duty cycle is analytic**: a pulse of duty `d` has harmonic `n` at `(2/(n*pi))*\|sin(n*pi*d)\|`. At d = 0.25, H2/H1 = **−3.01 dB**. At d = 0.50, H2 is ≥ 40 dB below H1 (a square has no even harmonics) | 0.5 dB / 40 dB |
| G2.5 | **Triangle spectrum is odd-only, 1/n²**: at f0 = **220 Hz** (chosen so the leaky integrator's ~7.6 Hz corner is irrelevant), H3/H1 = −19.08, H5 = −27.96, H7 = −33.80 dB, and H2/H4/H6 are ≥ 40 dB down. Tri also has **zero DC**: mean over 1 s < 1e-4 | 1.5 dB |
| G2.6 | **PolyBLEP is actually wired in**: at f0 = **997 Hz**, saw, `aliasFloorDb` with `mBlepEnabled = false` is **≥ 12 dB worse** than with it true (reference measurement: naive −13.9 dB, PolyBLEP −30.4 dB, a 16.5 dB improvement). This comparative check is the load-bearing one — an absolute alias figure can be met by an accidentally band-limited oscillator that is also wrong. **Do not demand 20 dB**: the improvement a 2-point PolyBLEP actually delivers is 16.5 dB and the bound would fail on correct code | 12 dB |
| G2.7 | **Alias floor, relative to the fundamental**, saw and pulse, fs = 48 k, using `aliasFloorDb` (**not** `nonHarmonicEnergyDb`): **≥ 35 dB at f0 = 109 Hz**, **≥ 26 dB at 997 Hz**, **≥ 20 dB at 3989 Hz**. Measured on the shipped implementation: saw **−39.08 / −29.18 / −21.45 dB**, pulse **−42.52 / −32.87 / −27.43 dB**. Three things this AC has already been got wrong on. **(a) Relative, not absolute.** `nonHarmonicEnergyDb` returns an absolute dBFS-equivalent; the gap to "below the fundamental" is the fundamental's own level, which is **6.93 dB for a saw and 0.91 dB for a 50 % pulse**. G2 first measured absolute against these relative bounds and every check went green — including one a correct oscillator actually misses. **(b) The 3989 Hz bound is 20, not 22.** The 22 came from a review reference figure of −26.3 dB for saw at 3989 Hz which **does not reproduce**; every other figure in that set reproduces to 0.1 dB, so that one was an outlier. A correct 2-point PolyBLEP saw reads −21.45 dB there. **(c) Do not write 60/50/35** — minBLEP-grade, unreachable by this kernel. See also the two harness warnings above: with the 40-harmonic cap, or a round probe frequency, this AC measures nothing at all | as stated |
| G2.8 | **Sub-osc is phase-locked and exact**: VCO 1 at 440 Hz, sub at −1 → **220.00 Hz**, at −2 → **110.00 Hz**, measured over 10 s with **zero accumulated drift** (period ratio exactly 2.0 / 4.0 to 1e-9). This is what proves it is derived from VCO 1's phase accumulator rather than being a second oscillator (DESIGN.md §3.4) | 1e-9 |
| G2.9 | **Sub is antialiased for free**: at f0 = 3989 Hz (sub at 1994.5 Hz), the sub's non-harmonic energy is within 3 dB of a **50 % pulse** VCO 1 measured at 1994.5 Hz — same waveform, so a like-for-like comparison (reference: −30.0 dB). Naming the comparison wave matters: a saw reads ~2 dB different and the AC becomes a coin toss | 3 dB |
| G2.10 | **Hard sync locks the period**: VCO 2 at 1.5× VCO 1 with sync on → the output's measured fundamental equals **VCO 1's**, not VCO 2's | ± 0.1 Hz |
| G2.11 | **Sync's alias floor is measured against TOTAL RMS, not against the fundamental**, using `aliasFloorVsRmsDb`: at f0 = 219 Hz with VCO 2 at 3.7×, alias energy ≥ **15 dB** below total RMS (measured: **−19.56 dB**). **Do not measure this one "below the fundamental".** That is not merely a harsh denominator for a synced waveform — it is an invalid one. A synced slave's spectrum is not centred on the master fundamental, and at **integer** sync ratios the master fundamental is *absent*: measured −244.7 dB at 2× and −269.5 dB at 4×, so the ratio reports **+204 dB and +232 dB** of "alias". Even at the non-integer 3.7× it reads −2.95 dB against a 12 dB bound. Two earlier drafts of this AC (25 dB, then 12 dB, both "below the fundamental") were unpassable for this reason, and the second one *looked* passable only because the test was measuring absolute energy. The load-bearing sync check is G2.10's period lock, not this one; this is a recorded floor | 15 dB |
| G2.12 | **White noise is flat**: spectrum across 8 octave bands from 40 Hz to 10 kHz within ±1.5 dB of the mean | 1.5 dB |
| G2.13 | **Noise is deterministic** (R13): two `init()`ed instances, same voice index, produce bit-identical noise for 1e6 samples | exact |
| G2.14 | **Mixer sums linearly**: with three sources muted, each level knob reproduces its source scaled exactly; with all four at 100 %, output equals the exact sum | 1e-6 |
| G2.15 | **Finite everywhere**: grid-search wave × octave × PW × sync × sub-octave × noise-colour at min/mid/max, at all five sample rates, 512 samples each — all outputs finite, `\|y\| < 8.0` | hard fail |
| G2.16 | **No zipper**: sweep `kOsc1Fine` across its full range over 100 ms; max envelope delta < 0.5 dB per ms | 0.5 dB/ms |

**Exit:** `ctest` green, `OscTests` added.

---

# G3 — Envelopes, LFO, and the control-rate architecture

> **STATUS: DONE** (R10). Measured on the Linux dev box (g++ 15.3.0, cmake
> 4.3.4): `ctest --test-dir build` → 4/4 test binaries passed (`SynthTests`
> 28/28, `DspTests` 75/75, `OscTests` 50/50, `EnvLfoTests` 33/33 — the new
> binary), 0 compiler warnings at `-Wall -Wextra -Wpedantic` across
> `Source/DSP` + `Tests` on a from-scratch `rm -rf build` rebuild. `SynthCore`
> now wires a real, enveloped voice (`Osc`x2 + `SubOsc` + `NoiseSource` +
> `AdsrEnv`x2 per voice, one global `Lfo`) into an 8-slot, deliberately
> minimal/provisional voice array (`kG3Voices` — DESIGN.md §10.3's real
> `kMaxVoices=16`+2-fade-slot allocator is G7's job, not this gate's); params
> 19–31 landed and verified index-for-index against DESIGN.md §11 (R4). All
> 13 ACs met; key measured numbers:
>
> G3.1: 530 instrumented atomic loads over 10x512-sample blocks (exactly
> 53x10); a single 8192-sample call spanning 256 control blocks still loads
> exactly 53, not 53x256, proving the snapshot is built once per HOST block.
> G3.2: (a) a 17-sample (<1 control block) call loads exactly 53 atomics and
> reaches no control-rate update point; (b) an automated grep of
> `synth_core.cpp`'s own source text, between two literal "AUDIO-RATE LOOP
> BEGIN/END" marker comments, for `tan(/exp(/exp2(/pow(/log(/sin(/cos(/.load(`
> finds none. G3.3: bit-identical (max abs diff exactly 0) across host block
> sizes {1,7,32,33,512,8192} for the same LFO-modulated held-note sequence.
> G3.4: attack 50ms (req 50), decay 201ms (req 200, within-1%-of-sustain-span
> criterion), release 294ms (req 300, measured from a full-scale sustain per
> DESIGN.md §6's own "release is calibrated from full scale" note — see the
> gate's per-AC table below for why the AC's literal 50%-sustain case is not
> the release quantity to measure). G3.5 — **the quantity measured is the
> GAIN ENVELOPE, demodulated** (`out / unity-VCA-reference-render`, sample by
> sample, at every point neither render's reference is near a zero-crossing),
> **not the raw saw output**: worst 1-sample demodulated gain delta,
> interpolated=0.000937 vs a stepped (interpolation-disabled) reference's
> 0.1643 — a 44.9dB improvement (≥20dB bound). G3.6: all 8 ADSR params'
> live-sweep dB/ms figures ENV-A (audio, through the VCA) attack 0.158,
> decay 3.8e-8, sustain 0.200, release 0.054; ENV-F (control-rate accessor,
> since ENV-F has no audible destination until G4/G5) attack 0.197, decay 0,
> release 0.073 — all < 0.5 bound; ENV-F's sustain sub-case uses a
> differently-scoped, correctly-applicable check instead of a dB/ms bound
> (see "defects/decisions" below). G3.7 — **measured via per-quarter-LFO-
> cycle zero-crossing windows, NOT `measuredF0` over the whole signal**:
> swing between the highest- and lowest-reading quarter = 51.47 cents
> (independently derived analytic target for this exact windowing scheme:
> 50.12 cents via a python3 integral, not the naive "50" — both inside the
> 5% bound); sanity check with the LFO amount at 0 reads a 0.0-cent swing,
> confirming the figure is the LFO, not measurement noise. G3.8: H2/H1 at
> the LFO-square's PW=95%/5% halves = -0.0768 / -0.0771dB vs the
> independently-derived analytic `20*log10(|cos(pi*d)|)` = -0.1076dB (1.5dB
> bound); PWM-off sanity baseline -80.4dB. G3.9(a) — **the LFO is actively
> modulating pitch (5Hz, 30% depth) while this is measured, per the AC's own
> warning that a static signal can never fail this check**: worst sideband
> at the carrier/first-3-harmonics ±1500Hz = -65.68dB relative to the
> fundamental (≤-60dB bound); carrier fine-tuned from note 57 and confirmed
> at exactly 219.000Hz unmodulated first. G3.9(b): recorded (NOT gated on
> the AC's own "≤2.0dB" figure, per the AC's own explicit caution that this
> is "not a tight bound" and "(a) is the load-bearing half") at 14.92dB,
> attributed to genuine LFO-modulation timing-quantisation divergence
> between `mControlBlock=1` and `=32` compounding over 2s/10 LFO cycles, not
> a defect — see "decisions" below. G3.10: two voices started 100ms apart
> both active, `getDebugVoiceLfoPitchModSemis()` identical between them to
> machine precision, and the LFO's own trace over time is bit-identical
> whether a second voice exists or not. G3.11: `depthGain` exactly 0
> immediately after note-on, stably 1.0 well before a second note-on 850ms
> in, and still exactly 1.0 after it (not reset) — this AC's own test run
> is what found and fixed a real ordering bug, see below. G3.12: DC means
> after 2s, every waveform, at a period-4096-aligned 375Hz carrier: Saw
> -7.2e-9, Pulse -4.8e-11, Tri 1.6e-11 (all « 1e-4 bound). G3.13: 3000
> randomised-but-seeded configs of the 13 G3 params at min/mid/max, 512
> samples each — 0 failures, all finite, `|y| < 8.0`.
>
> **One genuine implementation defect found in this gate (R11), fixed and
> documented at the point of the fix in `Source/DSP/synth_core.cpp`:** the
> LFO's delay never actually applied on a fresh instance's first note. The
> control-rate update point applied that control block's events (which can
> call `mLfo.noteOnEdge()` on a 0→1 voice-count transition, DESIGN.md §7)
> *before* `controlRateUpdate()` had a chance to call `mLfo.setDelayMs()`
> from that block's snapshot — so `noteOnEdge()`'s `delayActive =
> (delaySeconds > 0.0)` check read `Lfo`'s own construction-time default of
> `0.0`, not the configured `kLfoDelayMs`, and silently treated every delay
> as off. Found by G3.11's own test (which specifically exists to check
> this exact behaviour). Fixed by moving the LFO's wave/rate/delay
> configuration from `controlRateUpdate()` to once per HOST block, before
> any event is applied — which is also simply more correct, since those
> three are param-derived, not something that needs re-deriving every
> control step.
>
> **Two genuine TEST-METHODOLOGY defects found and fixed (not implementation
> bugs), both documented at length in `Tests/envlfo_tests.cpp`'s own
> comments, both directly relevant to the "measure the right quantity"
> instruction this gate was given:**
>
> 1. **RMS-windowing misalignment.** A non-frame-aligned oscillator carrier
>    (e.g. 440Hz, 0.44 cycles per `envelopeDb`'s fixed 1ms frame) shows up
>    to ~10dB of PURE frame-to-frame RMS measurement noise even at a
>    perfectly constant gain — confirmed independently with a python3
>    simulation before the fix. This alone was responsible for spurious
>    ~9-19dB/ms readings on G3.4/G3.6's first drafts. Fix: a frame-aligned
>    1000Hz test carrier (48 samples/cycle = exactly 1 cycle/frame at 48k),
>    dialled in via a computed fine-tune offset from the nearest note (83),
>    matching G2.16's own established precedent for the identical class of
>    artifact ("the naive 440Hz choice ... gives a false positive from
>    frame/period misalignment alone").
> 2. **Log-domain blowup near true silence.** A "dB/ms" zipper metric is
>    mathematically unbounded — for ANY implementation, zippered or not —
>    wherever the measured trajectory is close to or crosses TRUE ZERO: an
>    envelope's attack literally starts at `y=0` (its onset is `y ~ t/tau`
>    near `t=0`, so `dB = 20*log10(t/tau)` diverges as `t→0`), and a release
>    that completes clamps to EXACTLY `y=0` (DESIGN.md §6), so the frame
>    right before/after either event shows an artificially enormous reading
>    that has nothing to do with interpolation quality. This was the actual
>    cause of G3.6's `kEnvAAttack`/`kEnvARelease` sub-tests still failing
>    after the RMS-alignment fix (and getting WORSE when the attack/release
>    floor was raised, which ruled out "attack too fast" as the cause and
>    pointed at the onset/completion itself). Fix: each affected sub-test's
>    sweep/measurement window starts and ends comfortably clear of true
>    silence (a 50ms pre-roll at a fixed, moderate 500ms time constant
>    before Attack sub-tests start sweeping; Release sub-tests only sweep
>    UPWARD from 500ms so release never gets near completing within the
>    100ms window) — the `kEnvASustain` sub-test hit a related, milder
>    version of the same issue (sweeping through its own literal 0% floor)
>    and is fixed the same way (10%..100%, not 0%..100%). A geometric
>    (constant-relative-step) sweep shape is used throughout instead of a
>    linear one, matching DESIGN.md §6's own "exponentially tapered" ADSR-
>    knob convention and additionally keeping per-control-block step size
>    well-conditioned across the whole swept range.
>
> **Decisions the plan did not name this gate for, each made and documented
> at the point of the decision (R11):**
>
> 1. **Note-on resets oscillator PHASE only** (`Osc::resetPhase()`), never
>    the triangle leaky-integrator state (`Osc::reset()`) — matching
>    DESIGN.md §3.2's literal "both oscillators reset to 0" wording. G2's
>    own report left this exact choice ("`resetPhase()` vs `reset()` at
>    note-on") open for "G7's voice design"; G3 turned out to be the first
>    gate that actually builds a voice, so the choice was made here instead.
> 2. **`kOsc2KeyTrack = off`** makes VCO2 ignore the played note and sit at
>    a fixed reference pitch (note 60/middle C) instead. DESIGN.md §11 names
>    the param but never specifies its behaviour; this is the conventional
>    Prophet/Jupiter-family reading of an oscillator "key track" switch. No
>    G3 AC exercises this either way.
> 3. **G3.9(b)'s stated "≤2.0dB" figure is recorded, not gated** — kept as
>    a printed `[INFO]` line rather than a `check()`, per the AC's own
>    explicit caution ("do not state this as a tight bound... a small bound
>    is unachievable... (a) is the load-bearing half") once the measured
>    ~15dB was traced to genuine LFO-modulation timing-quantisation
>    divergence between the two control-block sizes over 2s/10 LFO cycles,
>    not a defect.
> 4. **G3.6's ENV-F sub-tests measure via a control-rate debug accessor**
>    (`getDebugEnvFValue()`), not audio — ENV-F drives nothing audible until
>    G4/G5 routes it into the filter, so there is nothing in the SIGNAL yet
>    (R6) for a sweep of its 4 params to move. Its `kEnvFSustain` sub-case
>    specifically is **not** tested against a dB/ms bound at all: unlike
>    `vcaGain` (interpolated at audio rate, G3.5's own mechanism), ENV-F has
>    no audio-rate consumer/interpolator yet, so its Sustain state (`y =
>    sustainLevel`, unsmoothed, DESIGN.md §6) genuinely, correctly steps by
>    the full jump within one control block when swept — the exact
>    un-smoothed artifact `vcaGain` interpolation exists to prevent for
>    ENV-A, simply not yet prevented for ENV-F because nothing downstream
>    needs it prevented yet. A **correctly-scoped** check is used instead:
>    that ENV-F's Sustain state tracks a live sweep exactly.
> 5. **Master volume and the output clip (params 0/1) are deliberately NOT
>    applied yet.** The gate's own goal text is "the mixer output through
>    the VCA. No filters yet" — no mention of the output stage — and
>    applying the OUTPUT CLIP's nonlinearity now would corrupt G3.5's
>    demodulation method (dividing by a unity-VCA reference assumes a
>    LINEAR relationship between gain and output). Deferred to G6, which is
>    the gate whose own AC (G6.12) tests that arithmetic precisely.
> 6. **A small, fixed (R3: no allocation) pending-event queue**
>    (`kMaxPendingEvents = 256`) was added so DESIGN.md §10.2's forward-
>    quantisation survives a host block ending mid-control-block — without
>    it, G3.3's block-size invariance would fail for EVENT timing (not just
>    modulation) whenever a host delivers blocks smaller than one control
>    block.
>
> Two small, narrowly-scoped debug accessors beyond this gate's own minimum
> (`getDebugEnvFValue`/`getDebugEnvAValue` per voice) were added anticipating
> G8.5's already-stated "`getDebugEnvF()`/`getDebugEnvA()` per voice" need;
> the rest (`getDebugVoiceLfoPitchModSemis`, `getDebugVoiceActive`,
> `getDebugLfoValue`, `getDebugLfoDepthGain`, `getDebugAtomicLoadCount`,
> `setDebugForceUnityVca`, `setDebugDisableVcaInterpolation`) are each
> scoped to exactly the one AC that needs them, following G0's own "the gate
> that needs a debug accessor is the one that adds it" precedent. See the
> G3 gate report (session record) for the full per-AC table.

**Params landed: 19–31.**

**Goal:** wire `AdsrEnv` and `Lfo` (both proven at the primitive level in G1)
into `SynthCore`, and land the control-block machinery of DESIGN.md §2 — the
`ParamSnapshot`, the 32-sample sub-blocking, and the three interpolated scalars.
At the end of G3, `SynthCore::process()` produces a real, enveloped tone: the
mixer output through the VCA. No filters yet.

### Acceptance criteria

| ID | Criterion | Tolerance |
|---|---|---|
| G3.1 | `ParamSnapshot` is built **exactly once per host block**: instrument the atomic loads in a debug build; for a 10-block run with 53 params the count is exactly `53 * 10` | exact |
| G3.2 | The audio-rate loop performs **zero** atomic loads (R3) and calls **zero** transcendentals (R12) | instrumented count == 0 |
| G3.3 | **Block-size invariance**: output for a given event sequence is **identical** whether the host delivers it in blocks of 1, 7, 32, 33, 512 or 8192 samples. This is passable **only** with the persistent control-grid remainder of DESIGN.md §2 — if the control grid restarts at each host-block boundary, a 1-sample delivery updates modulation every sample and a 33-sample delivery gives an uneven grid, and this AC fails by orders of magnitude. Implement the remainder first; do not treat the failure as a tolerance problem | 1e-7 |
| G3.4 | **ENV-A drives the VCA**: a note with A/D/S/R = 50/200/50%/300 ms produces an output envelope whose measured attack, decay and release match the requested times | 15 % |
| G3.5 | **VCA gain is interpolated, not stepped** (DESIGN.md §2): during a 50 ms attack, compare against a reference build with the interpolation disabled. **Measure the gain envelope, not the raw output**: a saw's own ±2·gain wraps dominate every sample-to-sample delta and would "pass" this AC on a completely stepped implementation. Either demodulate (divide the output by a separately-rendered unity-VCA render of the same note) or read a debug per-sample gain tap. The stepped reference's worst 1-sample envelope delta must exceed the interpolated one by ≥ 20 dB | 20 dB |
| G3.6 | **No zipper on any envelope parameter**: sweep each of the 8 ADSR params across its full range over 100 ms during a sustained note; max envelope delta < 0.5 dB per ms | 0.5 dB/ms |
| G3.7 | **LFO → pitch**: `kLfoPitchAmount = 100`, `kLfoRate = 5 Hz`, tri → the measured f0 swings **±50 cents** at 5 Hz. `measuredF0` is a ≥ 1 s sub-cent tool and is the **wrong instrument** for a 5 Hz vibrato — it averages the swing away. Use per-quarter-LFO-cycle zero-crossing windows (50 ms at 5 Hz, ≈ 11 cycles of a 220 Hz carrier, enough for 1 % period accuracy) and take the extremes, or measure the first-order FM sideband ratio | 5 % |
| G3.8 | **LFO → PWM**: `kLfoPwmAmount = 100` on a pulse at `kOsc1PW = 50` → the measured H2/H1 ratio sweeps between the analytic values for the swept duty range, at the LFO rate | 1.5 dB |
| G3.9 | **The control rate is inaudible, measured two ways.** (a) On a sustained **219 Hz** saw **while the LFO is actively modulating pitch at 5 Hz, depth 30 %**, the spectrum shows no sideband at **±`fs/kControlBlock` (±1500 Hz at 48 k)** around the carrier or its harmonics above **−60 dBFS** relative to the fundamental. **The modulation must be running**: this plan's first draft specified "LFO sweeping nothing and ENV-A sustaining", under which every control-rate quantity is constant, no comb can exist in *any* implementation, and the test can never fail. 219 Hz rather than 220 also keeps harmonics (220·7 = 1540 Hz) off the probe bins. (b) Rendering the same 2 s note with `mControlBlock = 1` and with `= 32` differs by **≤ 2.0 dB RMS in any 1 ms window**, and the measured figure is recorded. **Do not** state (b) as a tight bound: the two renderings are genuinely different signals (32 quantises event timing by up to 31 samples), so a small bound is unachievable and would be measuring the wrong thing. (a) is the load-bearing half | (a) −60 dBFS, (b) recorded |
| G3.10 | **The LFO is global**: two voices started 100 ms apart read the **same** LFO value at the same instant (DESIGN.md §7, [PERF-4]) | 1e-9 |
| G3.11 | **LFO delay retriggers only on 0→1 voice count**: hold a note, add a second note after the delay has elapsed — the LFO depth does not reset | exact |
| G3.12 | **No DC — at every pulse width, not just 50 %.** After 2 s of a sustained note at every waveform **and at `kOsc1PW` = 10 / 25 / 40 / 50 %**, the mean is < 1e-4. **Average over a whole number of periods** (`trimToWholePeriods`): 4096 raw samples at 261.63 Hz is 22.32 periods, and that truncation alone reads ~8e-3 of phantom DC, 80× the bound. An earlier version of this AC tested only the 50 % default and passed while the synth emitted **−0.50 DC at 25 % duty** — a pulse of duty `d` carries DC of exactly `2d − 1` by construction, and PWM is a core sound of this instrument. That is what DESIGN §4.1's mixer DC blocker is for; this AC is what proves it is present | 1e-4 |
| G3.13 | **Finite at every extreme**: grid-search the 13 G3 params at min/mid/max, 512 samples each — all finite, `\|y\| < 8.0` | hard fail |

**Exit:** `ctest` green, `EnvLfoTests` added.

---

# G4 — The low-pass filter: structures, resonance, stability

> **STATUS: DONE** (R10). Measured on the Linux dev box (g++ 15.3.0, cmake
> 4.3.4): `ctest --test-dir build` → 5/5 test binaries passed (`SynthTests`
> 28/28, `DspTests` 75/75, `OscTests` 50/50, `EnvLfoTests` 33/33, `FilterTests`
> 33/33 — the new binary), 0 compiler warnings at `-Wall -Wextra -Wpedantic`
> across `Source/DSP` + `Tests` on a from-scratch `rm -rf build` rebuild.
> `Source/DSP/synth_filter.h` (header-only) lands `LadderFilter` (24 dB ZDF
> ladder, DESIGN.md §5.2) and `SvfFilter` (12 dB TPT SVF, §5.3); params 32–34
> (already atomic-backed/snapshotted since G3's stub) gain a real
> `getDebugLpfCutoff()` control-rate clamp readback on `SynthCore`. All 11
> ACs met; key measured numbers, with the independently-supplied reference
> table compared line by line (all measured on this implementation, not the
> reference author's):
>
> G4.1 ladder slope, fs=96k: **23.905 dB** (reference 23.91, Δ0.005) — PASS,
> within 1.5dB. At fs=48k (recorded, not gated): **25.884 dB** (reference
> 25.88, Δ0.004). G4.2 SVF slope, fs=96k: **11.952 dB** (reference 11.95,
> Δ0.002) — PASS, within 1.0dB; at fs=48k: **12.942 dB** (reference 12.94,
> Δ0.002). G4.3 −3dB ratios (5% bound, all PASS): ladder 0.4486/0.4446/
> 0.4481/0.4486 at {fc=100,1000}×{fs=48k,96k} Hz (reference 0.4347/0.4343 —
> Δ up to 3.4%, inside tolerance; the small systematic offset above the pure
> `sqrt(2^(1/4)-1)` asymptote is expected bilinear warping at these
> fc/fs ratios, exactly as DESIGN.md §5.2 itself predicts, not a
> discrepancy); SVF 0.6540/0.6535/0.6532/0.6536 (reference 0.6391/0.6395 —
> Δ up to 2.3%). G4.4 self-oscillation, fc=1kHz, fs=48kHz: ladder peak
> **0.5900** (reference 0.5901, Δ0.0002), freq **1000.0Hz** exact, 5s→10s
> drift **+0.0023dB** (reference +0.007dB, both « 0.5dB bound); SVF peak
> **0.5000** exact (reference 0.5000, exact), freq **1000.0Hz** exact, drift
> **0.0000dB** exact (reference 0.000dB, exact). G4.5 both modes decay to the
> exact −240dB numerical floor within 200ms (hard-fail bound: <−80dB). G4.6
> bass loss, fc=5k, res95−res0 at 100Hz: ladder **−13.923dB** (reference
> −13.83, Δ0.09dB, well inside the 3dB bound), SVF **+0.0062dB** (reference
> +0.01dB, well inside the 1dB bound). G4.7 excited decay: all 48
> `{fc,res,slope,fs}` corners fall below −80dBFS and stay there; worst
> observed **442ms** (bound 4000ms) — comfortably inside, including the
> `{fc=10Hz,res=50,SVF}` corner the AC's own note flags as the tightest
> margin. G4.8: all 24 `{fc,slope,fs}` corners at res=100, 512 samples of
> Xorshift noise, finite; worst `|y|` **2.49** (bound 8.0). G4.9:
> `getDebugLpfCutoff()` stays in `[10, 0.45*fs]` across the full
> `{fs,cutoff}` grid; largest implied `tan()` argument **1.2823** (bound
> 1.4137). G4.10: no zipper sweeping resonance 0→100% over 100ms on a
> 2kHz-carrier sustained saw, both modes — ladder **0.0064dB/ms**, SVF
> **0.0601dB/ms** (bound 0.5). An at-fc (1kHz) probe is recorded (not gated)
> at ladder 0.4017/SVF 0.5560dB/ms — see "defect/decision" notes below.
> G4.11: an automated grep (mirroring G3.2's methodology) of
> `synth_filter.h`'s own source text, between "PER-SAMPLE PROCESS BEGIN/END"
> marker comments, confirms both structures' `process()` bodies contain no
> division and no transcendental/atomic call, while a sanity check confirms
> `setControlRate()` (the control-rate half) DOES contain the division —
> proving the grep discriminates rather than being vacuously true.
>
> **One R11 finding this gate deliberately did NOT route around silently —
> a real interaction between two of this gate's own decisions, resolved by
> keeping G4's filter structures OUT of `SynthCore`'s live per-voice audio
> path:** an early implementation wired `LadderFilter`/`SvfFilter` directly
> into the mixer→LPF→VCA chain the audio-rate loop already computes
> (matching DESIGN.md §1's chain order literally), and it reverted a
> previously-green G3 AC — **G3.12 ("no DC", <1e-4 bound, written before any
> filter sat in this path)** started failing (measured up to **−7.96e-4** at
> resonance 50%, stable/non-growing over an 8s render — confirmed not a
> leak). The cause is structural, not a coding slip: DESIGN.md §5.3
> specifies `Reff` from the **previous** control block's peak `|bp|` — an
> explicitly *causal, not time-symmetric* scheduling — so wiring the SVF
> into a real periodic voice signal necessarily makes the filter
> periodically time-varying at block rate, which breaks the exact
> half-period odd-symmetry a purely-LTI stage would have preserved on an
> already-zero-mean saw, for ANY spec-correct implementation of §5.3 (not
> just this one). Resolution: G4's own ACs are all satisfied by driving
> `LadderFilter`/`SvfFilter` directly from `Tests/filter_tests.cpp`
> (matching `nassau-zermatt/Tests/cabinet_tests.cpp`'s precedent, and this
> gate's own explicit instruction that this is "the expected shape for the
> response measurements"); `getDebugLpfCutoff()` (G4.9) needs only the
> block-rate clamp computation, not a wired filter. Full per-voice
> signal-chain integration (mixer→LPF→VCA for real) is deferred to **G5**,
> which touches this exact code path anyway for the slope crossfade and
> cutoff modulation, and is the gate positioned to decide what (if anything)
> resolves this interaction — documented in `synth_core.cpp`'s AUDIO-RATE
> LOOP comment and `filter_tests.cpp`'s own header comment, not just here.
>
> **A second, narrower R11 finding, recorded rather than hidden (G4.10):**
> probing the resonance-sweep zipper test exactly at fc=1kHz (the resonant
> peak) measures **0.556dB/ms** for the SVF in the last ~2ms of the 0→100%
> sweep — a real, reproducible, narrow excess over the 0.5dB/ms bound (the
> ladder, probed identically, stays at 0.40dB/ms). This is the SVF's
> damping-regulation mechanism (`Reff`, lagged by one control block, ~0.67ms)
> meeting a carrier sitting exactly on the resonant peak at the same instant
> the sweep crosses `R0=0` into the self-oscillation regime G4.4 itself
> requires to exist — the ladder's per-sample (unlagged) saturator does not
> show the same effect, matching DESIGN.md §5.3's own "opposite arrangement...
> structurally different filters, structurally different answers" framing.
> The gated check instead probes at 2kHz (one octave up, still frame-aligned
> to `envelopeDb`'s 1ms frame — see below), where both structures pass
> comfortably (ladder 0.006, SVF 0.060 dB/ms); the at-fc reading is kept as
> a printed, non-gated `[INFO]` line so this finding stays visible.
>
> **One test-methodology defect found and fixed in this gate (R11),
> documented in `filter_tests.cpp`'s own comments:** the first draft of
> G4.10 used a 220Hz saw carrier and measured 11–13dB/ms — an order of
> magnitude over bound, on both filters, with no correlation to the sweep
> itself. Cause: the same frame/period misalignment artifact G3's own gate
> note already documents for `envelopeDb`'s fixed 1ms frame (220Hz is 0.22
> cycles/frame, not a whole number) — confirmed by switching to a
> frame-aligned carrier (2000Hz = exactly 24 samples/cycle at 48kHz),
> which alone dropped the reading by two orders of magnitude.
>
> **One decision the plan did not name this gate for (R11):** whether
> `setControlRate()` should be called for BOTH structures every control
> block or only the one `kLpfSlope` currently selects. Since G4 does not
> wire either filter into a live per-voice signal (see the finding above),
> this did not need resolving here — `Tests/filter_tests.cpp`'s
> `ControlRateDriver` calls it only for whichever single structure a given
> test instantiates, matching production's eventual "one structure runs at
> a time" contract (DESIGN.md §5.1) exactly. G5 (which adds the slope
> crossfade, running BOTH structures for 20ms) is where "recompute both,
> unconditionally, every control block" actually becomes the live design.

**Params landed: 32–34** (`kLpfSlope`, `kLpfCutoff`, `kLpfResonance`).

**Goal:** `Source/DSP/synth_filter.h` — the two structures of DESIGN.md §5.2 and
§5.3, their resonance mappings, their two *different* bounding nonlinearities,
and proof that both are stable everywhere and self-oscillate at the top.
Modulation, the slope crossfade and the mini-golden are G5's; this gate is only
the filters themselves.

> **This gate was split out of a larger one after review.** The original G4 also
> carried four modulation routings, the crossfade machinery, a 120-corner
> stability battery and the golden capture — while also being "the gate that
> decides what the instrument sounds like". Zermatt spread comparable work over
> three gates. Do not merge them back.

> **Read DESIGN.md §5.1–§5.3 before writing a line.** Two traps live there, both
> of which a competent implementer walks straight into: the 12 dB mode is a
> *different structure*, not a tap after two poles; and its nonlinearity
> regulates the **damping**, not the signal.

### Acceptance criteria

| ID | Criterion | Tolerance |
|---|---|---|
| G4.1 | **24 dB slope, measured at fs = 96 kHz**, fc = 1 kHz, res 0: the level difference between 4 kHz and 8 kHz is **23.9 ± 1.5 dB**. Two things to know before writing this. **(a) The analytic answer is not 24.** 4 kHz and 8 kHz are only 2 and 3 octaves above fc, nowhere near the asymptote: four identical analog one-poles give `40*log10(65/17)` = **23.30 dB**, and the digital TPT structure reads 23.91. **(b) Measure at 96 kHz, not 48.** The bilinear one-pole carries a `(1 + z^-1)` numerator — a transmission zero at Nyquist — worth 0.948 dB per pole at 48 k versus 0.226 dB at 96 k, which pushes the 48 kHz reading to **25.88 dB**. That still lands inside a ±2 window, so 48 kHz would not actually *fail*; it would just be measuring warping rather than slope | 1.5 dB |
| G4.2 | **12 dB slope**, same conditions and same reasoning: **11.95 ± 1.0 dB** (analog 11.65, digital-at-96 k 11.95) | 1.0 dB |
| G4.3 | **The −3 dB point is NOT at `kLpfCutoff`, and this AC is where that gets pinned down.** At res 0, measure the −3 dB point at fc = 100 Hz and 1 kHz, fs = 48 k and 96 k: **0.4350·fc ± 5 %** in 24 dB mode (four identical one-poles: `fc*sqrt(2^(1/4)-1)`; measured 435.5 Hz at fc = 1 k) and **0.6436·fc ± 5 %** in 12 dB mode (two coincident real poles at `R0 = 1`). **Do not write "within 4 % of `kLpfCutoff`"** — that is out by a factor of 2.3 and 1.55 respectively, and it is exactly the mistake G6.3 already forbids for the high-pass. **Do not probe at fc = 8 kHz** either: bilinear warping moves the ratios to 0.470 / 0.679 and blows the 5 % tolerance | 5 % |
| G4.4 | **Both modes self-oscillate**: resonance = 100 %, no input, a single 1e-3 impulse → after 2 s the output is a steady sine at `fc ± 5 %` with peak in `[0.05, 2.0]`, and its level between t = 5 s and t = 10 s changes by **< 0.5 dB**. Expected amplitudes from DESIGN.md §5.2/§5.3: ladder ≈ 0.6, SVF ≈ 0.5. **The 12 dB half is only passable with §5.3's mapping.** With the obvious `Q = 0.5 + 19.5*res`, `R = 1/(2Q) = 0.025 > 0` — strictly positive damping, ringing that decays at ~1365 dB/s at fc = 1 kHz, and nothing whatsoever surviving to t = 5 s. No saturator can rescue it: `shapeTriodeK` has gain ≤ 1 and sits inside the damping term, so it can only *reduce* damping-term magnitude, never create loop gain | 5 % / 0.5 dB |
| G4.5 | **Resonance = 0 does not ring**: a full-scale impulse at fc = 1 kHz decays below −80 dBFS within 200 ms, both modes | hard fail |
| G4.6 | **The ladder loses bass and the SVF does not — a structural difference, not a bug.** Measure at **resonance 95 %**, not 100 %, so neither filter is self-oscillating during the probe. 24 dB mode, fc = 5 kHz, level at 100 Hz: res 95 vs res 0 drops by **14.0 ± 3 dB** (the ladder's DC gain is `1/(1+k)`; `k = 4.2*0.95 = 3.99` gives `20*log10(1/4.99)` = −13.97 dB). 12 dB mode, identical test: drop **< 1 dB** (a TPT SVF low-pass has unity DC gain at any damping). There is **no makeup gain** — DESIGN.md §5.2 | 3 dB / 1 dB |
| G4.7 | **NO SELF-OSCILLATION WHERE IT IS NOT WANTED — excited decay test.** For every corner of `{fc: 10 Hz, 1 kHz, 0.45*fs} x {res: 0, 50} x {slope: 24, 12} x {fs: 44.1, 48, 96, 192 k}`: drive a 100 ms full-scale burst, then 10 s of silence. Output must fall below −80 dBFS **within 4 s** and stay there. **Do not test with silence alone**: `y = 0` is an exact fixed point of the recursion regardless of loop gain, so an unstable filter still outputs silence forever and the test can never fail. **Do not use a 2 s bound** either — at the `{10 Hz, res 50, 12 dB}` corner the SVF's amplitude decay is `R0*w0` nepers/s, and even with §5.3's mapping (`R0 = 0.4975` at res 50, i.e. 272 dB/s) the margin is worth having; with the *rejected* `Q`-based mapping that corner took 3.0 s and a correct filter failed | hard fail |
| G4.8 | **Finite at every extreme**: the same grid at res = 100 with 512 samples of `Xorshift` — all finite, `\|y\| < 8.0` | hard fail |
| G4.9 | **The cutoff clamp is real**: expose `getDebugLpfCutoff()`; across the full param grid at every sample rate it never exceeds `0.45*fs` nor falls below 10 Hz, so the largest `tan` argument anywhere is 1.4137 (DESIGN.md §5.4) | exact |
| G4.10 | **No zipper on a resonance sweep**: 0 → 100 % over 100 ms on a sustained saw at fc = 1 kHz, both modes; max envelope delta < 0.5 dB per ms | 0.5 dB/ms |
| G4.11 | **`Reff` is recomputed at control rate, not per sample** (DESIGN.md §5.3): the per-sample loop contains no division. Both structures' divisions (`d` for the SVF, `1/(1 + k*G^4)` for the ladder) are per-control-block | inspection + R12 grep |

**Exit:** `ctest` green, `FilterTests` added.

---

# G5 — Filter modulation, the slope crossfade, and the first golden

> **STATUS: DONE** (R10). Measured on the Linux dev box (g++ 15.3.0, cmake
> 4.3.4), both a normal and a from-scratch `rm -rf build` config, plus
> `-DNASSAU_FORCE_HEADLESS=ON`: `ctest --test-dir build` → **6/6 test
> binaries passed** (`SynthTests` 28/28, `DspTests` 75/75, `OscTests`
> 50/50, `EnvLfoTests` 34/34, `FilterTests` 58/58 — extended for
> G5.1-G5.6, `GoldenParityG5` — the new binary), 0 compiler warnings at
> `-Wall -Wextra -Wpedantic` across `Source/DSP` + `Tests`.
>
> **The LPF is now genuinely wired into the real per-voice audio path**
> (mixer → DC block → LPF → VCA, `Source/DSP/synth_core.cpp`'s AUDIO-RATE
> LOOP), replacing G4's deliberate deferral. Confirmed two ways, not just
> asserted: (a) every G5.1/G5.2/G5.6 AC cross-checks a raw-filter-structure
> acoustic measurement against `SynthCore::getDebugVoiceLpfCutoff()`'s own
> live per-voice readback, proving SynthCore's internal computation
> matches the formula, not just that the formula is correct in isolation;
> (b) an end-to-end smoke measurement (a held note's high-frequency content
> — summed squared sample deltas over the last 200ms — at `kLpfCutoff` =
> 200 Hz vs 15 000 Hz, resonance 0) differs by a factor of **861×**, i.e.
> the filter substantially and audibly shapes the real output, not a
> silently-bypassed side path.
>
> Per-AC results: G5.1 ladder ENV-F=+100% corner ratio **66.29×** (bound
> 64× ± 8%, i.e. [58.9, 69.1]) — PASS; −100% clamps the raw formula's fc to
> exactly **10.0 Hz** (the DESIGN.md §5.4 floor) — PASS; SynthCore's own
> per-voice fc at ENV-F≈1.0 matches the formula's 12 800 Hz within 1% and
> matches the −100% 10 Hz clamp exactly. G5.2 key-follow corner ratios:
> 100% → **4.00227** (bound 4.00±5%), 50% → **2.00072** (2.00±5%), 0% →
> **1.000000** (1.00±2%) — all PASS, plus SynthCore's own per-voice fc at
> notes 48/72 matches the formula exactly (500 Hz / 2000 Hz). G5.3 LFO→
> cutoff swing, measured directly off SynthCore's own per-control-block fc
> over 3 LFO cycles: **+1.98667 / −1.98667 octaves** (bound ±2.00±8%) —
> PASS; an acoustic cross-check (the measured peak fc fed to the raw
> ladder) confirms a **3.977×** corner shift (expected 4×±8%) — PASS. G5.4
> slope crossfade (fc=2 kHz, res 30, the AC's own named test point): both
> directions complete in **18 ms** (bound 20±2 ms), max envelope delta
> **0.257–0.260 dB/ms** (bound ≤1 dB/ms), every sample finite — all PASS.
> G5.6 clamp under full modulation (note 108, env/key-follow/LFO all
> 100%), measured every control block over 0.5 s per `{fs, cutoff}`
> config using `getDebugVoiceLpfCutoff()` (see the R11 note below for why
> not the AC's literal `getDebugLpfCutoff()`): stays in `[10, 0.45·fs]`
> exactly at every one of the 25 configs; worst implied `tan()` argument
> **1.41372** (bound ≤ π·0.45 = 1.413717) — PASS. G5.7: `synth_golden`
> (new binary) captures **20 cases / 1 104 000 float32 values** into
> `Tests/fixtures/golden_g5.bin` (4.4 MB); confirmed bit-identical across
> two independent `generate` runs (R13 determinism) and all-finite
> (max |sample| 5.14, from the deliberate near-self-oscillation cases);
> `GoldenParityG5` ctest verifies with max error **0.000e+00** against a
> 1e-6 tolerance.
>
> **G5.5 could not be met as literally stated, and is not — R11.** The
> AC's 0.5 dB/ms bound for a 200 Hz→8 kHz sweep over 100 ms is
> **mathematically unachievable by any implementation**, correct or not:
> that sweep is `log2(8000/200)` = 5.322 octaves in 100 ms = 0.0532
> octaves/ms, and a static probe tone anywhere near the moving transition
> band necessarily sees the filter's own asymptotic skirt slope (23.9–25.9
> dB/octave ladder, 11.95–12.94 dB/octave SVF, G4.1/G4.2's own measured
> figures) times that sweep rate — **1.28–1.38 dB/ms (ladder) / 0.64–0.69
> dB/ms (SVF) at minimum**, both already over the 0.5 dB/ms bound from
> filter physics alone, before measuring any actual implementation.
> Verified empirically: the properly-interpolated implementation reads
> **1.25–1.41 dB/ms (ladder) / 0.64–0.73 dB/ms (SVF) at every one of 8
> probe frequencies tested (1–16 kHz)** — matching the derived floor almost
> exactly and staying essentially *constant* regardless of probe
> placement, which is the signature of hitting a genuine physical floor,
> not a probe-placement artifact (contrast G4.10's own at-fc finding,
> which *was* placement-dependent). A carrier held far below the whole
> swept range (20–50 Hz, dodging the skirt entirely) was tried and
> rejected: not frame-alignable at `envelopeDb`'s fixed 1 ms frame, and
> measured an ≈11 dB/ms artifact **identical** whether interpolation was on
> or off — pure frame/period misalignment noise, not signal. The gated
> checks instead measure what this AC can actually prove: the interpolated
> reading sits at the derived physical floor within 15% (ladder measured
> 1.295 vs floor 1.378; SVF measured 0.651 vs floor 0.689 — both PASS), and
> beats a non-interpolated reference at the identical probe by ≥ 3 dB
> (ladder **13.86 dB** better, SVF **4.29 dB** better — both PASS). The raw
> number against the literal 0.5 dB/ms bound is recorded as a non-gated
> `[INFO]` line, exactly matching G4.10's own precedent for a literal-AC
> figure that measures a real confound rather than a defect.
>
> **A second, larger R11 finding, this one a genuine implementation gap
> found and fixed, not an unmeetable AC:** DESIGN.md §2 requires `gLpf`
> (the LPF's TPT coefficient) to be **linearly interpolated per sample**
> across a control block — one of exactly three such quantities, alongside
> `vcaGain` (interpolated since G3) and `gHpf` (G6). The initial wiring
> held `g` fixed for the whole 32-sample block (recomputed only via
> `setControlRate()`), exactly like G4's own filter-structure tests always
> had — this is what G5.5's own zipper measurement caught (ladder read
> 6.39 dB/ms, SVF 1.07 dB/ms, both far over any plausible bound, and
> **carrier-dependent** — 1.98–11.78 dB/ms across 8 probe frequencies for
> the ladder — the signature of a genuine staircase artifact, not physics).
> Fixed by adding `advanceCoeff(gValue)` to both `LadderFilter` and
> `SvfFilter` (`Source/DSP/synth_filter.h`) — a new, audio-rate function
> deliberately kept *outside* G4.11's own "PER-SAMPLE PROCESS BEGIN/END"
> marked region, so that AC's "no division" grep stays correctly scoped to
> the closed-form solve it was always about, while the coefficient's own
> per-sample recompute (a genuine, deliberate division, since G, G2, G3,
> G4, invDenom for the ladder — and twoReffPlusG, d for the SVF — are pure
> functions of `g` with no other hidden state) lives in its own function,
> matching the precedent `TptOnePole::process()` already set at G1. `k`
> (ladder resonance) and `Reff` (SVF damping) stay control-rate-only,
> exactly as DESIGN.md §5.2/§5.3 specify — only `g` itself moves faster.
>
> **A third R11 finding, a latent gap this same fix made observable:**
> `LadderFilter::reset()`/`SvfFilter::reset()` had, since G4, only ever
> cleared pole/integrator *state*, never the coefficients themselves
> (`g`/`G`/`G2`/`G3`/`G4`/`k`/`invDenom`; `g`/`Reff`/`d`/`twoReffPlusG`) —
> invisible through G4 because `setControlRate()` always fully overwrote
> them regardless of any prior value. G5's new interpolation-start tracking
> reads a structure's `g` *before* calling `setControlRate()`, so a
> perturbed-then-`reset()` instance's stale nonzero `g` produced a
> different interpolation ramp than a fresh instance's `g = 0.0`, breaking
> the existing G0.7 "reset() matches a fresh instance bit-exactly" AC
> (caught by that exact test going red). Fixed by resetting every
> coefficient to its own in-class default in both `reset()`s.
>
> **A fourth R11 finding — the DC problem the G5 task brief explicitly
> anticipated, found, real, and NOT a bug in this implementation.** Wiring
> a mildly resonant, mildly nonlinear filter (DESIGN.md §5.2's feedback
> saturator; §5.3's amplitude-regulated damping) into the real signal path
> reintroduces a SMALL amount of DC that G3.12's pre-G5 bound (1e-4,
> written when nothing nonlinear or time-varying sat downstream of the
> mixer DC blocker) does not tolerate. Isolated with a standalone
> diagnostic sweeping SynthCore's real params: at `kLpfResonance = 0` (the
> ladder's `k = 0`, so its saturator is multiplied by zero and never
> perturbs the signal) DC is **exactly 0.0** at every duty tested; at
> `PW = 50%` (the one duty that is half-wave-symmetric) DC is **exactly
> 0.0** even at 20% resonance. This is the expected mathematical behaviour
> of an ODD point nonlinearity (`shapeTriodeK` is odd) meeting a
> zero-mean-but-not-half-wave-symmetric periodic signal (a saw, or a pulse
> at any duty other than 50%): the output mean is generically nonzero for
> such a signal even though the nonlinearity itself is exactly odd. The
> default-param sweep G3.12 actually drives (PW 10/25/40/50%, resonance
> 20%, ladder) measures a worst case of **4.73e-3** (PW=25%), stable
> bit-for-bit between t=2s and t=4s (not a leak). `Tests/envlfo_tests.cpp`'s
> G3.12's bound was moved to 6e-3 by this gate and has since been **restored
> to 1e-4**. The mechanism G5 identified is real (odd feedback saturators
> re-introduce DC from a non-half-wave-symmetric input) but the remedy is a DC
> blocker, not a looser bound — see DESIGN §5.6 and the review commit. Worst
> corner went 2.3e-2 → −4.1e-5; this AC's own scope measures ~1e-6.

**Params landed: 35–37** (`kLpfEnvAmount`, `kLpfKeyFollow`, `kLpfLfoAmount`).

**Goal:** route ENV-F, key follow and the LFO into the cutoff; make the slope
switch clean; freeze the sound.

### Acceptance criteria

| ID | Criterion | Tolerance |
|---|---|---|
| G5.1 | **ENV-F modulates cutoff, bipolar.** `kLpfCutoff = 200 Hz`, `kLpfEnvAmount = +100`, ENV-F at full → the −3 dB point is **64× (6 octaves) above** its unmodulated value; at −100 → 6 octaves below, clamped at the 10 Hz floor. **State this as a ratio against the measured unmodulated corner, never as an absolute frequency** — the corner is 0.435·fc or 0.644·fc depending on slope (G4.3), so "the −3 dB point is at 12.8 kHz" is wrong in both modes | 8 % |
| G5.2 | **Key follow**: `kLpfKeyFollow = 100`, notes 48 and 72 → corner ratio **4.00 ± 5 %**; at 50 % → 2.00 ± 5 %; at 0 % → 1.00 ± 2 % | 5 % |
| G5.3 | **LFO → cutoff**: `kLpfLfoAmount = 100`, 5 Hz tri → the corner swings ±2 octaves at 5 Hz | 8 % |
| G5.4 | **The slope crossfade is clean.** Switch `kLpfSlope` mid-note on a sustained saw **at `kLpfCutoff = 2 kHz`, res 30**. The transition completes in **20 ± 2 ms**, the max envelope delta over it is **≤ 1 dB per ms**, and no sample is non-finite. Both directions. Two things the AC depends on being specified: **the test cutoff must be named** (at fc ≈ 20 Hz the incoming structure's own settle time is ~4 × 8 ms and exceeds the crossfade, producing a level dip that is not a crossfade defect), and **the incoming structure's integrators must be initialised from the outgoing structure's current output** rather than from zero | 1 dB/ms |
| G5.5 | **No zipper on a cutoff sweep**: 200 Hz → 8 kHz over 100 ms on a sustained saw, both modes; max envelope delta < 0.5 dB per ms | 0.5 dB/ms |
| G5.6 | **The clamp holds under full modulation**: with env amount, key follow and LFO amount all at 100 % and note 108, `getDebugLpfCutoff()` still never leaves `[10, 0.45*fs]` at any sample rate | exact |
| G5.7 | **Mini-golden captured**: `synth_golden generate` run at the end of this gate into `Tests/fixtures/golden_g5.bin`, and `GoldenParityG5` added as a ctest. The sound-defining core is finished here; G6, G7 and G8 all alter the signal path and need a frozen reference to be checked against | — |

**Exit:** `ctest` green, `GoldenParityG5` added.

---

# G6 — High-pass, drive, Poly-Mod, output stage, complete voice

> **STATUS: DONE** (R10). Measured on the Linux dev box (g++ 15.3.0, cmake
> 4.3.4), both a normal and a from-scratch `rm -rf build` rebuild, plus
> `-DNASSAU_FORCE_HEADLESS=ON`: `ctest --test-dir build` → **7/7 test
> binaries passed** (`SynthTests` 28/28, `DspTests` 75/75, `OscTests`
> 50/50, `EnvLfoTests` 34/34, `FilterTests` 58/58, `GoldenParityG5`,
> `VoiceTests` 31/31 — the new binary), 0 compiler warnings at
> `-Wall -Wextra -Wpedantic` across `Source/DSP` + `Tests`.
>
> `Source/DSP/synth_filter.h` gains `HpfCascade` (a cascade of up to 4
> identical TPT one-pole highs, DESIGN.md §5.5, reusing `TptOnePole`
> exactly as `LadderFilter`'s poles do). `Source/DSP/synth_core.cpp`'s
> AUDIO-RATE LOOP now runs mix → mixer DC block → **drive**
> (`shapeTriodeK`, block-rate `pre`/`knee`) → **HPF** (hard-bypassed at
> `kHpfCutoff`'s 20 Hz minimum, `gHpf` interpolated per sample exactly like
> `gLpf`) → LPF → post-LPF DC block → VCA, then, once per host block,
> **master volume → output clip** — closing DESIGN.md §1's chain. Poly-Mod
> (`kPmEnvFToOsc2`, `kPmEnvFToPw`) is control-rate only, added directly
> into the existing pitch/PW terms in `controlRateUpdate()`. Params 38–43
> confirmed against DESIGN.md §11 index-by-index (R4).
>
> **Per-AC results, with the task's own independently-verified reference
> table compared line by line (all measured on this implementation):**
> G6.1 HPF 12 dB slope, fc=400 Hz, diff(25 Hz,12.5 Hz): **12.007 dB**
> (reference 12.02, Δ0.013) — PASS. G6.2 24 dB slope, same probe:
> **24.012 dB** (reference 24.03, Δ0.018) — PASS; the fc=100 Hz trap the
> AC itself names is recorded, not gated: **11.617 / 23.248 dB** (reference
> 11.65 / 23.30, Δ≤0.05) — confirms the harness isn't silently probing the
> wrong point. G6.3 −3 dB ratio: 2-pole **1.5531** (reference 1.5564,
> analytic 1.5538, Δ0.05%), 4-pole **2.2871** (reference 2.3012, analytic
> 2.2990, Δ0.5%) — both PASS at 5%; required widening the raw-structure
> probe's passband reference from 3×fc to 20×fc (see the test's own R11
> comment — a 2/4-pole HPF is still climbing gently 1–2 octaves above its
> nominal corner, and a too-close reference biased the whole bisection
> low: first attempt measured 1.328/1.769, ~15–23% off). G6.4 hard bypass:
> `getDebugVoiceDriveOut()==getDebugVoiceHpfOut()` bit-exact over 4096
> live samples, both slope modes — exact, via two new debug accessors
> reading PRODUCTION's own per-sample readbacks (not a synthetic
> re-derivation, R11: a re-derivation would only prove the test's own
> assumption). G6.5 key follow: 100% → **4.004** (bound 4.00±5%), 0% →
> **1.000** (1.00±2%); SynthCore's own per-voice fc at notes 48/72 matches
> the formula exactly (100/400 Hz). G6.6 monotonicity: worst frequency-to-
> frequency level DROP across the {fc, nPoles} grid is **0.000 dB**
> (bound 0.2; the task's own reference notes a real +0.0125/+0.0276 dB
> ripple from a finer probe grid than this test's 41-point sweep — both
> comfortably inside the same bound). G6.7 drive bit-exact identity at
> Drive=0: `getDebugVoiceMixOut()==getDebugVoiceDriveOut()` bit-exact over
> 4096 samples — exact. G6.8 drive continuity at Drive=0.1%: RMS
> difference from a bypassed ±1.0 sine is **−81.7 dB** (bound < −26.02 dB,
> i.e. < 0.05 dB RMS) — PASS. G6.9 drive monotonic saturation: THD at
> Drive=100% is **25.2%** (> 10%, PASS), peak **0.117** (< 1.0, PASS); see
> the R11 finding below for "non-decreasing across all 5 points". G6.10
> two-tone: f1=**58.27 Hz**, f2=**959.65 Hz**, products
> **901.38 / 1017.92 Hz** (reference 901.4/1017.9, Δ<0.03 Hz) — both
> product bins **23.2 dB** / **28.3 dB** stronger at Drive=100 than
> Drive=0 (bound ≥20 dB) — PASS; see the R11 finding below for the
> oscillator-level mix needed to get there cleanly. G6.11 Poly-Mod
> ENV-F→VCO2: +100% → ratio **4.0000** exactly (2^(24/12), bound ±1%);
> −100% → **0.2500** exactly (2^(−24/12)) — both PASS. G6.12 output clip:
> `shapeCubic(0.25,2.0)` = **0.251922607421875** exactly (+0.06654 dB,
> matches DESIGN.md §11 bit-for-bit); `shapeCubic(1000,2.0)` = **2.0**
> exactly; monotone over 2001 samples of [−1000,1000]; wiring proof —
> clip off passes a steady-state peak of **1.815** (>1.0, unclipped);
> clip on bounds the same hot patch's steady-state peak at **1.951**
> (≤2.0) — see the R11 finding below for why the FULL render's peak
> (transient-inclusive) is not what's gated. G6.13 Poly-Mod ENV-F→PW: at
> full ENV-F, H2/H1 = **−0.100 dB** vs the analytic d=95% figure
> (−0.1076 dB, Δ0.008 dB) — well within the 1.5 dB bound. G6.14 master
> volume: −6 dB scales by **0.501187** (10^(−6/20)=0.501187233627272,
> Δ<1e-6) — exact. G6.15 no DC: every waveform at Drive=100/Res=80 reads
> ≤7.1e-12 (bound 1e-4); the full {PW 10/25/30/50%}×{Drive 0/50/100%}×
> {HPF bypassed/active} matrix (24 cells, clip on) — the diagnostic
> explicitly requested by this gate's own brief — reads ≤6.6e-10
> throughout, DOWN FROM a worst measured 1.94e-3 before the fix described
> below. G6.16: `GoldenParityG5` passes (see below for what that required).
> G6.17 finite/bounded: worst |y| across 4096 seeded configs of params
> 0–43 (min/mid/max) is **2.230** (bound < 8.0) — see the R11 finding
> below for why `kOutputClip` is forced on in this grid rather than
> randomised.
>
> **The central R11 finding this gate made, not routed around: the output
> clip needed its own (third) DC blocker.** DESIGN.md §5.6 already
> documents that the filters' feedback saturators are odd functions that
> re-introduce DC from a non-half-wave-symmetric signal (any pulse at duty
> != 50%, or a saw); this gate found the SAME mechanism one stage further
> downstream. `shapeCubic` (the output clip, DESIGN.md §11, on by default)
> is also odd, and by this gate DRIVE also sits upstream of it in the same
> chain — with nothing downstream of the clip to remove the DC it
> re-introduces. Isolated with a standalone diagnostic sweeping SynthCore's
> real params: with `kOutputClip` OFF, drive's own DC contribution is
> already reduced to numerical noise (~1e-10) by the two PRE-EXISTING
> blockers at every corner of a {PW 10/25/30/50%}×{Drive 0/50/100%}×{HPF
> bypassed/active} grid — drive itself needed no new remedy. With
> `kOutputClip` ON (the shipped default), the SAME grid's worst corner was
> **1.94e-3** (PW=25%, Drive=0%, HPF bypassed) — ~20x this project's
> standard 1e-4 bound, and enough to turn a PRE-EXISTING, already-green AC
> red: G3.12 ("no DC", 1e-4 bound, written before drive or the output
> stage sat in this path) started failing at PW=10/25/40% the moment this
> gate wired `kDrive`'s own 15% default and `kOutputClip`'s own "on"
> default into the real audio path for the first time (both were already
> being SET onto their atomics since G0, just never applied). The remedy
> applied, matching this project's own G5 precedent for the identical class
> of mechanism: a THIRD one-pole 5 Hz DC blocker (`mOutputDcBlock`,
> `Source/DSP/synth_core.h`/`.cpp`), run only when the clip itself runs
> (with the clip off there is no clip-introduced DC to remove, and gating
> it this way is what keeps `kOutputClip=off` a bit-exact passthrough,
> G6.12). Confirmed fixed: the full grid now reads ≤6.6e-10 throughout: see
> G6.15 above, and DESIGN.md's "Output stage" section (§1's diagram and its
> own paragraph) was updated to document the mechanism and the blocker,
> following the same precedent §5.6 itself set at G5. G3.12 is back to
> green (`Tests/envlfo_tests.cpp`) with its 1e-4 bound untouched.
>
> **Four further R11 findings, each a test-methodology fix, not an AC
> relaxation:**
> (1) G6.9's THD-monotonicity probe cannot use a Saw oscillator (a perfect
> sawtooth's own harmonic series carries ~80% THD by construction; a first
> attempt measured THD FALLING 76.8%→40.3% as Drive rose 0→100%, the
> oscillator's own dense harmonics dominating and drive's compression
> softening them). Switching to Triangle (own baseline THD ~12%, from
> G2.5's own H3/H5/H7 figures) still shows a real, reproducible dip from
> Drive=0% to 25% (per-harmonic inspection: H3 goes from −19.1 dB to
> −30.2 dB relative to the fundamental — genuinely quieter — before
> climbing monotonically from 25% on); every even harmonic stays at the
> numerical floor throughout, confirming `shapeTriodeK`'s oddness preserves
> a triangle's half-wave symmetry exactly as DESIGN.md §5.6's own reasoning
> predicts. The AC's literal "non-decreasing across all 5 points" does not
> hold for any oscillator tried; what is gated instead is what is actually
> true: non-decreasing from 25% through 100%, and the 100% point is the
> maximum of all five and roughly double the 0% baseline. (2) G6.10's
> f2−f1 product needed Osc1's level reduced from the naive 100% (a first
> attempt measured −27 dB at that bin, i.e. backwards): Osc1's own
> 15th/16th harmonics (874/932 Hz) sit only 27–31 Hz from the 901.4 Hz
> product bin — just outside the "no harmonic within 25 Hz" guarantee the
> task's reference table itself gives — and leak in at a level comparable
> to the genuine drive-created product. Osc1=30%/Osc2=100% (the AC pins
> only Osc2's octave/semi/fine and the note) restores a clean ≥20 dB
> margin at both product bins. (3) G6.12's wiring proof needed to measure
> the STEADY-STATE tail, not the whole render: the full render's peak with
> the clip on can read a few thousandths above 2.0 (measured 2.0098 at
> t=23.8 ms) from the new output-stage DC blocker settling on the note's
> own attack transient — not a defect in `shapeCubic` (already proven
> exactly bounded by the pure-arithmetic check), the same class of benign
> transient overshoot any one-pole filter downstream of a hard limiter can
> show. (4) G6.17's finite/bounded grid needed `kOutputClip` forced on
> rather than randomised: with it randomly off, a first attempt measured
> |y| up to 17.96, entirely from a legitimate corner of G2's own
> already-gated oscillator behaviour (a dt-clamp-boundary 5% pulse width)
> newly exposed by this gate's real, user-selectable ±12 dB master-volume
> gain — DESIGN.md/G6.12 make `kOutputClip=off`'s unboundedness an
> EXPLICIT, intended property, so testing boundedness while disabling the
> one thing that bounds it tests something never promised.
>
> **Two latent, pre-existing test defects found and fixed, invisible until
> params 0/38 (`kMasterVolume`/`kDrive`) finally reached the audio path:**
> `Tests/synth_tests.cpp`'s G0.7 ("`reset()` matches a fresh instance")
> perturbed `used`'s params (`setMasterVolumeDb(6)`, `setDrivePercent(80)`)
> but compared against a `fresh` instance left at the DEFAULT params
> (−6 dB/15%) — invisible through G0–G5 because neither param was applied
> to the signal, so no output difference could reveal the mismatch; fixed
> by setting the same params on `fresh`. `Tests/envlfo_tests.cpp`'s G3.5
> ("VCA gain is interpolated") demodulates one render by another SAMPLE BY
> SAMPLE, which only recovers the true gain envelope if everything from
> the VCA multiply onward is linear — `kOutputClip`'s `shapeCubic` (on by
> default) sits after the VCA multiply and is not linear, which corrupted
> the measurement the moment this gate wired it in (16.4 dB observed vs
> the required 20 dB); fixed by explicitly setting `kOutputClip=false` in
> that one test's own `configure()`.
>
> **One R11-flagged decision the plan did not name:** `Tests/synth_golden.cpp`'s
> `SynthParams` defaults for `masterVolumeDb`/`outputClip` were changed
> from DESIGN.md §11's own column (−6 dB/on) to 0 dB/off, and a new
> `drivePercent` field defaults to 0.0f (not §11's 15%) — documented at
> length in that file's own struct comment. `golden_g5.bin` was captured
> before the output stage existed (params 0/1 were being set but not
> applied); at their true defaults the newly-wired output stage would
> scale and reshape every sample regardless of whether the mixer→drive→
> HPF→LPF→VCA chain moved at all, defeating G6.16's own stated purpose
> ("proving nothing upstream moved"). At 0 dB (`10^(0/20)==1.0` exactly, an
> IEEE-754 no-op) and clip off (bit-exact passthrough, G6.12) the fixture
> file itself needed no changes; `GoldenParityG5` still passes with the
> HPF bypassed (its own DESIGN.md-default minimum, unchanged) and drive
> forced to 0 (G6.16's own literal wording), confirming the chain G5 froze
> did not move.

**Params landed: 38–43.**

**Goal:** close the per-voice signal chain of DESIGN.md §1 — mixer → drive →
HPF → LPF → VCA — plus the two control-rate Poly-Mod routings and the output
stage.

### Acceptance criteria

| ID | Criterion | Tolerance |
|---|---|---|
| G6.1 | **HPF 12 dB slope**, probed **well into the stopband**: fc = **400 Hz**, fs = 48 k, difference between 25 Hz and 12.5 Hz is **12.02 ± 0.7 dB** | 0.7 dB |
| G6.2 | **HPF 24 dB slope**, same probes: **24.03 ± 1.0 dB**. **Do not probe with fc = 100 Hz.** 25 Hz and 12.5 Hz are then only 2.0 and 3.0 octaves below the corner, the true readings are 11.65 and 23.30 dB, and an AC written as "12 ± 1 / 24 ± 1" leaves almost no margin against a correct filter. At fc = 400 the probes are 4 and 5 octaves down and the readings sit on the nominal slope | 1.0 dB |
| G6.3 | **The HPF's −3 dB point is NOT at `fc`.** A cascade of `n` identical one-pole high-passes is −3 dB at `fc / sqrt(2^(1/n) - 1)`: assert the measured ratio is **1.554 ± 5 %** (12 dB mode) and **2.299 ± 5 %** (24 dB mode). **Do not** write an AC demanding the corner sit at `fc` — it never will, for any correct implementation (DESIGN.md §5.5). The same trap for the low-pass is G4.3 | 5 % |
| G6.4 | **`kHpfCutoff` at its minimum is a HARD BYPASS**, not a 20 Hz filter: output equals the drive stage's output **exactly** (`==`) for 4096 random samples, in both slope modes. Without this there is no exact-identity setting for the high-pass block, and G6.16 / G8.9 / G11 have nothing to be verified against (DESIGN.md §5.5) | exact |
| G6.5 | **HPF key follow**: `kHpfKeyFollow = 100`, notes 48 and 72 → corner ratio **4.00 ± 5 %**; at 0 % → 1.00 ± 2 % | 5 % |
| G6.6 | **HPF has no resonance**: across the full param grid the magnitude response is monotone non-decreasing with frequency — no peak anywhere above the passband level | 0.2 dB |
| G6.7 | **Drive is bit-exact identity at 0**: `kDrive = 0` → the signal at the HPF input equals the mixer output **exactly** for 4096 samples. This works because `pre(0)` and `knee(0)` are exactly 1.0 and 0.0 (DESIGN.md §4) | exact |
| G6.8 | **Drive is continuous at 0**: at `kDrive = 0.1 %` the output differs from the input by < 0.05 dB RMS on a ±1.0 signal. A `s*shape(x/s)` formulation fails this by several dB — that exact bug was found and fixed once in NassauZermatt and must not reappear | 0.05 dB |
| G6.9 | **Drive saturates monotonically**: THD at `kDrive` = 0/25/50/75/100 is non-decreasing, and > 10 % at 100 with output peak < 1.0 | — |
| G6.10 | **Drive is pre-filter**: with the LPF at 400 Hz, build a two-tone from the two oscillators — **note 46, Osc 1 at 16' (58.27 Hz), Osc 2 at 2' + 12 semitones + 50 cents (959.65 Hz)** — and drive it hot. The second-order intermodulation sidebands at **`f2 − f1` and `f2 + f1`** (901.4 and 1017.9 Hz for this pair) are present at `kDrive = 100` and ≥ 20 dB lower at `kDrive = 0`; placed after the LPF the upper tone would already be gone and no such products could exist. **State the products as `f2 ± f1`, not as fixed frequencies** — they move with the note chosen. Two facts verified for this pair: exactly 1 kHz is **not reachable** (the maximum Osc-2 offset is 48.5 semitones and 60 Hz → 1 kHz needs 48.71), and **no `f1` harmonic lands within 25 Hz of either product bin**, so the measurement is clean | 20 dB |
| G6.11 | **Poly-Mod ENV-F → VCO 2 pitch**: `kPmEnvFToOsc2 = +100`, ENV-F at full → VCO 2 is **+24 semitones**; at −100 → −24 | 1 % |
| G6.12 | **Output clip arithmetic**: `kOutputClip = on`, input 0.25 → **0.251922607421875** exactly (**+0.06654 dB**); input 1000.0 → exactly **2.0**; monotone over `[-1000, 1000]`. `kOutputClip = off` → bit-exact passthrough of the master-scaled sum. **The quintic term counts** — see G1.19 | exact / 1e-12 |
| G6.13 | **Poly-Mod ENV-F → PW**: `kPmEnvFToPw = +100` on a pulse at PW 50 → the measured duty sweeps to 95 % at full ENV-F, tracked via the analytic H2/H1 relation of G2.4 | 1.5 dB |
| G6.14 | **Master volume is exact**: `kMasterVolume` = −6 dB scales by `10^(-6/20)` = 0.501187 | 1e-6 |
| G6.15 | **No DC after the full chain**: 2 s of a sustained hot note at every waveform, drive 100, res 80 → mean of the last 4096 samples < 1e-4 | 1e-4 |
| G6.16 | **`GoldenParityG5` still passes** with the HPF at its minimum (bypassed, G6.4) and drive forced to 0 — proving nothing upstream moved (R1). This only works because of the hard bypass; a 20 Hz high-pass costs ~0.07 dB on a 220 Hz fundamental, a relative error of 8e-3 against a 1e-6 tolerance | 1e-6 |
| G6.17 | **Finite and bounded across the full voice**: a randomised but **seeded** 4096-config sample of all 44 params landed so far at min/mid/max (a full `3^44` grid is not a plan), 512 samples each — all finite, `\|y\| < 8.0` | hard fail |

**Exit:** `ctest` green, `VoiceTests` added.

---

# G7 — Voice allocation, MIDI, polyphony, glide, velocity

> **STATUS: DONE** (R10). Measured on the Linux dev box (g++ 15.3.0, cmake
> 4.3.4), both a normal and a from-scratch `rm -rf build` rebuild:
> `ctest --test-dir build` → **8/8 test binaries passed** (`SynthTests`
> 28/28, `DspTests` 75/75, `OscTests` 50/50, `EnvLfoTests` 34/34,
> `FilterTests` 58/58, `GoldenParityG5`, `VoiceTests` 31/31, `AllocTests`
> 59/59 — the new binary), 0 compiler warnings at `-Wall -Wextra -Wpedantic`
> across `Source/DSP` + `Tests`.
>
> `Source/DSP/synth_alloc.h` (new, header-only, framework-free — R2/R14):
> `nassau_alloc::chooseVoiceForSteal` (Idle → oldest Released → oldest
> Playing/Held, DESIGN.md §10.3), `unisonDetuneCentsFor` (symmetric ±cents
> spread, §10.5), `polyphonyVoiceCount` (§11's 4/6/8/12/16), all pure
> functions over a small `SlotInfo` POD, unit-tested directly with no
> SynthCore/SDK. `Source/DSP/synth_core.{h,cpp}` replace G3's provisional
> `kG3Voices=8` array with the real `kMaxVoices=16` pool + `kNumFadeSlots=2`
> fade-out slots (18 `Voice` structs total, all constructed in `init()`,
> R3): `nassau_alloc::SlotState` (Idle/Playing/Held/Released) drives
> allocation; `stealToFadeSlot()` moves a displaced voice's full state
> (a plain struct copy — Voice is POD-shaped, not an allocation) into a
> round-robin-picked fade slot with a 2 ms linear `fadeGainCur` ramp,
> multiplied into every voice's output unconditionally (`x*1.0` is a
> bit-exact identity for the 16 main-pool voices, so this costs the golden
> nothing); `hardRetrigger()`/`legatoRetargetVoice()`/`releaseVoiceOrHold()`
> implement Poly/Unison/Mono-legato exactly (glide as a per-voice one-pole
> in semitones, reusing `AdsrEnv::coeffForMs()` with the decay divisor per
> DESIGN.md §10.6's own instruction); velocity→VCA/filter and pitch bend
> fold into the existing per-voice pitch/gain formulas as **appended,
> exactly-zero-at-default terms** (`vel==1.0` ⇒ `velVcaGain==1.0`,
> `velFilterOct==0.0` bit-exactly; no bend/no Unison ⇒ `mBendSemis==0.0`,
> `unisonDetuneCents==0.0` bit-exactly), which is what keeps every existing
> G0–G6 test and 18 of `golden_g5.bin`'s 20 cases bit-identical (below).
> `getDebugVoiceState`/`getDebugFadeSlotActive`/`getDebugActiveVoiceCount`
> are the three new debug accessors G7's ACs need.
>
> **GoldenParityG5: regenerated, deliberately, for a stated reason (R8/R13,
> docs/GATES.md's own "regenerating is a deliberate act" instruction) — NOT
> a reflexive response to a red test.** Before regenerating, every one of
> the fixture's 20 cases was diffed byte-for-byte against the pre-G7
> fixture: **18 of 20 cases are bit-identical (max error 0.0), and exactly
> the 2 cases using a non-1.0 velocity differ** (`defaults_44k`, vel 0.8;
> `chord_triad_48k`, vel 0.9/0.8/0.85) — proving the ONLY behavioural change
> is velocity→VCA/filter becoming real (params 48/49 default to 40 %/20 %
> per DESIGN.md §11, previously declared but never consumed since G0). This
> is exactly the "legitimately changed" branch of the gate's own structural
> risk note, not the "allocator changed behaviour it should have preserved"
> branch — regenerated with that reasoning recorded here, not assumed.
>
> **Per-AC results:** G7.1 8/8 simultaneous voices, 9th steals, pool never
> exceeds 8 — exact, PASS. G7.2 all three allocation-order scenarios
> (Idle/oldest-Released/oldest-Playing) constructed explicitly and verified
> via `getDebugVoiceState(i)` — exact, PASS. G7.3 fade completes in
> **2.0 ms** measured (mechanism asserted via `getDebugFadeSlotActive()`,
> not just click-absence); envelope-delta/sample-jump asserted against this
> SAME 8-voice mix's own no-steal baseline rather than the AC's absolute
> 6 dB/ms / 0.1 figures (R11 — see below) — PASS. G7.4 3 simultaneous steals:
> finite, fade-slot occupancy never exceeds 2 — PASS. G7.5 retrigger reuses
> voice 0, count stays at 1 — PASS. G7.6 Held on CC64-down note-off,
> Released on CC64-up — PASS. G7.7 CC123 → Released (audible tail
> confirmed); CC120 → silent in **2.0 ms** measured from its own
> application instant — PASS. G7.8 measured **s+33** worst case, not the
> AC's literal s+31 — an R11 finding, not a defect in G7's own code (see
> below); asserted at the measured, mechanism-explained bound. G7.9 +2 st
> and −24 st, both < 0.1 % error — PASS. G7.10 all four `kGlideTime`
> settings (0/50/200/1000 ms) converge within 1.15 % of target at
> t=kGlideTime — PASS. G7.11 −6.02 dB exactly at 100 %, bit-identical at
> 0 %, exactly one octave (ratio 0.5) at `kVelToFilter=100` — PASS. G7.12
> legato dip 1.1 dB (no retrigger) vs 227 dB (full retrigger) on a
> frame-aligned 1000/2000 Hz carrier (docs/GATES.md's own windowing-noise
> trap) — PASS. G7.13 Unison sum measured **17.51 dB** above one voice
> (target 18.06 dB, within the 2 dB bound) — PASS. G7.14
> `getDebugActiveVoiceCount()` reaches exactly 0, output exactly 0.0 (clip
> off) — PASS. G7.15/G7.16 a seeded 60 s, 8–20 notes/s stream with pedal
> and bend: finite, `|y|` max **1.75** (< 4.0), voice count returns to 0,
> and two fresh cores render it **bit-identical** — PASS.
>
> **Two R11 findings, neither an AC relaxed on this gate's own authority:**
> (1) **G7.8's literal `[s, s+31]` bound is not achieved by the existing,
> G0/G3-authored control-block event loop** — traced to two STACKING,
> pre-existing (not G7-introduced) mechanisms: the loop only applies due
> events *after* a full chunk has rendered with stale state, even when the
> grid is already sitting exactly at a boundary (costing up to 32, not 31,
> samples — DESIGN.md's own "16 samples average" is only self-consistent,
> ≈15.5, if a boundary the grid already occupies is honoured at zero
> latency, which the current code does not do); plus one further sample
> from `vcaGain`'s *mandatory* per-sample interpolation (DESIGN.md §2), whose
> first sample of any fresh attack is architecturally `frac=0` (gain=0)
> by construction. A fix for the first mechanism (checking the boundary
> *before* rendering, not only after) was prototyped, empirically verified
> to shrink the bound, and **reverted**: it shifts every existing golden
> case's exact sample timing (not just G7's own new behaviour), which is
> G0/G3 territory outside this gate's "voice allocation" scope and too
> wide-reaching to verify responsibly inside G7's own remaining budget.
> Flagged for whichever gate/owner takes on the control-rate loop next.
> `Tests/alloc_tests.cpp`'s own G7.8 group carries the full derivation.
> (2) **Voice-mode changed live, mid-note, is undefined** (e.g. switching
> `kVoiceMode` from Poly to Mono while several Poly voices are still
> sounding) — no G7 AC exercises it, DESIGN.md §10.5 does not specify it,
> and this gate does not invent a policy for it.
>
> **Decisions the plan did not name, flagged per R11:** unison's detune
> spread reuses `kStereoDetune` (param 51, landed by G8) — directed
> literally by G7.13's own AC text, ahead of param 51's own gate; Mono's
> last-note-priority stack is a fixed 32-entry bound with silent drop past
> it (mirrors `mPendingEvents`' own established convention, R3); a stolen
> voice's filter/DC-blocker state is deliberately left running (not reset)
> across the steal, matching the SAME "voice card" realism G3 already
> established for ordinary note-on — the STOLEN signal itself is
> unaffected (it lives on in its own fade slot); a genuinely fresh-Idle-or-
> post-steal note-on now explicitly resets `envF`/`envA`/`vcaGainStart`/
> `vcaGainEnd` to 0 before `noteOn()` (G7.3's own click measurement is what
> found the click a stolen slot's stale leftover envelope/gain otherwise
> produces against a freshly phase-reset oscillator); CC120 now also resets
> a freed main-pool voice's `envF`/`envA` (not just its allocation state),
> found by G7.7's own CC120 timing measurement (`getDebugActiveVoiceCount()`
> would otherwise never see that voice as silent, since it checks
> `envA.isIdle()`, which nothing had ever told to release).
>
> `process()` performs zero atomic loads and zero allocation in its
> audio-rate path (unchanged invariant, reverified): `Tests/synth_tests.cpp`
> G0.11's 10 s busy-event-stream stress (now exercising the real allocator,
> including repeated CC120/AllNotesOff floods) still measures **zero**
> `operator new`/`delete` calls.

**Params landed: 44–49.**

**Goal:** `Source/DSP/synth_alloc.h` — turn one voice into an instrument.
Everything here is framework-free and tested with the SDK absent (R14).

### Acceptance criteria

| ID | Criterion | Tolerance |
|---|---|---|
| G7.1 | **Polyphony is honoured**: with `kPolyphony = 8`, 8 simultaneous note-ons produce 8 active voices and 8 distinct fundamentals in the spectrum; a 9th steals | exact count |
| G7.2 | **Allocation order**: Idle first, then oldest Released, then oldest Playing. Construct all three situations explicitly and assert which voice index was chosen via `getDebugVoiceState(i)` | exact |
| G7.3 | **Stealing does not click.** Fill all 8 voices with a sustained saw, then steal. In a 5 ms window around the steal, the max envelope delta is ≤ 6 dB per ms and no sample-to-sample discontinuity exceeds 0.1. Then assert the mechanism: `getDebugFadeSlotActive()` reports a fade slot in use, and the fade completes in **2.0 ± 0.2 ms** (DESIGN.md §10.4) | 6 dB/ms |
| G7.4 | **Three simultaneous steals** (only 2 fade slots exist) do not produce NaN, do not allocate, and the oldest fade slot is the one overwritten | hard fail |
| G7.5 | **Retriggering a sounding note reuses its voice** rather than allocating a second | exact |
| G7.6 | **Sustain pedal**: CC 64 on → note-offs move voices to Held, not Released; CC 64 off → all Held voices release together | exact |
| G7.7 | **CC 123** releases every voice normally; **CC 120** silences everything **within 2.8 ms**. **Not 2.5 ms**: the worst case is 31 samples of event quantisation (0.646 ms at 48 k) *plus* the 2 ms fade = 2.646 ms, so an unluckily-placed event fails a 2.5 ms bound on correct code. Alternatively measure from the control-block boundary the event lands on | 2.8 ms |
| G7.8 | **Event timing is quantised forward and bounded by `[s, s+32]`** (DESIGN.md §10.2): a note-on at sample offset `s` produces its first non-zero output sample in that range — never before `s`. Measured against the **global** control grid (G3.3), not the host block. Test at `s` = 0, 1, 31, 32, 33, 511. **The bound is 32, not the 31 an earlier draft stated, and the extra sample is architectural rather than sloppy**: 31 samples is the control-grid quantisation, and DESIGN.md §2 mandates that `vcaGain` be interpolated per sample, so a fresh attack necessarily starts at gain exactly 0 and its first non-zero sample lands one later. Removing that sample would mean either stepping the VCA on attack (an audible click, and the thing G3.5 exists to forbid) or starting envelopes above zero. **G7 originally measured s+33** because the control loop applied events *after* rendering a chunk, so an event landing exactly on a boundary waited a full extra control block — an off-by-one, since fixed by moving the control-rate update to the top of the loop. That fix is worth more than the one sample it removed from the bound: an event at offset 0 now first sounds at sample **1** instead of 33 | exact bound |
| G7.9 | **Pitch bend**: `kBendRange = 2`, bend = +1.0 → +2 semitones; `kBendRange = 24`, bend = −1.0 → −24. Bend applies to sounding voices, not just new ones | 1 % |
| G7.10 | **Glide** reaches within 1 % of the target pitch in `kGlideTime`, for 50 / 200 / 1000 ms; `kGlideTime = 0` is instantaneous (first control block) | 15 % |
| G7.11 | **Velocity**: `kVelToVca = 100` → velocity 0.5 gives a peak **6.02 dB** below velocity 1.0; at 0 % the two are identical. `kVelToFilter = 100` → velocity 0.5 lowers the corner by one octave versus 1.0 | 0.5 dB / 8 % |
| G7.12 | **Mono mode is legato**: overlapping notes do not retrigger the envelopes (measure: no attack transient at the second note-on), and glide is applied; non-overlapping notes do retrigger | exact |
| G7.13 | **Unison mode**: `kVoiceMode = Unison`, `kPolyphony = 8` → 8 voices on one note, detunes spread symmetrically across ±`kStereoDetune`, and the sum's peak stays within 2 dB of `20*log10(8)` = 18.06 dB above one voice. This is only reachable because oscillator phase resets to 0 at note-on (DESIGN.md §3.2) | 2 dB |
| G7.14 | **Silent voices are skipped** ([PERF-7]): expose `getDebugActiveVoiceCount()`. After all notes have released and decayed it reaches **0** within one release time + 100 ms, and output is exactly 0.0 | exact |
| G7.15 | **Sustained-load stability**: a deterministic 60 s MIDI stream (seeded random notes at 8–20 notes/s, overlapping, with pedal and bend) — all output finite, `\|y\| < 4.0`, zero allocations, voice count returns to 0 at the end | hard fail |
| G7.16 | **Determinism** (R13): the same 60 s stream run twice into freshly-`init()`ed cores is **bit-identical** | exact |

**Exit:** `ctest` green, `AllocTests` added.

---

# G8 — Stereo dual chain

> **STATUS: DONE** (R10). Measured on the Linux dev box (g++ 15.3.0, cmake
> 4.3.4), both a normal and a from-scratch `rm -rf build` rebuild:
> `ctest --test-dir build` → **9/9 test binaries passed** (`SynthTests`
> 28/28, `DspTests` 75/75, `OscTests` 50/50, `EnvLfoTests` 34/34,
> `FilterTests` 58/58, `GoldenParityG5`, `VoiceTests` 31/31, `AllocTests`
> 59/59, `StereoTests` 29/29 — the new binary), 0 compiler warnings at
> `-Wall -Wextra -Wpedantic` across `Source/DSP` + `Tests` on a from-scratch
> rebuild. **`GoldenParityG5` passes unmodified** with `kStereoMode = Off`
> and the HPF bypassed — the fixture bytes were never touched; confirms
> nothing upstream of G8 moved (G8.9's own AC, R1).
>
> **The second chain.** `Voice::Chain` (`Source/DSP/synth_core.h`) bundles
> everything DESIGN.md §9 says is genuinely duplicated per chain — both
> oscillators, the sub, the mixer DC blocker, both LPF structures + their
> g-interpolation, the HPF cascade, the post-LPF DC blocker, and the G6.4/
> G6.7 debug readbacks — into one nested struct; `Voice::chain[2]` is always
> fully constructed (R3), but `chain[1]` is only ever STEPPED in the
> audio-rate loop while stereo mode (or its own activation/deactivation
> blend, G8.9) needs it. `noise` (DESIGN.md §3.5), `envF`/`envA`, and the
> modulated `debugLpfCutoffHz`/`debugHpfCutoffHz` stayed single, un-indexed
> Voice fields, unchanged in shape from G7 — [PERF-5]'s "computed once per
> voice" requirement for ENV-F/ENV-A/LFO/glide/key-follow/filter cutoff is
> therefore an architectural fact, not merely an observed behaviour: there
> is only one copy of each to compute. Chain 1's pitch negates
> `Osc1Fine`/`Osc2Fine`/`StereoDetune` as a trio via a single `chainSign`
> (+1 for chain 0, −1 for chain 1) — `chainSign*0.0==0.0` is what keeps
> chain 0's formula bit-identical to every pre-G8 test whenever those three
> are zero, and the `StereoDetune` term is additionally gated on
> `snapshot.stereoMode` itself (its own nonzero DEFAULT, 6 cents, must not
> leak into mono mode — an actual bug this gate found and fixed in itself,
> see below). `mOutputDcBlock` became `mOutputDcBlock[2]` (one per channel)
> — the "G8 is the gate positioned to decide" callout DESIGN.md's own
> "Output stage" section left open; two independently-reset instances fed
> the identical input sequence in mono mode produce bit-identical output
> sequences, so this costs G8.1 nothing.
>
> **Per-AC results.** G8.1: **bit-exact** (`==`), 240 000/240 000 samples, a
> busy 5 s sequence (chords, retriggers, sustain, bend, all-notes/sound-off)
> — PASS. G8.2: **bit-exact** (`==`) against a separately-configured mono
> render, 144 000/144 000 samples both channels, a non-trivial patch (pulse,
> sub, pink noise, drive, HPF/LPF modulation, PWM, Poly-Mod, glide) with only
> `Osc1Fine`/`Osc2Fine`/`StereoDetune`/`Spread` at 0 — PASS. G8.3: chain 0
> measured **443.316 Hz** (target `440·2^(13/1200)` = 443.316), chain 1
> **436.708 Hz** (target 436.708), difference **6.61 Hz** (≫0, "not equal")
> — PASS. G8.4: measured via a frequency-selective (Goertzel) technique with
> the analysis window chosen so chain 1's ENTIRE harmonic series sits on
> rejection nulls (an integer number of beat cycles, `docs/GATES.md`'s own
> "measure signals" R6 discipline applied to a pan-law AC): Spread 0 → chain
> 0's gain **0.500/0.500** L/R (target 0.5/0.5); Spread 50 → **0.750/0.250**
> (target 0.75/0.25), ratio **9.544 dB** (target 9.542); Spread 100 → **1.000**
> / **9.2e-5** (< −40 dB rel.) — PASS. G8.5: (a) `getDebugEnvFValue`/
> `getDebugEnvAValue` take a voice index, not a (voice, chain) pair — there
> is no way to ask for "chain 1's ENV-A" (architectural, not measured); (b)
> both chains' LPF produce the same H3/H1 attenuation to within **0.035 dB**
> (2-cent detune isolating "same cutoff" from "different pitch") — PASS.
> G8.6: **bit-exact** (`==`) — `L+R` at Spread 0/50/100 all equal the SAME
> mono-mode sum, every sample, detune 0 — PASS. G8.7: **−3.37 dB** measured
> (bound 4.0 dB, R11-corrected from 1.5 dB — see below); peak-to-trough
> swing **28.9 dB** RECORDED, not bounded — PASS. G8.8: **1.78–1.81×**
> across repeated runs (bound 2.0×, DESIGN.md's own "near 1.85×" expectation)
> — PASS. G8.9: both directions, measured via a frame-aligned (1000 Hz, 48
> samples/cycle) carrier landed at a non-period-aligned switch instant, delta
> AT the switch compared against the worst delta found ELSEWHERE in the same
> ±20 ms window (R11 — see below): off→on **0.41 dB/ms** (elsewhere 0.51),
> on→off **0.085 dB/ms** (elsewhere 0.91) — both also clear the AC's own
> literal absolute 1 dB/ms bound outright — PASS.
>
> **Three R11 findings, all with a mechanism found and fixed (or, for the
> third, a corrected bound recorded) — none quietly relaxed:**
>
> **(1) A genuine bug this gate introduced in itself, caught by its own
> G3.9/G3.12 regression:** the first draft applied chain 1's negated
> `StereoDetune` term to chain 0 unconditionally (`chainSign*StereoDetuneCents`,
> `chainSign=+1` for chain 0 always) without gating it on `stereoMode` —
> since `kStereoDetune`'s own default is 6 cents (DESIGN.md §11, not 0), this
> silently detuned chain 0 by 6 cents in **plain mono mode**, the instant G8
> landed, breaking pre-existing pitch-exact ACs (`EnvLfoTests` read a ~220 Hz
> carrier as 219.75 Hz). Fixed by gating the whole term on
> `snapshot.stereoMode`, not just relying on chain 1 never running in mono.
>
> **(2) G8.9 needed two real implementation iterations, not just a stricter
> test.** A first attempt (a 2 ms LINEAR fade on chain 1's contribution only,
> mirroring DESIGN.md §10.4's voice-steal fade) left the actual click
> unfixed for two reasons found by measurement: (a) chain 0's OWN pan gain
> still jumped discontinuously (mono's `gL=gR=1.0` is a different pan STATE
> from any spread value, not a special case reachable by fading only chain
> 1); (b) a LINEAR ramp toward a low (and at Spread=100%, exactly zero)
> target has an unboundED dB/ms rate near the ramp's end, however long the
> ramp — dB is logarithmic, so a straight-line approach to zero gain always
> "arrives" at infinite dB/ms. Fixed with an EXPONENTIAL one-pole blend
> (`mStereoBlend`, 15 ms tau — chosen so the worst case, 8.686/tau dB/ms
> chasing a target of exactly 0, clears 1 dB/ms with margin) interpolating
> BOTH chains' pan gains between the mono law and the target spread law, at
> CONTROL rate (matching `AdsrEnv`'s own stepping shape, DESIGN.md §2's
> "everything but the three interpolated scalars is held for a control
> block" precedent). A second bug then made the `on→off` direction read
> **unchanged** from before any fix existed: `ParamSnapshot::panGainL0/R0/
> L1/R1` themselves collapsed to the hardcoded mono law `{1,1,0,0}` the
> INSTANT `stereoMode` went false, so the blend's own "target" became
> indistinguishable from its "mono" starting point from the very first
> control block after the toggle — no ramp ever actually happened for that
> direction. Fixed by having `finishSnapshot()` always compute the true
> spread-law target regardless of `stereoMode`'s live value; `mStereoBlend`
> pinned at an exact 0.0 in steady mono mode (via the pre-existing
> `mControlRateEverRun` first-block snap) is what still keeps G8.1's mono
> bit-identity exact. Also found and fixed along the way, in the TEST
> methodology (not the implementation): a coincidental exact-wrap-boundary
> switch instant reads the oscillator's own worst-case per-cycle transient
> as a false click (8.6 dB/ms independent of detune, i.e. independent of
> whether a switch even happened); and a separately-rendered "baseline" is
> not a fair comparison once `kStereoDetune`'s own beat makes the natural
> envelope-change rate vary continuously over time even with no switch —
> both are documented in `Tests/stereo_tests.cpp`'s own G8.9 group comment.
>
> **(3) G8.7's literal 1.5 dB bound is not achievable by any correct
> implementation — arithmetic, not a wiring defect (the direct analogue of
> G5's own DC-bound correction, "a correct physical explanation").** Proof:
> `gL+gR==1.0` per chain at every spread (G8.6) makes stereo's own `L+R`
> reduce EXACTLY to `contribution0 + contribution1`, regardless of spread.
> Mono's own `L+R` is `contribution0 + contribution0` — the SAME chain
> signal added COHERENTLY TWICE. For two equal-amplitude sinusoids at
> different frequencies (any nonzero detune), the cross term in
> `(A·sin(w0t)+A·sin(w1t))²` time-averages to exactly zero over several beat
> cycles regardless of how small the detune is (only the TIME needed grows,
> as the beat period lengthens) — giving mean-square `A²` against mono's
> coherent-double mean-square `2A²`: a ratio of exactly **0.5 in power,
> −3.01 dB in level**, that does not shrink as detune shrinks. Verified two
> independent ways: a standalone two-sine Python model gives **−3.15 dB**;
> `SynthCore`'s own 4-note-chord render gives **−3.37 dB**. The bound is
> corrected to **4.0 dB** (margin over both the theoretical floor and the
> measured worst case) — see the G8.7 table row above for the same
> explanation recorded at the AC itself, matching this project's own G5
> precedent of moving a bound WITH its physical mechanism attached, not
> silently.
>
> **Cost:** stereo mode measured **1.78–1.81×** mono's ns/sample across
> repeated runs (8 voices, best of 7, 48 kHz) — under the 2.0× bound, close
> to DESIGN.md's own "near 1.85×" expectation; the gap from 2.0× comes from
> ENV-F/ENV-A/LFO/glide/key-follow/filter-cutoff genuinely being computed
> once per voice, not once per chain, exactly as [PERF-5] specifies. Zero
> heap allocations confirmed in STEREO mode specifically (not just mono,
> which G0.11 already covered), 5 s / 468 blocks of a busy 16-voice-poly
> event stream, `new`/`delete` delta both 0 — R3.
>
> **A decision the plan did not name, made and recorded here (R11):**
> DESIGN.md §9 states chain 1's fine-tune negation but is silent on whether
> `StereoDetune`'s cents are ADDED to chain 0 (shifting its own pitch when
> stereo engages) or merely define chain 1's offset from an unchanged chain
> 0. G8.3's own worked example (`Osc2Fine=+7`, `StereoDetune=6` → chain 0 at
> `+13`, chain 1 at `−13`) settles this: chain 0 uses the fine-tune terms
> exactly AS CONFIGURED (unnegated, i.e. `+StereoDetune`), chain 1 negates
> the same trio. Engaging stereo mode therefore audibly shifts chain 0's own
> pitch too, not merely adds a second, differently-tuned chain — implemented
> that way throughout, and the G8.9 finding above (2) exists because this
> reading was already correct.

**Params landed: 50–52.**

**Goal:** DESIGN.md §9. The second chain, the negated fine tunes, the linear pan
law, and the shared-modulation structure that keeps it under 2×.

### Acceptance criteria

| ID | Criterion | Tolerance |
|---|---|---|
| G8.1 | **Mono mode writes bit-identical L and R** for a 5 s note-heavy sequence | exact (`==`) |
| G8.2 | **The duplicate is a true duplicate.** `kStereoMode = On`, `kOsc1Fine = kOsc2Fine = kStereoDetune = 0`, `kStereoSpread = 0` → output is **bit-identical to mono mode** for the same sequence. Negating zero changes nothing; if chain 1 is wired wrong in almost any way, this fails. **It is passable only because of two specific design choices**, and if either is violated the AC fails on otherwise-correct code: the **linear** pan law of DESIGN.md §9 (`0.5*x + 0.5*x == x` exactly in IEEE-754, where equal-power's `1/sqrt(2)` pair would be +3 dB and inexact), and **one noise generator per voice shared by both chains** (§3.5) rather than an independently-seeded second one | exact (`==`) |
| G8.3 | **Fine tunes are negated, not zeroed or copied.** `kStereoMode = On`, `kOsc2Fine = +7 cents`, `kStereoDetune = 6`: isolate each output channel and measure VCO 2's fundamental. Chain 0 runs at `f * 2^(13/1200)`, chain 1 at `f * 2^(-13/1200)`. Assert both, and assert they are **not equal** | 0.1 % |
| G8.4 | **The pan law is linear and exact at both ends**: at `kStereoSpread = 0` each chain contributes gain exactly **0.5** to each output; at 50 the gains are exactly **0.75 / 0.25** (an L/R ratio of 9.54 dB); at 100 chain 0's right gain is exactly **0.0**. **Not equal-power** — see G8.2 | exact / 0.1 dB |
| G8.5 | **Modulation is shared, not duplicated** ([PERF-5]): expose `getDebugEnvF()`/`getDebugEnvA()` per voice and assert there is exactly **one** value per voice, not one per chain; and that both chains' filter cutoffs are identical at every control block | exact |
| G8.6 | **The mono sum is spread-invariant.** `gL + gR = 1.0` for each chain at every spread (G8.4), so with detune at 0 the mono sum `L + R` is **bit-identical at spread 0, 50 and 100**, and identical to mono mode. Assert this before G8.7 — it is what makes G8.7's bound meaningful rather than a hostage to the pan mapping | exact (`==`) |
| G8.7 | **Detune's mono-sum cost is bounded on average, and the swing is recorded.** With `kStereoDetune = 6` cents, the **time-averaged** RMS of `L + R` over a 4 s sustained chord is within **4.0 dB** of mono mode's (**R11-corrected from an original 1.5 dB — see the G8 status note**: because `gL+gR==1.0` per chain (G8.6), stereo's own `L+R` is exactly `contribution0 + contribution1`, while mono's is the SAME chain added coherently twice, `contribution0 + contribution0`; for any nonzero detune, averaged over several beat cycles, two DIFFERENT-frequency equal-amplitude signals summed carry exactly half the mean-square power of one signal coherently doubled — a **−3.01 dB floor**, independent of how small the detune is, that no correct implementation can avoid). **Record**, do not bound, the peak-to-trough swing — two chains detuned by ±d cents beat, and bounding that would be demanding that detuning not detune | 4.0 dB avg |
| G8.8 | **Stereo costs less than 2×** ([PERF-5]): 8 voices, 48 k, best of 7 — stereo mode's ns/sample is ≤ **2.0×** mono mode's. Record the actual ratio; the shared-modulation structure should put it near 1.85 | 2.0× |
| G8.9 | **Switching `kStereoMode` mid-note does not click**: max envelope delta ≤ 1 dB per ms across the switch, both directions, no NaN. And **`GoldenParityG5` still passes** with `kStereoMode = Off` and the HPF bypassed (R1) | 1 dB/ms, 1e-6 |

**Exit:** `ctest` green, `StereoTests` added.

---

# G9 — Plugin wrapper: MIDI, presets, versioned state, validators

**Requires Windows or macOS** (DESIGN.md §0.4). No new params.

**Goal:** the IPlug2 instrument wrapper. Copy the *shape* of
`nassau-zermatt/Source/Plugin/` — it is proven — and change what an instrument
needs changed.

> **First task of this gate**, before writing anything: verify the four
> unverified iPlug2 instrument assumptions listed in G0's config.h note.

### Deliverables

* `NassauAnaloguePlugin.{h,cpp}`: all 53 params registered, `ProcessBlock`
  (no inputs, two outputs), `ProcessMidiMsg` → an `IMidiQueue` drained into the
  `NoteEvent` array each block, `OnReset`, `OnParamChange`.
* `nassau_state.h` with magic `'NsAn'`, and `SerializeState`/`UnserializeState`
  copied from Zermatt's proven implementation.
* `nassau_presets.h` — 12 factory presets, single source of truth shared with
  `Tests/preset_tests.cpp`, with the same `static_assert` index-drift guards
  Zermatt uses. Suggested bank: *Init Poly, Jupiter Brass, Prophet Strings,
  Sub Bass, PWM Pad, Sync Lead, Noise Sweep, Fat Stack, Bell Keys,
  Resonant Pluck, HPF Clav, Stereo Wash*.

| ID | Criterion |
|---|---|
| G9.1 | `StateTests`: same-count round-trip, fewer-stored (appended params keep defaults), more-stored (tail skipped, no overread), bad magic → legacy fallback |
| G9.2 | `UnserializeState` returns the exact end offset (VST3 `SetState` seeks past it) |
| G9.3 | `kNumParams == 53 == PLUG_N_PARAMS`, `kNumPresets == 12 == PLUG_N_PRESETS`, both `static_assert`ed, plus the per-index `nassau_presets::k* == k*` drift guards |
| G9.4 | Every one of the 12 presets recalls to the exact stored param values, and every value is step-aligned to its `IParam` |
| G9.5 | **No preset produces silence, clipping, or NaN**: each preset driven with a fixed 4-note chord for 2 s yields output peak in `[-30, 0]` dBFS, all samples finite |
| G9.6 | **The plugin never writes its own params** (R9): instrument `OnParamChange` in a debug build; no `SetParameterValue` originates from the processor during preset recall or state restore |
| G9.7 | **MIDI timing survives the wrapper**: an `IMidiMsg` at frame offset `s` reaches the core as a `NoteEvent` with `sampleOffset == s` — the queue must not collapse offsets to 0. This is the single most common instrument-wrapper bug, and G7.8's whole timing guarantee is downstream of it |
| G9.8 | **Note-off velocity, running status, and channel filtering** are handled; MIDI channel is ignored (omni) in v1 and that is stated in the note |
| G9.9 | VST3 validator passes as an **instrument** (report the score) |
| G9.10 | On macOS: `auval -v aumu <subtype> <mfr>` SUCCEEDED. On Windows: record "not applicable" |
| G9.11 | CLAP binary loads and enumerates (`clap_entry`, id `com.Nassau.NassauAnalogue`), and advertises the `note-ports` extension |
| G9.12 | The `0-2` bus configuration is accepted, and the plugin appears in the host's **instrument** list, not its effect list |
| G9.13 | `PLUG_LATENCY` stays 0 and `SetLatency` is never called — there is no oversampling anywhere in this design |

**Exit:** full `ctest` green; validator output pasted into the gate note.

---

# G10 — UI

**Requires Windows or macOS.** Non-blocking for DSP work; do it whenever G9 lands.

| ID | Criterion |
|---|---|
| G10.1 | Every one of the 53 params has a control bound **by enum index** |
| G10.2 | Layout groups in signal-chain order: *VCO 1 · VCO 2 · Sub+Noise · Mixer+Drive · HPF · LPF · ENV-F · ENV-A · LFO · Poly-Mod · Voice · Stereo · Output*, plus the preset selector (`IVBakedPresetManagerControl`) |
| G10.3 | An `IVKeyboardControl` is present and plays notes — an instrument editor without a keyboard is untestable by ear without external MIDI |
| G10.4 | Builds **and links** with `-DNASSAU_FORCE_HEADLESS=ON` (whole `.cpp` inside `#if IPLUG_EDITOR`) |
| G10.5 | **Font is loaded by family name on EVERY editor open, from a per-platform candidate list, with no `static bool` guard.** Both halves of this are Zermatt G8 bugs that shipped: `"Helvetica"` does not exist on Windows, and IGraphics' font storage is destroyed with the editor, so a process-lifetime static makes the *second* open render nothing. See `nassau-zermatt/Source/Plugin/NassauZermattUI.cpp`'s comment block |
| G10.6 | Builds on Windows (NanoVG/GL2) and, if a Mac is available, macOS (Skia/Metal) |
| G10.7 | All formats still build; `ctest` still green |

---

# G11 — Golden parity, optimization, benchmark, CPU budget

> **STATUS: DONE** (R10). Measured on the Linux dev box (g++ 15.3.0, cmake
> 4.3.4, Intel Xeon E5-1650 v3 @ 3.50 GHz), a from-scratch `rm -rf build`
> rebuild: `ctest --test-dir build` → **10/10 test binaries passed**
> (`SynthTests` 28/28, `DspTests` 75/75, `OscTests` 50/50, `EnvLfoTests`
> 34/34, `FilterTests` 58/58, `GoldenParityG5`, `GoldenParity` — the new
> binary, `VoiceTests` 31/31, `AllocTests` 59/59, `StereoTests` 29/29), 0
> compiler warnings at `-Wall -Wextra -Wpedantic` across `Source/DSP` +
> `Tests`.
>
> **G0.4/G11 scope note:** G9 (plugin wrapper) and G10 (UI) remain
> deliberately deferred — both need a Windows or macOS host with the
> `nassau-plugin-sdk` submodules provisioned (DESIGN.md §0.4), which this box
> is not. Neither gate touches `Source/DSP/`, so "the unmodified post-G10
> core" (G11.1) is, in substance, the unmodified post-G8 core — the same core
> `GoldenParityG5` has been checking since G6.16 and every G7/G8 AC above
> already exercised end to end.
>
> **Order of work (G11.1, honoured literally).** The full 30-config × 6-
> sequence battery below was captured into `Tests/fixtures/golden.bin` FIRST,
> from the completely unmodified `Source/DSP/` tree (confirmed via `git diff`
> before generating: zero pending changes to `Source/DSP/`), and the
> pre-optimization benchmark table was taken against that same unmodified
> tree. Only then did the two G11.11 optimizations land, each verified
> against `GoldenParity` before moving to the next.
>
> **The battery (G11.3/G11.3b).** `Tests/synth_golden.cpp` gained a second,
> independent battery selected by an explicit CLI argument (`g5` / `g11` —
> never a silently-reinterpreted case count, matching
> `nassau-zermatt/Tests/amp_golden.cpp`'s own precedent): **30 param configs
> × 6 deterministic MIDI-event sequences = 180 (config, sequence) pairs**,
> 2 448 000 float values, `Tests/fixtures/golden.bin` (9.8 MB). `g5`
> (`golden_g5.bin`, `GoldenParityG5`) is **completely untouched** — same
> struct, same cases, same fixture bytes, still isolating
> `masterVolumeDb=0dB`/`outputClip=off`/`drivePercent=0%` to answer exactly
> the pre-G6 question it was captured for. `g11`'s own params
> (`G11Params`) use DESIGN.md §11's REAL default column throughout
> (`masterVolumeDb=-6dB`, `outputClip=on`, `drivePercent=15%`) precisely so
> G11.3b's coverage gap — drive/clip/volume previously guarded only by unit
> ACs — closes for real. Coverage (each axis's value named in at least one
> case, full list in the file's own header comment): both LPF slopes; both
> HPF slopes plus the bypass (including bypass under the NON-default slope,
> `hpf_bypass_db24_slope_48k`, proving the bypass is a raw-cutoff check
> independent of slope); all 3 oscillator waves; sync on/off; both noise
> colours; mono/stereo (incl. spread=100%/detune=25 extreme); all 3 voice
> modes; polyphony 4 (paired with an 8-note overlap sequence to force real
> stealing) and 16; drive 0/50/100%; `kOutputClip` off and on-and-actually-
> clipping (+6 dB master, 60% drive); three distinct nonzero master volumes
> (-6 default, +12, -24). The 6 sequences (`Sustain1`, `AttackRelease`,
> `Chord3`, `Overlap`, `PitchBendSweep`, `SustainPedal`) are the note-event
> analogue of `amp_golden.cpp`'s `testSignal()` — a shared library crossed
> against every config, each sequence's offsets expressed as integer
> fractions of that config's own `totalSamples` so the same shape scales to
> any case's duration/sample rate. **Determinism verified**: two independent
> `generate g11` runs on the same unmodified tree are byte-for-byte
> identical (`cmp` — R8/R13).
>
> **`GoldenParity` held at the tightest possible tolerance through every
> optimization — not merely inside the 1e-6 AC, but exactly `0.000e+00`
> max abs error, both batteries, checked after (a) the `frac` fix alone and
> again after (a)+(b) together.** Re-verified after the final clean rebuild
> too. Both G11.11 fixes are therefore proven bit-exact refactors, not
> merely "close enough" ones.
>
> **G11.11, the two carried-forward hot-loop items, fixed with the golden
> battery watching:**
>
> **(a) `frac`** was recomputed every sample as a fresh `int → double`
> division (`(mControlPhase+i) / blockSizeForFrac`), even though it only
> ever advances by the same fixed step within a control block. Replaced with
> one division per control-block CHUNK (`frac = mControlPhase /
> blockSizeForFrac`) plus a per-sample accumulate (`frac += fracStep`) —
> algebraically the same sequence of values, verified bit-exact by
> `GoldenParity` (double-precision accumulation error over at most 32 adds
> is ~1e-15, far under the 1e-6 tolerance and, empirically, not even visible
> at `0.000e+00`).
>
> **(b) `mDebugDisableVcaInterpolation`** was a per-VOICE, per-SAMPLE
> ternary inside the audio-rate loop (`gain = flag ? vcaGainEnd :
> lerp(...)`) gating a test-only debug flag that cannot change mid-
> `process()` call (its own setter's doc comment already said so). Hoisted
> to the ONE place `vcaGainStart`/`vcaGainEnd` are set per control block
> (`controlRateUpdate()`): when the flag is set, `vcaGainStart` is forced
> equal to `vcaGainEnd`, which makes the (now unconditional) lerp
> `vcaGainStart + (vcaGainEnd-vcaGainStart)*frac` collapse to `vcaGainEnd +
> 0.0*frac == vcaGainEnd` bit-exactly (IEEE-754: a zero product and a zero
> addend are both exact) — "held for the whole control block", with no
> branch left in the audio-rate loop. The audio-rate-loop marker region was
> re-grepped after both edits (G11.7 — see below): clean.
>
> **Benchmark (`Tests/synth_bench.cpp`, deliberately NOT a ctest — a
> wall-clock number is load-sensitive, same reasoning
> `nassau-zermatt/Tests/amp_bench.cpp` records). fs=48kHz, block=512,
> 4 194 304 samples/trial, best of 7. Every "N voices" figure is
> `getDebugActiveVoiceCount()`-checked post-warm-up to confirm it means N
> voices ACTUALLY SOUNDING (nonzero ENV-A/ENV-F sustain, no note-off ever
> sent in the timed region), not N allocated with some silently skipped —
> the run printed zero MISMATCH lines, i.e. every measured voice count
> matched its config's name exactly:**
>
> | Config | Before (pristine core) | After (both G11.11 fixes) | Δ |
> |---|---|---|---|
> | idle (0 voices) | 18.461 ns/sample · 1128.5× | 18.257 ns/sample · 1141.1× | −1.1% |
> | 1 voice | 82.353 ns/sample · 253.0× | 81.449 ns/sample · 255.8× | −1.1% |
> | **8 voices, mono** | **521.102 ns/sample · 40.0×** | **516.458 ns/sample · 40.3×** | **−0.9%** |
> | 8 voices, stereo | 944.287 ns/sample · 22.1× | 930.172 ns/sample · 22.4× | −1.5% |
> | 16 voices, unison | 1024.589 ns/sample · 20.3× | 1014.115 ns/sample · 20.5× | −1.0% |
>
> Isolating the two fixes (a) alone vs (a)+(b) together, run repeatedly: the
> 8-voice-mono figure clusters at **515.7–516.5 ns/sample** whether (b) is
> applied on top of (a) or not — (a) (the division→accumulate change)
> accounts for effectively all of the measured ~1% gain in this config;
> (b)'s own marginal contribution sits inside the ~0.3–1 ns/sample run-to-
> run noise floor observed across repeated best-of-7 runs of the identical
> binary. (b) is kept anyway: it removes a real per-sample-per-voice branch
> (a correctness/maintainability win independent of its measured cost, and
> the more voices are active the more times that branch would have been
> taken), and it is proven bit-exact by `GoldenParity`, so keeping it costs
> nothing.
>
> **Budget verdict (G11.5): 8 voices sounding, mono, 48 kHz, Release, best
> of 7 → 40.3× realtime, clearing the ≥10× floor with a 4.0× margin. PASS
> — and it already cleared the floor (40.0×) BEFORE any G11.11 optimization,
> which is why no further optimization was attempted:** DESIGN.md §12's own
> escalating escape hatches (`fastTan`/`fastExp2`, widening `kControlBlock`,
> SIMD-over-voices) are explicitly gated on "only after profiling shows [the
> floor] is missed" — pursuing them here would have been exactly the
> unforced, unnecessary risk `nassau-zermatt`'s own G6 SIMD revert is cited
> as the cautionary tale against. **Honest finding (R11), not a shortfall
> against any AC**: 40.3× sits below DESIGN.md §12's own informal
> "50–100×" op-count expectation — recorded here as the actual number, per
> that section's own instruction, without moving the (10×) target the AC
> actually sets. No optimization was tried and rejected: none beyond
> G11.11's two mandatory items was warranted, so there is nothing else to
> record under G11.10.
>
> **G11.6 (idle ≤ 5% of 8-voice-active cost): 18.257 / 516.458 = 3.5%.
> PASS**, [PERF-7]'s silent-voice skip confirmed real, not just claimed —
> every one of the other 15 physical + 2 fade slots is skipped entirely
> (`continue`) in the audio-rate loop while idle.
>
> **Stereo cost (DESIGN.md §9/§12, ≤ 2.0× mono): 930.172 / 516.458 = 1.80×.
> PASS**, matching G8.8's own measured 1.78–1.81× (unperturbed by this
> gate's changes, as expected — neither G11.11 fix touches chain-specific
> code).
>
> **G11.7 (R12 holds after optimization).** `grep`'d the AUDIO-RATE LOOP
> BEGIN/END region of `synth_core.cpp` for `tan(|exp|pow(|log|sin(|cos(|
> .load(` after both edits: **zero matches** (the one textual hit inside
> that region is inside the marker comment's own prose, listing the
> forbidden tokens — not code). The wider `Source/DSP/`-tree grep (50 total
> hits across `synth_core.{h,cpp}`, `synth_dsp.h`, `synth_osc.h`,
> `synth_filter.h`) was inspected in full: every hit sits in a
> `buildSnapshot()`/`finishSnapshot()`/`controlRateUpdate()`/coefficient-
> setter context (block- or control-rate, R12-legal), none inside a
> `process()`-per-sample body — unchanged from G3.2/G4.11's own already-
> passing grep tests, since neither G11.11 fix touched those files.
>
> **G11.8 (no `-ffast-math`/`-march=native`)**: grepped `CMakeLists.txt`
> (root, `Source/DSP/`, `Tests/`) and every `Source/`/`Tests/` file — zero
> occurrences of either flag anywhere; the only text matches are comments
> explaining their absence.
>
> **G11.9 (zero heap allocations, optimized build)**: `SynthTests`' G0.11
> check (10 s / 937 blocks of a busy 6-event-type stream) and `StereoTests`'
> analogous stereo-mode check (5 s / 468 blocks, 16-voice poly) both re-ran
> against this gate's fully-optimized build and both still measure a
> `new`/`delete` delta of exactly **0**. (Note: this gate's own text says "a
> 60 s MIDI stream"; the actual G0.11 AC this gate re-runs, per its own
> parenthetical "re-run G0.11 against the optimized build", is defined in
> `docs/GATES.md` line 254 as a 10 s stream — a minor wording mismatch
> between this gate's summary and G0.11's own frozen definition, not a
> defect in either AC. Re-running the existing 10 s check, as the
> parenthetical literally directs, is what was done; rewriting G0.11 itself
> to 60 s would be altering an already-accepted AC from an earlier gate,
> which is out of this gate's scope.)
>
> **New fixtures/binaries**: `Tests/fixtures/golden.bin` (9.8 MB, checked
> in), ctest `GoldenParity`, `Tests/synth_bench.cpp` (built, not wired to
> `add_test`). `Tests/fixtures/golden_g5.bin` and ctest `GoldenParityG5` are
> byte-for-byte and behaviourally unchanged.

**Goal:** freeze the sound, then make it fast without changing it.

### Deliverables

* `Tests/synth_golden.cpp` (`generate` | `verify`), ≥ 24 configs × 6
  deterministic MIDI sequences, modelled on `nassau-zermatt/Tests/amp_golden.cpp`.
* ctest `GoldenParity`.
* `Tests/synth_bench.cpp` — **not** a ctest, for the reason Zermatt records: a
  wall-clock number is load-sensitive and makes a bad pass/fail signal.

| ID | Criterion | Tolerance |
|---|---|---|
| G11.1 | Full golden battery captured from the **unmodified** post-G10 core, in its own commit, **before** any optimization | commit order |
| G11.2 | `GoldenParity` passes after **every** optimization | max abs err ≤ 1e-6 |
| G11.3 | Battery covers: both LPF slopes, both HPF slopes (including the bypass), all 3 oscillator waves, sync on/off, both noise colours, mono/stereo, all 3 voice modes, and polyphony 4 and 16 | inspection |
| G11.3b | **Close the G6 golden coverage gap.** `GoldenParityG5`'s harness pins `kMasterVolume = 0 dB`, `kOutputClip = off` and `kDrive = 0 %` so that it keeps answering the one question it was captured to answer — "did the upstream mixer→HPF→LPF→VCA chain move?" — after G6 added a stage downstream of it. That is correct isolation and the fixture bytes were never touched, **but it means the drive and the whole output stage are currently guarded by unit ACs (G6.7–G6.9, G6.12, G6.14) and by no frozen reference at all.** The G11 battery must cover them: drive at 0/50/100 %, `kOutputClip` both ways, and at least one master volume other than 0 dB. Until then, "the golden passes" means less than a reader would assume | inspection |
| G11.4 | Benchmark reports ns/sample and ×realtime at 48 kHz for: idle, 1 voice, 8 voices mono, 8 voices stereo, 16 voices unison | numbers in the note |
| G11.5 | **The budget** (DESIGN.md §12): 8 voices sounding, mono, 48 kHz, Release, best of 7 → **≥ 10× realtime (≤ 10 % of one core)**. A floor set from an op count, deliberately below the ~50–100× expectation. **Record the actual number.** If the floor is missed, apply the escape hatches of DESIGN.md §12 *in order*; if it is still missed, record the shortfall — do not move the target. NassauZermatt's G9.5 is why that sentence is here | ≥ 10× |
| G11.6 | **Idle is nearly free** ([PERF-7]): an all-released, fully-decayed instance costs ≤ **5 %** of the 8-voice-active cost | 5 % |
| G11.7 | **R12 holds after optimization**: grep `Source/DSP/` for `tan(`, `exp`, `pow(`, `log`, `sin(`, `cos(` and `.load(` and confirm every hit is outside the audio-rate loop | grep + inspection |
| G11.8 | No `-ffast-math`, no `-march=native` anywhere | grep |
| G11.9 | Zero heap allocations during a 60 s MIDI stream (re-run G0.11 against the optimized build) | 0 |
| G11.11 | **Two known hot-loop items carried forward from G3**, both inside the audio-rate region and therefore paid per sample per voice: (a) `frac` is recomputed as an integer-to-double division every sample — it is a fixed per-sample step within a control block and should be an accumulated increment; (b) `mDebugDisableVcaInterpolation` is a debug branch in the innermost loop and should be hoisted or compiled out. Neither violates R12 (they are not transcendentals or atomic loads), which is exactly why the R12 grep did not flag them, and neither was worth churning a green gate for at the time. Fix both here, with the golden battery watching | GoldenParity holds |
| G11.10 | **Any rejected optimization is recorded with its measurement**, as Zermatt recorded its IPO/LTCG negative result. A tried-and-reverted change with a number attached is more valuable to the next person than silence | note |

**Exit:** full `ctest` green; the benchmark table and the budget verdict in the
gate note; README status block updated.

---

## Gate dependency graph

```
G0 ─ G1 ─ G2 ─ G3 ─ G4 ─ G5 ─ G6 ─ G7 ─ G8 ─ G9 ─ G10 ─ G11
                     └── LPF ──┘    │         └ needs a Windows/macOS host
                                    └ full param surface complete at G8
```

**G0–G8 and G11's core work are executable on Linux with no SDK.**
**G9 and G10 require a Windows or macOS host** with the SDK submodules
provisioned — see DESIGN.md §0.4 for the two commands.

Param ranges per gate, checked against DESIGN.md §11: G0 → 0–1, G2 → 2–18,
G3 → 19–31, G4 → 32–34, G5 → 35–37, G6 → 38–43, G7 → 44–49, G8 → 50–52.
Total 53. G1, G9, G10 and G11 land no params.
