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

## Performance (measured, G11 + §12.3 SIMD-over-voices)

Linux dev box (g++ 15.3.0, Release, Intel Xeon E5-1650 v3 @ 3.50 GHz),
`Tests/synth_bench.cpp`, fs = 48 kHz, block = 512, best of 7, run **alone**
(never alongside `ctest`, which was measured to inflate every figure by
~40%). Every "N voices" row is checked against `getDebugActiveVoiceCount()`
post-warm-up, so it means N voices *actually sounding* (nonzero sustain,
never note-off'd during the timed region), not N allocated with some
silently skipped by the silent-voice-skip path ([PERF-7]):

| Config | before SIMD (ns/sample) | after SIMD (ns/sample) | speedup | ×realtime@48k (after) |
|---|---|---|---|---|
| idle | 17.95 | 12.77 | 1.41× | 1631× |
| 1 voice | 72.27 | 70.79 | 1.02× | 294× |
| **8 voices, mono** | **472.25** | **312.90** | **1.51×** | **66.6×** |
| 8 voices, stereo | 856.80 | 575.59 | 1.49× | 36.2× |
| 16 voices, unison | 932.00 | 612.91 | 1.52× | 34.0× |

**Budget verdict: 8 voices mono clears the ≥10× floor at 66.6× realtime — a
6.7× margin.** The budget was already met before any optimization (40.0×
realtime, `docs/GATES.md`'s G11 status note), so §12's escape hatches were
never *required* — §12.3 (SIMD-over-voices, `Source/DSP/synth_simd.h`) was
built anyway, by request, on the understanding that it is the most invasive
of the three and must not be attempted without the golden battery frozen and
watching. It was: **both golden batteries verify at exactly the same
value as before this work, bit for bit** — `golden.bin` (G11) at
`0.000e+00`, `golden_g5.bin` (G5) at `7.105e-15` (a pre-existing, documented
value, not introduced by this change — see `docs/DESIGN.md` §12.3). Per-voice
marginal cost dropped **56.79 ns → 37.52 ns (1.51×, 34% less)** — well past
this section's own pre-implementation estimate of ~10-12%; §12.3 explains
why (a genuine division-count reduction in the HPF cascade, plus better
instruction-level parallelism than the five separate scalar calls it
replaces — not just narrower SIMD lanes).

Idle costs **4.1%** of the 8-voice-active cost (bound ≤ 5 %, [PERF-7]
confirmed real). Stereo costs **1.84×** mono (bound ≤ 2.0×).

SSE2, 2-wide `double`, x86-64's baseline ISA (no `-march` flag, R7-legal).
`-DNASSAU_NO_SIMD=ON` is the portable fallback R7 requires — see
`docs/DESIGN.md` §12.3 for the mechanism, the bit-exactness argument, and a
note for whoever adds a NEON backend next (none exists yet: no ARM hardware
was available to build or verify one here).

Two earlier hot-loop items (pre-SIMD, `docs/GATES.md`'s G11 status note) were
found and fixed with the golden battery watching: `frac`'s per-sample
division replaced with a per-chunk division plus an accumulated step, and a
per-sample-per-voice debug-only branch (`mDebugDisableVcaInterpolation`)
hoisted out of the audio-rate loop entirely. Combined effect: **≈1%** across
every voice-loaded config.

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

### Opt-in single-precision DSP path (`-DNASSAU_DSP_FLOAT=ON`), weak/ARM machines

**This deviates from this project's stated double-precision non-negotiable**
("coefficient math and filter state will use double; public I/O stays
float") for filter state, filter coefficients and the DC blockers only —
oscillator phase accumulators stay `double` unconditionally in both builds
(a G2.8 acceptance criterion needs 1e-9 precision; `float` only carries
~1e-7). It is **off by default**; the default build is untouched and stays
bit-identical to every gate through G11. See `docs/DESIGN.md` §12.2 for the
full mechanism, correctness results and measured numbers. Build it in a
**separate** build directory:

```sh
cmake -S . -B build-float -DCMAKE_BUILD_TYPE=Release -DNASSAU_DSP_FLOAT=ON
cmake --build build-float -j
ctest --test-dir build-float --output-on-failure
```

Every stability/tuning/DC acceptance criterion still holds in the float
build (self-oscillation stays bounded at ≈0.59/0.50 for the ladder/SVF, as
in the default build). The golden battery cannot and does not hold at the
shared 1e-6 tolerance once the audio-rate arithmetic itself runs in `float`
(the fixtures are, and stay, double-precision references — a separate,
still-tight tolerance applies only in this build); the measured
float-vs-double difference is **-76.6 to -78.6 dBFS relative to full
scale** — small, but past this project's own "-80 dBFS is inaudible" line,
so treat it as a real, if minor, timbral difference from the reference
rendering, not a free option.

**Measured on this project's x86-64 dev box (Xeon E5-1650 v3), the flag is a
6–7 % SLOWDOWN, not a speedup** — x86-64 doubles are not half-rate relative
to floats the way many ARM cores' are, so there is no throughput to win here,
while every filter call now crosses a `double`↔`float` boundary the default
build never pays. **The motivating ARM benefit (many ARM cores: half-rate
double throughput, 4-wide NEON for `float32`) is reasoned, not measured** —
no ARM hardware was available for this work. Re-run
`Tests/synth_bench.cpp` on the target hardware (alone, never alongside
`ctest`) before relying on this flag anywhere.

### SIMD-over-voices (`Source/DSP/synth_simd.h`, on by default) and its fallback

The default build pairs active voices' filter processing two at a time via
SSE2 2-wide `double` intrinsics (`docs/DESIGN.md` §12.3) — bit-identical to
the plain scalar path it replaces (both golden batteries verify at exactly
the same value, unchanged by this feature). To force the portable
scalar-pair fallback (no intrinsics, no arch header — the R7-required escape
hatch, and what any non-x86 target without an intrinsic backend uses today):

```sh
cmake -S . -B build-nosimd -DCMAKE_BUILD_TYPE=Release -DNASSAU_NO_SIMD=ON
cmake --build build-nosimd -j
ctest --test-dir build-nosimd --output-on-failure
```

There is currently no NEON backend (no ARM hardware was available to build
or verify one) — an ARM/Apple Silicon host falls back to the same portable
scalar path automatically. See `docs/DESIGN.md` §12.3 for what a future NEON
branch needs to implement.

### If the build fails on `File can't be removed and still exist`

iPlug2 auto-installs the built plugin to the system plugin folder after every
build; if a DAW or plugin scanner holds the previous binary open, that copy fails
and takes the whole build — including the test targets — down with it. For a
development loop, configure with `-DIPLUG_DEPLOY_PLUGINS=OFF`.
