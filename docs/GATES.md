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
| G2.6 | **PolyBLEP is actually wired in**: at f0 = **997 Hz**, saw, `nonHarmonicEnergyDb` with `mBlepEnabled = false` is **≥ 12 dB worse** than with it true (reference measurement: naive −13.9 dB, PolyBLEP −30.4 dB, a 16.5 dB improvement). This comparative check is the load-bearing one — an absolute alias figure can be met by an accidentally band-limited oscillator that is also wrong. **Do not demand 20 dB**: the improvement a 2-point PolyBLEP actually delivers is 16.5 dB and the bound would fail on correct code | 12 dB |
| G2.7 | **Alias floor, absolute**, saw and pulse, fs = 48 k, non-harmonic energy below the fundamental by: **≥ 35 dB at f0 = 109 Hz**, **≥ 26 dB at 997 Hz**, **≥ 22 dB at 3989 Hz**. Reference measurements of the exact kernel in DESIGN.md §3.1, all harmonics subtracted: saw −39.6 / −30.4 / −26.3 dB, pulse −42.5 / −32.9 / −27.4 dB. **Do not write 60/50/35 dB here.** Those are minBLEP-grade numbers; 2-point PolyBLEP sits ~20 dB above them at low f0 and no correct implementation of DESIGN.md §3.1 can pass them. See also the two harness warnings above — with the inherited 40-harmonic cap, or a round probe frequency, this AC measures nothing at all. Record the measured values | as stated |
| G2.8 | **Sub-osc is phase-locked and exact**: VCO 1 at 440 Hz, sub at −1 → **220.00 Hz**, at −2 → **110.00 Hz**, measured over 10 s with **zero accumulated drift** (period ratio exactly 2.0 / 4.0 to 1e-9). This is what proves it is derived from VCO 1's phase accumulator rather than being a second oscillator (DESIGN.md §3.4) | 1e-9 |
| G2.9 | **Sub is antialiased for free**: at f0 = 3989 Hz (sub at 1994.5 Hz), the sub's non-harmonic energy is within 3 dB of a **50 % pulse** VCO 1 measured at 1994.5 Hz — same waveform, so a like-for-like comparison (reference: −30.0 dB). Naming the comparison wave matters: a saw reads ~2 dB different and the AC becomes a coin toss | 3 dB |
| G2.10 | **Hard sync locks the period**: VCO 2 at 1.5× VCO 1 with sync on → the output's measured fundamental equals **VCO 1's**, not VCO 2's | ± 0.1 Hz |
| G2.11 | **Sync's alias floor is much looser, and stated as such**: in sync mode at f0 = 219 Hz with VCO 2 at 3.7×, non-harmonic energy ≥ **12 dB** below the fundamental (reference measurement: −16.6 dB). **Do not** hold sync to G2.7's numbers, and do not use 25 dB either — that was this plan's own first guess and a correct implementation misses it by 8 dB. Two compounding reasons: DESIGN.md §3.3's approximate step scaling, *and* the fact that a sync waveform's nominal fundamental is itself weak (most energy sits near the slave frequency), which makes "below the fundamental" a harsh denominator. The load-bearing sync check is G2.10's period lock, not this one. Record the measured figure | 12 dB |
| G2.12 | **White noise is flat**: spectrum across 8 octave bands from 40 Hz to 10 kHz within ±1.5 dB of the mean | 1.5 dB |
| G2.13 | **Noise is deterministic** (R13): two `init()`ed instances, same voice index, produce bit-identical noise for 1e6 samples | exact |
| G2.14 | **Mixer sums linearly**: with three sources muted, each level knob reproduces its source scaled exactly; with all four at 100 %, output equals the exact sum | 1e-6 |
| G2.15 | **Finite everywhere**: grid-search wave × octave × PW × sync × sub-octave × noise-colour at min/mid/max, at all five sample rates, 512 samples each — all outputs finite, `\|y\| < 8.0` | hard fail |
| G2.16 | **No zipper**: sweep `kOsc1Fine` across its full range over 100 ms; max envelope delta < 0.5 dB per ms | 0.5 dB/ms |

**Exit:** `ctest` green, `OscTests` added.

---

# G3 — Envelopes, LFO, and the control-rate architecture

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
| G3.12 | **No DC**: after 2 s of a sustained note at every waveform, the mean of the last 4096 output samples is < 1e-4 | 1e-4 |
| G3.13 | **Finite at every extreme**: grid-search the 13 G3 params at min/mid/max, 512 samples each — all finite, `\|y\| < 8.0` | hard fail |

**Exit:** `ctest` green, `EnvLfoTests` added.

---

# G4 — The low-pass filter: structures, resonance, stability

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
| G6.10 | **Drive is pre-filter**: with the LPF at 400 Hz, build a **two-tone from the two oscillators** — Osc 1 at 16' and Osc 2 at 2' + 12 semitones gives roughly **60 Hz + 960 Hz** — and drive it hot. Intermodulation products around 900/1020 Hz are present at `kDrive = 100` and ≥ 20 dB lower at `kDrive = 0`; placed after the LPF, the upper tone would already be gone and no such products could exist. **There is no audio input to this plugin**, so the classic 60 Hz + 1 kHz pair must come from the oscillators — and exactly 1 kHz is out of reach (the maximum Osc-2 offset is 3 octaves + 12 semitones + 50 cents = 48.5 semitones, just short of the 48.7 needed). Use saw waves and pick product bins clear of both tones' own harmonics | 20 dB |
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
| G7.8 | **Event timing is quantised forward, and bounded** (DESIGN.md §10.2): a note-on at sample offset `s` produces its first non-zero output sample in `[s, s + 31]` — never before `s`. Measured against the **global** control grid (G3.3), not the host block. Test at `s` = 0, 1, 31, 32, 33, 511 | exact bound |
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
| G8.7 | **Detune's mono-sum cost is bounded on average, and the swing is recorded.** With `kStereoDetune = 6` cents, the **time-averaged** RMS of `L + R` over a 4 s sustained chord is within **1.5 dB** of mono mode's. **Record**, do not bound, the peak-to-trough swing — two chains detuned by ±d cents beat, and bounding that would be demanding that detuning not detune | 1.5 dB avg |
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
| G11.4 | Benchmark reports ns/sample and ×realtime at 48 kHz for: idle, 1 voice, 8 voices mono, 8 voices stereo, 16 voices unison | numbers in the note |
| G11.5 | **The budget** (DESIGN.md §12): 8 voices sounding, mono, 48 kHz, Release, best of 7 → **≥ 10× realtime (≤ 10 % of one core)**. A floor set from an op count, deliberately below the ~50–100× expectation. **Record the actual number.** If the floor is missed, apply the escape hatches of DESIGN.md §12 *in order*; if it is still missed, record the shortfall — do not move the target. NassauZermatt's G9.5 is why that sentence is here | ≥ 10× |
| G11.6 | **Idle is nearly free** ([PERF-7]): an all-released, fully-decayed instance costs ≤ **5 %** of the 8-voice-active cost | 5 % |
| G11.7 | **R12 holds after optimization**: grep `Source/DSP/` for `tan(`, `exp`, `pow(`, `log`, `sin(`, `cos(` and `.load(` and confirm every hit is outside the audio-rate loop | grep + inspection |
| G11.8 | No `-ffast-math`, no `-march=native` anywhere | grep |
| G11.9 | Zero heap allocations during a 60 s MIDI stream (re-run G0.11 against the optimized build) | 0 |
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
