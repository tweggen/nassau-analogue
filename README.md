# NassauAnalogue

A **polyphonic 1980-vintage analogue synthesiser** — two PolyBLEP VCOs, a
sub-oscillator and noise into a mixer, a little soft saturation, a switchable
12/24 dB high-pass with key follow, a switchable 24/12 dB resonant low-pass with
its own ADSR, a VCA ADSR, one global LFO, and an optional stereo mode that
duplicates the whole per-voice signal flow into a second chain with negated fine
tunings.

Voicing target: a **Roland Jupiter-6 with a bit of Sequential Prophet-5**.

Built on the Nassau plugin stack — **IPlug2 + Steinberg VST3-SDK** (not JUCE) —
consuming the shared [`nassau-plugin-sdk`](../nassau-plugin-sdk), exactly as
[`nassau-zermatt`](../nassau-zermatt) does.

The DSP heart (`SynthCore`, in `Source/DSP/`) is a dependency-free C++17 class
kept strictly separate from a thin IPlug2 wrapper (`Source/Plugin/`). Unlike
Zermatt, **the whole instrument** — voice allocation, MIDI event handling, glide,
sustain-pedal state, stereo duplication — lives in the core, behind a
framework-free `NoteEvent` interface. It builds and is unit-tested standalone,
with no SDK present.

> **Status: G0–G8 and G11 done** (every gate this Linux box can execute
> without the plugin SDK, DESIGN.md §0.4) — full signal chain, voice
> allocation, MIDI, stereo, the golden battery, and the CPU budget are all
> proven by `ctest`. **G9 (plugin wrapper) and G10 (UI) remain deferred**,
> needing a Windows or macOS host with `nassau-plugin-sdk`'s submodules
> provisioned; neither touches `Source/DSP/`, so the DSP core itself is
> final. Twelve gates (G0–G11) are specified in
> [`docs/GATES.md`](docs/GATES.md), each with objectively measurable
> acceptance criteria; see that file for the per-gate status.
>
> The plan went through one independent review pass before any code was written.
> It found nine defects that a correct implementation would have failed —
> among them a low-pass −3 dB criterion that was out by a factor of 2.3, a
> state-variable filter that could not self-oscillate at any setting with the
> mapping originally specified, alias-floor targets that belonged to minBLEP
> rather than PolyBLEP, and an inherited test helper whose 40-harmonic cap
> would have made every alias measurement read the oscillator's own harmonics.
> All are fixed, and where an error is easy to re-introduce the correction is
> recorded inline as a **"Do not…"** note next to the criterion.

## Documents

| | |
|---|---|
| [`docs/DESIGN.md`](docs/DESIGN.md) | The specification — signal chain, every block's math, the control-rate architecture, the 53-parameter surface, and the eight places where performance was chosen over authenticity. |
| [`docs/GATES.md`](docs/GATES.md) | The implementation plan: twelve gates, each with objectively measurable acceptance criteria, plus the R1–R14 rules that bind every gate. |

## The one priority that outranks the others

**Performance over 100 % 80s authenticity.** DESIGN.md §0.1 lists the eight
concrete decisions that follow from it, each tagged **[PERF-n]** where it bites.
The headline consequences:

* a **32-sample control rate** for all modulation — and the corollary that
  transcendentals are then free, so there is no `fastTan` and no `fastExp2`;
* **PolyBLEP** antialiasing and **no oversampled region anywhere**, with the
  drive kept mild by construction so none is needed;
* a ZDF ladder solved in **one fixed-point step** rather than a Newton-iterated
  nonlinear ladder;
* **silent voices skipped entirely**, so idle cost is near zero.

The budget the plan holds itself to: **8 voices, mono, 48 kHz, ≥ 10× realtime
(≤ 10 % of one core)**, stereo mode ≤ 2.0× that.

## Performance (measured, G11)

Linux dev box (g++ 15.3.0, Release, Intel Xeon E5-1650 v3 @ 3.50 GHz),
`Tests/synth_bench.cpp`, fs = 48 kHz, block = 512, best of 7. Every "N voices"
row is checked against `getDebugActiveVoiceCount()` post-warm-up, so it means
N voices *actually sounding* (nonzero sustain, never note-off'd during the
timed region), not N allocated with some silently skipped by the
silent-voice-skip path ([PERF-7]):

| Config | ns/sample | ×realtime@48k |
|---|---|---|
| idle (0 voices) | 18.257 | 1141.1× |
| 1 voice | 81.449 | 255.8× |
| **8 voices, mono** | **516.458** | **40.3×** |
| 8 voices, stereo | 930.172 | 22.4× |
| 16 voices, unison | 1014.115 | 20.5× |

**Budget verdict: 8 voices mono clears the ≥10× floor at 40.3× realtime — a
4.0× margin** (and already cleared it, at 40.0×, before either G11.11
optimization below — see `docs/GATES.md`'s G11 status note for why no further
optimization, e.g. SIMD-over-voices, was attempted). Idle costs **3.5%** of
the 8-voice-active cost (bound ≤ 5 %, [PERF-7] confirmed real). Stereo costs
**1.80×** mono (bound ≤ 2.0×).

Two hot-loop items were found and fixed with the golden battery watching
(`Tests/fixtures/golden.bin`, `GoldenParity`, held at **exactly `0.000e+00`**
max abs error throughout, not merely inside the 1e-6 tolerance): `frac`'s
per-sample division replaced with a per-chunk division plus an accumulated
step, and a per-sample-per-voice debug-only branch
(`mDebugDisableVcaInterpolation`) hoisted out of the audio-rate loop entirely.
Combined effect: **≈1%** across every voice-loaded config — modest, because
the budget was never actually tight; see `docs/GATES.md`'s G11 status note
for the full before/after table and the per-optimization breakdown.

## Dependencies

NassauAnalogue consumes the shared **[nassau-plugin-sdk](../nassau-plugin-sdk)**
via the `NASSAU_SDK_DIR` CMake cache variable, which defaults to the sibling
checkout `../nassau-plugin-sdk`. The DSP core and tests build even without the
SDK; only the plugin targets require it.

## Platform support

| | DSP core + tests | Plugin formats | UI backend |
|---|---|---|---|
| macOS | ✅ | VST3 + AU + CLAP | Skia / Metal, else headless |
| Windows | ✅ | VST3 + CLAP | NanoVG / GL2 |
| Linux | ✅ | ❌ — skipped at configure time | — |

Gates G0–G8 and G11 are fully executable on Linux against the DSP core and its
tests alone. **G9 (wrapper/validators) and G10 (UI) require a Windows or macOS
host.**

## Build

One-time, after cloning, to provision the VST3/CLAP SDKs that iPlug2 needs but
doesn't vendor (skip if you only want the DSP core + tests):

```sh
cd ../nassau-plugin-sdk
git submodule update --init --recursive
cmake -P cmake/ProvisionDeps.cmake
```

macOS / Linux (single-config generator):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Windows (Visual Studio is multi-config — pass `--config`, not `CMAKE_BUILD_TYPE`):

```pwsh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Point at a non-default SDK location with `-DNASSAU_SDK_DIR=/path/to/nassau-plugin-sdk`,
and force a no-UI build with `-DNASSAU_FORCE_HEADLESS=ON`.

### If the build fails on `File can't be removed and still exist`

iPlug2 auto-installs the built plugin to the system plugin folder after every
build; if a DAW or plugin scanner holds the previous binary open, that copy fails
and takes the whole build — including the test targets — down with it. For a
development loop, configure with `-DIPLUG_DEPLOY_PLUGINS=OFF`.
