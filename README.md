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

> **Status: G0 done** (scaffold, build, test harness, `SynthCore`'s complete
> 53-param API declared — silent, no DSP yet). Twelve gates (G0–G11) are
> specified in [`docs/GATES.md`](docs/GATES.md), each with objectively
> measurable acceptance criteria; see that file for the per-gate status.
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
