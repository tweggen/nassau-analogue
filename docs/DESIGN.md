# NassauAnalogue — Design

A **polyphonic 1980-vintage analogue synthesiser**: two VCOs, a sub-oscillator
and a noise source into a mixer, a little saturation, a switchable 12/24 dB
high-pass with key follow, a switchable 24/12 dB low-pass with resonance and its
own ADSR, and an amplitude ADSR. One global LFO. An optional **stereo mode**
that duplicates the whole per-voice signal flow into a second chain whose fine
tunings are negated.

Built on the Nassau plugin stack — **IPlug2 + Steinberg VST3-SDK** (not JUCE) —
consuming the shared [`nassau-plugin-sdk`](../../nassau-plugin-sdk), exactly as
[`nassau-zermatt`](../../nassau-zermatt) does.

This document is the **specification**. [`GATES.md`](GATES.md) is the schedule.
Where the two disagree, this file wins and you fix that one.

---

## 0. Provenance, reference targets, and the one priority that outranks them

### 0.1 The stated priority

> **Performance is the priority goal, as opposed to 100 % 80s authenticity.**

This is not a footnote; it is the tie-breaker that decides at least eight
concrete design questions in this document. Every place it bites is marked
**[PERF]** and states what was given up. The list, up front, so nobody has to
hunt for it:

| # | Decision | Authenticity cost |
|---|---|---|
| PERF-1 | 32-sample control rate for all modulation | note timing quantised to ≤ 0.67 ms @ 48 k |
| PERF-2 | PolyBLEP antialiasing, **no oversampling anywhere** | alias floor ≈ −60 dB, not ≈ −100 dB |
| PERF-3 | ZDF ladder with **one** fixed-point step, saturator outside the implicit loop | not a true per-stage nonlinear ladder |
| PERF-4 | One **global** LFO, not one per voice | no per-voice LFO phase scatter |
| PERF-5 | Envelopes and LFO **shared** between the two stereo chains | the two chains cannot drift |
| PERF-6 | Audio-rate cross-modulation (Prophet Poly-Mod osc→osc) **excluded** | no metallic FM timbres |
| PERF-7 | Silent voices skipped entirely | none — pure win |
| PERF-8 | 12 dB LPF mode is a **different structure** (SVF), not a tap | 20 ms crossfade on the slope switch |

### 0.2 Reference targets

The voicing target is a **Roland Jupiter-6 with a bit of Sequential Prophet-5**.
What that concretely means here:

* **From the Jupiter-6**: a switchable **−24 / −12 dB** low-pass that resonates
  in both positions; a separate **high-pass** in front of it; two VCOs with
  independent waveform, range and PWM; a sub-oscillator; two ADSRs (one filter,
  one amplifier); one global LFO with a delay; VCO-2 detune and hard sync.
* **From the Prophet-5**: the *character* of the filter envelope routing — a
  bipolar filter-envelope amount and a "Poly-Mod"-flavoured **ENV-F → VCO-2
  pitch** and **ENV-F → pulse width** pair; and keyboard tracking on the filter
  as a continuous amount rather than an off/half/full switch.

### 0.2b One interpretation, settled before the plan was written

The commissioning request said "ADSR for both OSCs". That admits two readings:
one amplitude envelope serving the oscillator section, or an independent level
envelope per oscillator. **It was asked and answered: two envelopes total —
ENV-F to the filter, ENV-A to the VCA**, which is the Jupiter-6 (ENV-1/ENV-2)
and Prophet-5 layout and the cheaper of the two. It is recorded here because it
is a voice-architecture decision, not a knob, and re-deciding it after G3 would
invalidate every gate from there on.

### 0.3 Provenance tags (R5)

Every numeric constant in `Source/DSP/` carries one of:

* `// [ref]` — taken from the published specification or measured behaviour of
  a reference instrument (Jupiter-6, Prophet-5).
* `// [dsp]` — a standard DSP derivation (RBJ cookbook, Zavalishin TPT/ZDF,
  PolyBLEP).
* `// [voicing]` — a tuning choice made here, with no external authority.

**There is no source transcript for this plugin.** Unlike NassauZermatt there is
no PDF being followed; nothing may be tagged `[ref]` unless it is a published,
checkable number (e.g. "8' = A4 at 440 Hz"). A guess is `[voicing]`. Being
honest about which is which is what makes the voicing reviewable later.

### 0.4 Development-environment reality

Two facts that shape the gate order and must not be discovered late:

1. **`nassau-plugin-sdk`'s submodules are not checked out** in the current
   working copy (`external/iplug2` and `external/vst3sdk` are both empty).
   Before any plugin target can build: `git submodule update --init --recursive`
   then `cmake -P cmake/ProvisionDeps.cmake`, both inside the SDK checkout.
2. **The primary development box is Linux**, and `NassauPlugin.cmake` hard
   `FATAL_ERROR`s on anything that is not macOS or Windows. The root
   `CMakeLists.txt` therefore guards the plugin subdirectory exactly as
   Zermatt's does, and **G0–G8 and G11 are fully executable on Linux against
   the DSP core and its tests alone**. G9 (wrapper/validators) and G10 (UI)
   require a Windows or macOS host. This is why the entire instrument — voice allocation
   and MIDI handling included — lives in `Source/DSP/` (see §11, R14).

---

## 1. Signal chain

Per voice, per chain. `chain` is `0` in mono mode; `0` and `1` in stereo mode
(§9). Everything below runs once per chain, except the boxes marked **shared**,
which are computed once per voice and used by both chains (PERF-5).

```
 MIDI ──> VoiceAllocator ──> Voice[0..N-1]  (+2 fade-out slots, §10.3)
                                  │
     ┌────────────────────────────┴────────────────────────────────┐
     │  shared per voice:  ENV-F, ENV-A, glide, key-follow, vel     │
     │  shared globally :  LFO                                      │
     └────────────────────────────┬────────────────────────────────┘
                                  │
   per chain c ∈ {0} | {0,1}:     ▼
     1. Pitch      note + bend + glide + LFO*vib + (ENV-F → VCO2)
                   fine tunings NEGATED when c == 1        (§9)
     2. VCO 1      PolyBLEP  saw | pulse(PWM) | tri      × Osc1Level
     3. VCO 2      PolyBLEP  saw | pulse(PWM) | tri      × Osc2Level
                   optional hard sync to VCO 1
     4. SUB        square at VCO1/2 or VCO1/4, from VCO1's phase × SubLevel
     5. NOISE      white | pink                          × NoiseLevel
     6. MIX        sum of 2..5
     6b. DC BLOCK  1-pole HP @ 5 Hz — a pulse carries DC of (2d-1)
     7. DRIVE      shapeTriodeK(x, k(Drive))    — exact identity at Drive = 0
     8. HPF        TPT 1-pole HP cascade, 2 or 4 poles, cutoff × key follow
     9. LPF        24 dB: ZDF ladder │ 12 dB: TPT SVF        (§5)
                   cutoff = base × keyfollow × 2^(ENV-F*amt) × 2^(LFO*amt)
     9b. DC BLOCK  1-pole HP @ 5 Hz — odd saturators re-make DC (§5.6)
    10. VCA        ENV-A × velocity
    11. PAN        c == 0 → left by Spread, c == 1 → right by Spread
                                  │
                                  ▼
          voice accumulator (L, R) ──> Master volume ──> Output clip ──>
          Output DC block (§11, when clip is on) ────────────────> out
```

### Ordering constraints (non-negotiable)

1. **The drive sits between the mixer and the filters**, never after them. It is
   the mixer's summed level that pushes it, exactly as in a real instrument
   where the VCA-input stage is what runs out of headroom first.
2. **The HPF precedes the LPF.** Jupiter-6 order. It also means the HPF removes
   the sub and the low saw energy *before* the resonant LPF sees it, which is
   what stops the resonance from sounding muddy.
3. **The VCA is last in the voice**, after both filters, so the filter's
   resonant tail is enveloped rather than the other way round.
4. **The output clip is post-summation and post-master**, so it limits the
   stack, not the individual voice.

---

## 2. The control-rate architecture  **[PERF-1]** — read this before anything else

This is the single largest performance decision in the plugin, and every other
block's cost is quoted relative to it.

`SynthCore::process()` splits each host block into **control blocks of
`kControlBlock = 32` samples**. Per control block, per voice:

* the ADSRs advance one step,
* the LFO advances one step (globally, once — not per voice),
* pitch, filter cutoff, VCA gain and mixer levels are recomputed.

Inside a control block only the oscillator phase accumulators, the filter state
recursions and three interpolated scalars run at audio rate.

**Exactly three quantities are linearly interpolated across the control block**,
because a 1.5 kHz staircase in them is audible:

| Interpolated | Why |
|---|---|
| `gLpf` (LPF TPT coefficient) | a 2 ms filter attack sweeps ~0.9 octave per control block; held, that is a clearly audible staircase |
| `gHpf` | same, when key follow is high and glide is fast |
| `vcaGain` | held, this is textbook zipper noise |

**Everything else is held** for the duration of the control block — resonance,
drive knee, pulse width, oscillator phase increment, mixer levels, pan. Each was
checked: the fastest thing that modulates them is the LFO at 30 Hz, which at a
1.5 kHz update rate gets 50 steps per cycle. That is inaudible.

**The control grid is global and persists across `process()` calls.** A
`mControlPhase` member counts how far into the current control block the core
is; a host block that ends mid-control-block leaves the remainder to be
completed by the *next* call. The grid is therefore anchored to the stream, not
to the host's block boundaries.

This is not a detail — it is the difference between an implementation that
passes G3.3 and one that cannot. If the grid restarted at every host block, a
host delivering 1-sample blocks would update modulation every sample and a host
delivering 33-sample blocks would produce an uneven grid, so the same MIDI
sequence would render differently under different buffer sizes. Hosts change
buffer size at will; that would be a real bug in a real DAW.

`mControlBlock` is a **runtime member** (default 32), not a compile-time
constant, purely so `Tests/` can set it to 1 and measure the artifact the
choice of 32 introduces (G3.9). The cost of that is one `std::min` per control
block, not per sample.

### 2.1 The corollary nobody expects: transcendentals are free

`tan()` and `exp2()` are the two expensive calls in this design. At the control
rate their total cost is:

```
8 voices x 2 oscillators x (48000/32) control blocks/s  = 24 000 exp2 calls/s
8 voices x 2 filters     x (48000/32) control blocks/s  = 24 000 tan  calls/s
```

Fold in the cutoff-modulation and HPF key-follow exponentials (§5.4 — write them
as one `exp2` of a summed octave offset per filter, not one per term) and the
real figure is roughly **48 000 transcendental calls per second in total**.

At ~20 ns each that is **≈ 1 ms of CPU per second, or 0.2 % of one core.**

Therefore: **`fastTan` and `fastExp2` are explicitly out of scope.** Use
`std::tan` and `std::exp2`. Writing approximations here would be optimising a
0.1 % line item, and every approximation is a place for a tuning bug to hide.

This is recorded rather than silently omitted because "a synth needs a fast
tan approximation" is received wisdom that is simply false once the coefficient
update is off the audio path. If G11's benchmark ever misses its floor, the
escape hatch is documented in §12 — but the measurement must come first.

### 2.2 The per-block parameter snapshot

Once per *host block* (not per control block), `process()` builds one
`ParamSnapshot` for the whole instrument: every atomic is loaded exactly once,
and every parameter-derived constant that does not depend on the voice — ADSR
one-pole coefficients, LFO increment, drive knee, mixer gains, resonance `k`,
base cutoffs — is computed there. Voices then read a `const ParamSnapshot&`.

This is what keeps per-voice work down to what genuinely varies per voice:
pitch, key follow, envelope state, filter state. It is also R3's mechanism: the
audio path performs zero atomic loads.

---

## 3. Oscillators  (`synth_osc.h`)

### 3.1 Antialiasing: PolyBLEP, no oversampling **[PERF-2]**

Every discontinuous waveform is generated naively and corrected with a
**2-sample polynomial BLEP** at each discontinuity:

```cpp
// [dsp] Valimaki/Leary polyBLEP. t is the fractional distance, in samples,
// from the discontinuity; dt is the phase increment (cycles/sample).
inline double polyBlep(double t, double dt) {
    if (t < dt)        { t /= dt;      return t + t - t*t - 1.0; }
    if (t > 1.0 - dt)  { t = (t - 1.0)/dt; return t*t + t + t + 1.0; }
    return 0.0;
}
```

* **Saw**: `y = 2*phase - 1`, minus one BLEP at the wrap.
* **Pulse**: `y = phase < pw ? 1 : -1`, plus a BLEP at the wrap and a
  sign-flipped BLEP at the `pw` crossing.
* **Triangle**: **not** BLAMP-corrected. Generated as a **leaky-integrated
  BLEP square**, normalised by `4*dt`. This is cheaper (it reuses the pulse's
  two BLEPs and adds one multiply-accumulate) and is self-antialiasing, because
  integration is a first-order low-pass on whatever residual alias the square
  had. The leak coefficient is `0.999` at 48 kHz, scaled by sample rate.
  `[dsp]`

What this buys and what it costs. Measured by *total* non-harmonic energy
relative to the fundamental — every true harmonic up to Nyquist subtracted — a
2-point PolyBLEP saw sits at about **−40 dB at 110 Hz, −30 dB at 1 kHz and
−26 dB at 4 kHz**, against a naive saw's −23 / −14 dB. A minBLEP table or 4×
oversampling would reach −60 dB and beyond, at several times the cost.
**[PERF-2]** takes the ~−30 dB midband figure.

**Do not quote "−60 dB" for 2-point PolyBLEP.** That number belongs to minBLEP
and to per-partial measurements that count only the loudest few aliases; it is
roughly 20 dB better than what the kernel above actually delivers, and an
acceptance criterion written from it is unpassable. G2.7 states the real
figures and records the measurement.

### 3.2 Pitch

```
semitones = note + bend*BendRange + glide + octave*12 + semi + fine/100
            + lfoPitch + polyModEnvToOsc2        (VCO 2 only)
f = 440 * exp2((semitones - 69) / 12)                       // [dsp]
phaseInc = f / fs                                            // clamped to 0.49
```

`8'` is the reference range: **note 69, all offsets zero, 8' ⇒ 440.000 Hz.**
`16' / 8' / 4' / 2'` ⇒ 220 / 440 / 880 / 1760 Hz. `[ref]`

**Triangle interacts badly with hard sync, and the integrator must be
re-anchored.** A hard-synced slave's truncated square has a non-50/50 duty every
cycle, and the leaky integrator's DC gain (~1000, from the deliberately low
7.6 Hz corner) amplifies that persistent bias into a runaway offset — measured
at 65–70× nominal amplitude and still climbing, for the entirely legitimate
setting `kOsc2Wave = Tri` with `kOsc2Sync = on`. §3.1 and §3.3 are each correct
in isolation and the gap is in their interaction. **At every sync reset, the
integrator is re-anchored to the analytically correct triangle value** for the
slave's phase at that instant. Found by G2's own extreme-grid search (G2.15),
which is exactly the kind of corner a parameter grid exists to find. `[dsp]`

**Phase at note-on: both oscillators reset to 0.** Free-running phase would make
the instrument non-deterministic (R13, and the golden battery depends on it) and
would make unison mode's summed level depend on arrival phase. The cost is a
slightly more "digital" attack consistency than a real VCO, which is a
[PERF]-adjacent trade this design accepts.

**Pulse width is clamped against the phase increment**: `pw` is clamped to
`[max(0.05, dt), min(0.95, 1 - dt)]`. Without this, a 5 % pulse at 4 kHz
(`dt = 0.083`) is narrower than one sample: the two BLEP corrections overlap,
and the naive edge the second correction is supposed to fix may not exist in the
sampled signal at all. `[dsp]`

### 3.3 Hard sync

When VCO 1's phase wraps, VCO 2's phase is reset to the same fractional
overshoot. The reset discontinuity is BLEP-corrected using VCO 2's *actual*
step height at that instant (which is not, in general, the full ±1 of a natural
wrap), so the correction is only approximate. **This is expected**: exact sync
antialiasing needs a MinBLEP table with the correct step scaling, and
**[PERF-2]** does not buy one. G2 therefore holds sync mode to a *looser* alias
floor than the unsync'd waveforms and says so in the AC, rather than pretending
the numbers are the same.

### 3.4 Sub-oscillator

A square at VCO 1's frequency divided by 2 or 4, derived from **VCO 1's own
phase accumulator** — a wrap counter, exactly like the flip-flop divider in the
real circuit. Its edges therefore coincide with VCO 1's wraps, so it reuses the
fractional wrap offset VCO 1 already computed and its BLEP correction is
**free**: no second phase accumulator, no second wrap test. `[dsp]`

### 3.5 Noise

* **White**: `Xorshift32` → `float` in [−1, 1). Each voice is seeded
  deterministically from its voice index in `init()`/`reset()` (R8, R13) so the
  golden battery is reproducible.
* **Pink**: the Kellet 3-pole approximation — three one-poles summed, ≈ −3 dB
  per octave to within about **0.7 dB across 40 Hz – 10 kHz**. It is a 3-pole
  *approximation*; its error grows outside that span, so do not claim
  20 Hz – 20 kHz for it. `[dsp]`

**One noise generator per voice, shared by both stereo chains.** Not one per
chain: an independently-seeded second generator would break the bit-identity
property of §9 that makes the stereo duplication cheap to verify, and two
uncorrelated noise sources hard-panned is a different (wider, hissier) sound
than the one this design specifies.

---

## 4. Mixer and drive  (`synth_voice.h`)

```
mix   = o1*Osc1Level + o2*Osc2Level + sub*SubLevel + noise*NoiseLevel
pre   = 1 + 2*(Drive/100)                  // pre(0) == 1.0 EXACTLY
knee  = 3.0 * (Drive/100)^1.5              // knee(0) == 0.0 EXACTLY
drive = shapeTriodeK(mix * pre, knee) / pre
```

### 4.1 The mixer DC blocker — not optional

A pulse wave of duty `d` carries a DC offset of **exactly `2d − 1`**. That is not
a defect to be tuned out; it is what a pulse *is*. Measured on this
implementation before the blocker existed: **−0.50 at PW = 25 %** and **−0.80 at
PW = 10 %**. PWM is one of this instrument's core sounds, so without AC coupling
the synth emits an enormous DC offset in ordinary use.

So a **one-pole high-pass at 5 Hz sits on the mixer output**, per voice, before
everything downstream. Real hardware does this with a coupling capacitor.
Measured after: **1e-6 to 5e-6 at every duty**, against G3.12's 1e-4 bound.

**It must sit before the drive and the filter, not at the output.** DC into a
saturator biases it into asymmetric clipping, so the timbre would track pulse
width in a way that is not the PWM sound anyone wants; and DC into a resonant
ladder shifts the operating point its feedback saturator is bounded around.

Two consequences worth knowing before writing a test against this chain. The
blocker is a 5 Hz one-pole, so its **step response at note-on has τ = 31.8 ms**
and takes ~160 ms to settle. Any zero-crossing-based pitch or period estimate
must skip that, or it reads the settling transient as pitch drift (G3.7's
control measured 1.43 cents of phantom drift before its window was moved, and
0.0027 cents after). And an unmodulated periodic signal must be averaged over a
**whole number of periods** — 4096 samples at 261.63 Hz is 22.32 periods, and
the truncation alone shows up as ~8e-3 of phantom DC, which is 80× G3.12's
bound. This was found the hard way, twice, during G4's review.

Dividing by `pre` — not by some separate normalisation function — is what makes
the drive a *timbre* control rather than a volume control: `shapeTriodeK` has
unity slope at the origin, so the small-signal gain of the stage is exactly
`pre`, and dividing it back out leaves the quiet parts of the signal untouched
while the loud parts compress. At `Drive = 0` both `pre` and the division are
exactly 1.0 and `knee` is exactly 0.0, so the whole stage is **bit-exact
identity** — which is what G6.7 asserts.

`shapeTriodeK(x, k) = x / (1 + k*|x|)` is lifted verbatim from
`nassau-zermatt/Source/DSP/amp_dsp.h`, **including the reason it exists**:
`k = 0` is *exactly* identity, so the drive control is continuous at zero. The
alternative formulation `s*shape(x/s)` is discontinuous at "off" — that bug was
already found and fixed once in Zermatt (its DESIGN §1.5, C-correction) and must
not be reintroduced here. `[voicing]`

The drive is **mild by construction** (a soft algebraic knee, never a hard clip)
precisely so that it does not need oversampling. This is the third leg of
**[PERF-2]**: the reason there is no oversampled region anywhere in this plugin
is that no stage in it generates enough high-order product to require one. If
someone later wants a fuzz-grade drive, it needs an oversampled region and a new
gate — it is not a knob-range change.

---

## 5. Filters  (`synth_filter.h`)

### 5.1 Two structures behind one slope switch  **[PERF-8]**

The obvious implementation of "24/12 dB switchable" is a 4-pole ladder with a
tap after pole 2. **It does not work**, and the reason is worth stating because
it is not obvious: a cascade of two first-order low-passes inside a negative
feedback loop can never reach 180° of phase shift — it only *asymptotes* to it —
so a 2-pole ladder **cannot self-oscillate at any feedback gain**. The
Jupiter-6 resonates in *both* slope positions. A tap would silently lose that.

So:

* **24 dB mode** → 4-pole **ZDF ladder** (§5.2).
* **12 dB mode** → 2-pole **TPT state-variable filter**, low-pass output (§5.3),
  which resonates properly and self-oscillates — **but only with the resonance
  mapping and the amplitude-regulating damping of §5.3.** The obvious mapping
  `Q = 0.5 + 19.5*res` does *not* self-oscillate at any setting, and a
  `shapeTriodeK` in the feedback cannot make it. §5.3 explains why.

Only one structure runs at a time. Changing `kLpfSlope` **crossfades over 20 ms**
with both structures running, then drops the unused one. A slope switch is a
rare, deliberate gesture, so paying 2× filter cost for 20 ms is free in any
average sense — and it avoids the click that resetting one structure's state
would otherwise produce.

### 5.2 The 24 dB ZDF ladder

Per pole, TPT (Zavalishin):

```
g = tan(pi * fc / fs)        // once per control block; interpolated per sample
G = g / (1 + g)
v = (x - s) * G ;  y = v + s ;  s = y + v
```

The four-pole global-feedback loop is solved in closed form. With
`S_i = (1 - G) * s_i` the zero-input output of stage `i`:

```
Sigma = G^3*S1 + G^2*S2 + G*S3 + S4
y4    = (G^4 * x + Sigma) / (1 + k * G^4)       // linear zero-delay solution
u     = x - k * shapeTriodeK(y4, kSat)          // ONE fixed-point step  [PERF-3]
then run the four TPT poles on u to advance state.
```

**[PERF-3]**: a true nonlinear ladder puts a saturator inside *each* pole and
needs Newton iteration per sample to solve the implicit system. This design
solves the *linear* loop exactly and applies the saturator to the resulting
feedback signal in one step. It is stable, it is one division per sample, and it
still bounds the self-oscillation amplitude — which is the audible job the
nonlinearity actually does. It is not a Moog-accurate ladder and this document
does not claim it is.

`k` is the resonance: `k = 4.2 * (Resonance/100)`. `[voicing]` The theoretical
oscillation threshold of the 4-pole loop is exactly 4, so 4.2 puts
self-oscillation comfortably inside the top of the knob's travel.

`kLadderSat = 0.10` `[voicing]` is the feedback saturator's knee. This is the
constant that sets the self-oscillation amplitude, so it is worth knowing where
it comes from: the describing-function gain of `y/(1 + k|y|)` for a sine of
amplitude `A` is about `1/(1 + 0.85kA)`, and the loop settles where that gain
brings `k = 4.2` back down to the threshold 4.0 — i.e. `0.85*kLadderSat*A ≈
0.05`, giving **A ≈ 0.6**. That sits inside the `[0.05, 2.0]` window G4.4
asserts. Here the saturator's *gain reduction* is what bounds the oscillation,
which is why this placement is stable; §5.3's is not the same situation.

**The −3 dB point of this filter is NOT at `fc`.** Four identical one-poles are
−3 dB at `fc * sqrt(2^(1/4) - 1)` = **0.4350·fc**. This is exactly the same
arithmetic as the high-pass warning in §5.5, and it applies here too — an
acceptance criterion demanding the corner sit at `kLpfCutoff` fails on a
perfectly correct filter by a factor of 2.3.

**Resonance loses bass and that is deliberate.** The ladder's global feedback
subtracts the low-frequency content; a real Prophet and a real Jupiter both do
this and it is a large part of why a resonant sweep sounds the way it does.
There is **no makeup gain**. §5.5's AC bounds the loss so that we know it is
happening on purpose rather than by accident.

### 5.3 The 12 dB TPT SVF

```
g = tan(pi*fc/fs) ;  d = 1 / (1 + 2*Reff*g + g*g)
hp   = (x - (2*Reff + g)*ic1 - ic2) * d
bp   = g*hp + ic1 ;  ic1 = g*hp + bp
lp   = g*bp + ic2 ;  ic2 = g*bp + lp        // lp is the output
```

**Resonance maps to the damping `R`, not to `Q`:**

```
R0   = 1.0 - 1.005 * (Resonance/100)     // [voicing]  res=0 -> +1.0, res=100 -> -0.005
Reff = clamp(R0 + kSvfSat * peakBpPrev^2, -0.02, 2.0)   // kSvfSat = 0.02  [voicing]
```

`peakBpPrev` is the peak `|bp|` observed over the **previous control block**, so
`Reff` — and therefore the division `d` — is recomputed once per control block,
not per sample.

Two things here are deliberate and were both got wrong on the first pass of this
design:

1. **`R` must be able to reach zero and go slightly negative.** The natural
   mapping `Q = 0.5 + 19.5*res` gives `R = 1/(2Q) = 0.025 > 0` at full
   resonance — strictly positive damping, poles strictly inside the unit circle,
   ringing that decays at `R*w0` nepers per second and never sustains. **A
   2-pole SVF with positive `R` cannot self-oscillate at any `Q`, however
   large.** `R0` reaching −0.005 is what actually creates the oscillation.

2. **The nonlinearity regulates the damping, it does not saturate the signal.**
   Putting a `shapeTriodeK` on `ic1` inside the `(2R + g)` term is worse than
   useless once `R0 < 0`: a saturator has gain ≤ 1, and reducing the magnitude
   of a *negative* damping term makes the filter **more** unstable, not less.
   Making `Reff` grow with amplitude instead gives a Van-der-Pol-style limit
   cycle that settles exactly where `Reff = 0`, i.e. at
   `peakBp = sqrt(-R0/kSvfSat)` = **0.5** at full resonance — inside G4.4's
   window, and bounded by construction.

   This is the opposite arrangement from the ladder in §5.2, where the loop has
   genuine *excess* gain (`k = 4.2 > 4`) and a gain-reducing saturator is
   exactly the right bounding mechanism. Same goal, structurally different
   filters, structurally different answers.

**The −3 dB point of this filter is not at `fc` either**: at `Resonance = 0`
(`R0 = 1.0`, two coincident real poles) it sits at **0.6436·fc**.

### 5.6 The post-filter DC blocker — also not optional

A second one-pole 5 Hz high-pass sits on the **LPF output**, per voice, before
the VCA.

The mixer blocker of §4.1 does not cover this, and assuming it does is a real
mistake that was made here once. A DC-free *input* does not give a DC-free
*output*: both structures' feedback saturators are **odd** functions, and an odd
function fed a signal that is zero-mean but **not half-wave-symmetric** — which
any pulse at duty ≠ 50 % is — re-introduces a nonzero time-average. Measured
with only the mixer blocker present:

```
SVF, res 99 %, PW 30 %, fc 500 Hz    DC = 2.3e-2   peak 3.69     <- 230x the bound
SVF, res 99 %, PW 50 %, fc 500 Hz    DC = 1.5e-6                 <- symmetric: no DC
SVF, res  0 %, PW 30 %, fc 500 Hz    DC = 7.5e-4                 <- no resonance: small
```

The two controls are what identify the mechanism: DC appears only when the input
is asymmetric **and** the nonlinear feedback is engaged. With the blocker the
worst corner reads **−4.1e-5**. This is the same mechanism, and the same remedy,
as `nassau-zermatt`'s `mCfDcBlock` and `mPowerDcBlock` — its DESIGN records the
identical reasoning about odd functions and asymmetrically-shaped inputs.

**Known residual, recorded not bounded.** At extreme resonance (99 %) the SVF's
control-rate damping regulation produces slow sub-5 Hz *wander* that a 5 Hz
blocker cannot remove. Measured over successive 1 s windows at
{res 99 %, PW 30 %, fc 500 Hz, note 48}: −1.06e-4, −7.0e-5, **+6.2e-5**,
+7.6e-6, −1.5e-4. It **changes sign**, so it is not an offset and it averages
toward zero over longer windows — which is why G3.12's 1e-4 bound stands rather
than being loosened to accommodate it. Raising the blocker corner would fix the
number and cost real low-end on 16' notes; it is not worth it.

### 5.4 Cutoff modulation

```
fc = clamp( LpfCutoff
            * exp2( KeyFollow/100 * (note - 60)/12 )
            * exp2( 6.0 * EnvAmount/100 * envF )      // bipolar, +/- 6 octaves
            * exp2( 2.0 * LfoAmount/100 * lfo ),      // +/- 2 octaves
            10.0, 0.45*fs )
```

The `0.45*fs` clamp is not cosmetic: `tan(pi*fc/fs)` diverges at Nyquist. At the
clamp the tangent's argument is `pi*0.45 = 1.4137`, which is the largest value
any `tan` call in this plugin ever sees. `[dsp]`

Key follow of 100 % means one octave of cutoff per octave of keyboard, referred
to middle C (note 60).

### 5.5 The high-pass

A cascade of **identical** TPT one-pole high-passes — 2 of them in 12 dB mode,
4 in 24 dB mode. No resonance (the Jupiter-6's high-pass has none). `[ref]`

```
fc_hp = clamp( HpfCutoff * exp2( HpfKeyFollow/100 * (note - 60)/12 ),
               10.0, 0.45*fs )
```

**`kHpfCutoff` at its minimum (20 Hz) is a HARD BYPASS**: the high-pass stages
are skipped entirely and the signal passes bit-exactly. Not a 20 Hz filter — an
actual bypass. Two reasons, and the second is the load-bearing one:

* it is what the leftmost position of a real instrument's high-pass switch does,
  and it is cheaper;
* **without it there is no exact-identity setting for this block**, and every
  golden fixture captured before the high-pass existed becomes unverifiable. A
  2-pole 20 Hz high-pass attenuates a 220 Hz fundamental by ≈ 0.07 dB and
  rotates its phase — a relative error of ~8e-3 against a golden tolerance of
  1e-6, i.e. a failure by three and a half orders of magnitude, on a completely
  correct filter. G6.16 and G8.9 depend on this bypass existing.

**A cascade of identical one-poles is not a Butterworth**, and its −3 dB point
is *not* at `fc`. For `n` identical one-pole high-passes the −3 dB point sits at
`fc / sqrt(2^(1/n) - 1)`, i.e. at **1.554·fc** for n = 2 and **2.30·fc** for
n = 4. Anyone writing or reading an AC against this filter must probe the
asymptotic slope, not the corner (see GATES.md G6.3's "Do not" note).

---

## 6. Envelopes  (`synth_env.h`)

Analogue-style: a one-pole running toward a target it never reaches, so the
curve is exponential rather than linear. Two per voice — **ENV-F** to the filter
and **ENV-A** to the VCA — and they are **shared by both stereo chains**
(**[PERF-5]**).

| Stage | Target | Coefficient | Exit condition |
|---|---|---|---|
| Attack  | `1.15`   | `a = 1 - exp(-1/(tau*fs_c))`, `tau = t_A / 2.037` | `y >= 1.0` → Decay |
| Decay   | `S`      | `tau = t_D / 4.6`                                 | within 1 % of `S` → Sustain |
| Sustain | `S`      | held                                              | note-off → Release |
| Release | `-0.05`  | `tau = t_R / 3.045`                               | `y <= 0` → Idle, `y := 0` |

`fs_c = fs / kControlBlock` — the envelopes run at the control rate.

The three magic divisors are **derived, not tuned**, and are written here so
nobody re-guesses them:

* Attack overshoots to 1.15 and must cross 1.0 at exactly `t_A`:
  `t = tau*ln(1.15/0.15)`, and `ln(7.6667) = 2.037`.
* Decay is defined as reaching **within 1 %** of sustain in `t_D`:
  `ln(100) = 4.605`.
* Release is defined from **full scale** down to zero with an undershoot target
  of −0.05: `t = tau*ln(1.05/0.05)`, and `ln(21) = 3.045`. From a sustain level
  below 1.0 the release is correspondingly faster — that is the behaviour of the
  real circuit and it is not a bug.

Times: **1 ms .. 10 s**, exponentially tapered on the knob. `[voicing]`

Consequence worth knowing before writing the AC: the attack curve reaches 50 %
at `tau*ln(1.15/0.65) = 0.5705*tau` and 99 % at `tau*ln(1.15/0.16) = 1.9723*tau`,
so **the 50 %/99 % time ratio is 0.289**. A linear ramp would give 0.505. That
ratio is how G3 proves the curve is actually exponential.

---

## 7. LFO  (`synth_lfo.h`)

**One global LFO** (**[PERF-4]**), advanced once per control block, read by
every voice. The Jupiter-6 has one LFO; per-voice LFOs would multiply the cost
by the polyphony for a difference this instrument's reference targets do not
have anyway.

* Waves: Triangle, Saw (falling), Ramp (rising), Square, Sample & Hold. The S&H
  source is a dedicated `Xorshift32` seeded in `init()` (R8/R13).
* Rate: 0.05 .. 30 Hz, exponential taper. `[voicing]`
* Delay: 0 .. 3000 ms. The delay is **per note-on of the first held note** — the
  LFO depth ramps in linearly after the delay, over 200 ms. `[ref]`
  **At `kLfoDelay = 0` there is no ramp at all**: full depth immediately. A
  control at zero means off, and the default must not carry an unrequested
  200 ms fade. (Raised by G1, which had to pick one reading; settled here.)
  It resets only when the voice count goes from 0 to 1, so it does not restart
  under a held chord.
* Destinations: pitch (± 50 cents at 100 %), pulse width (± 45 % at 100 %),
  and LPF cutoff (§5.4). None of the three is interpolated across a control
  block; see §2.

---

## 8. Poly-Mod, the cheap half  **[PERF-6]**

Two Prophet-flavoured routings, both **control-rate only**:

* `kPmEnvFToOsc2` — ENV-F to VCO 2 pitch, bipolar, ± 24 semitones at 100 %.
* `kPmEnvFToPw`   — ENV-F to both oscillators' pulse width, bipolar, ± 45 %.

**Audio-rate oscillator-to-oscillator cross-modulation is excluded from v1.**
It is the one Poly-Mod destination that genuinely needs an oversampled region:
FM by a full-bandwidth saw produces sidebands far above Nyquist by construction,
and PolyBLEP corrects a *waveform's* discontinuities, not a modulator's
sidebands. Adding it means adding oversampling to the oscillator section, which
is the single most expensive thing this design has avoided. See §12.

---

## 9. Stereo mode

`kStereoMode = On` runs the **entire per-chain signal flow twice per voice**
(§1, boxes 1–11), with exactly two differences in chain 1:

1. **Every fine-tune quantity is negated**: `Osc1Fine`, `Osc2Fine`, and
   `StereoDetune`. If chain 0 runs VCO 2 at +7 cents, chain 1 runs it at −7.
2. **Pan**, with a **linear**, not equal-power, law:

```
s = Spread/100
chain 0:  gL = 0.5 + 0.5*s ,  gR = 0.5 - 0.5*s
chain 1:  gL = 0.5 - 0.5*s ,  gR = 0.5 + 0.5*s
mono mode: chain 0 only, gL = gR = 1.0
```

At `Spread = 0` both chains land centred in both outputs — a mono-compatible
thickening rather than a width effect. At `Spread = 100` chain 0's right gain is
**exactly 0.0**.

**Linear rather than equal-power is a deliberate choice and it buys two exact
properties.** Equal-power would put `1/sqrt(2)` on each chain at centre, so two
centred chains would sum to `sqrt(2)` times one — 3 dB louder than mono mode,
with no exact way to reconcile them. With the law above, `0.5*x + 0.5*x == x`
**bit-exactly** in IEEE-754 (halving and doubling are exact), so:

* stereo-at-`Spread = 0`-with-zero-detune is bit-identical to mono mode, which
  is the cheap correctness check below; and
* the **mono sum `L + R` is independent of `Spread`** — `gL + gR = 1.0` for each
  chain at every spread. Collapsing to mono cannot change the level, only the
  detune beating can.

Equal-power's usual justification is uncorrelated sources; these two chains are
the same note a few cents apart, i.e. strongly correlated, which is the case
where linear panning is the more correct law anyway.

Everything else — ENV-F, ENV-A, the LFO, glide, key follow, velocity, the
filter cutoff — is **shared** (**[PERF-5]**). Only the oscillator phase
increments and the two filter/VCA state sets differ. That is what keeps the
measured stereo cost below 2×: the modulation half of the per-voice work is paid
once.

**Two properties that make this cheap to test, and that G8 asserts:**

* Mono mode must write **bit-identical** L and R.
* Stereo mode with *all* fine tunings and `StereoDetune` at zero and
  `Spread = 0` must produce output **bit-identical to mono mode**. If chain 1 is
  a true duplicate of chain 0, negating zero changes nothing. This single check
  catches almost every way the duplication can be wired wrong — but only if the
  pan gains above are used verbatim, and only if the two chains share one noise
  generator (§3.5). Both are requirements, not conveniences.

**Enabling stereo mode costs ~3 dB when the output is folded to mono, and that
is a floor, not a tuning choice.** Because `gL + gR = 1.0` per chain, a mono
fold-down of stereo mode sums the two *detuned* chains — which are mutually
incoherent — whereas mono mode sums one chain to itself coherently. Incoherent
addition gives `sqrt(2)·A`, coherent gives `2A`, so the ratio is
`20·log10(sqrt(2)/2)` = **−3.01 dB** for any nonzero detune, independent of how
large the detune is. Verified against a two-sine model: −2.94 dB at ±2 cents,
−3.00 dB at ±25 cents, and exactly 0.00 dB at zero detune. The instrument
measures −3.37 dB, the extra fraction being harmonic content rather than a pure
tone.

This is inherent to detuning anything and it is why G8.7 bounds the mono-sum at
4 dB rather than the 1.5 dB an earlier draft demanded — that figure was
arithmetically impossible for any correct implementation. In stereo playback
there is no loss; both chains are heard at full level.

**Mono-sum behaviour is a known, accepted artifact.** Two chains detuned by
±d cents beat against each other; summed to mono their level modulates at the
beat frequency. Because `gL + gR = 1.0` at every spread, this is the *only*
mono-sum effect there is — the pan law contributes none. G8 bounds the
*time-averaged* level and **records** the peak-to-trough swing rather than
bounding it; bounding it would be demanding that detuning not detune.

---

## 10. Voices, allocation and MIDI  (`synth_voice.h`, `synth_alloc.h`)

### 10.1 Where this lives, and why

**All of it is in `Source/DSP/`, not in the plugin wrapper** (R14). The core
accepts a plain, framework-free event list per block:

```cpp
struct NoteEvent {
    int  sampleOffset;   // 0 .. numSamples-1
    enum Type { NoteOn, NoteOff, PitchBend, Sustain, AllNotesOff, AllSoundOff };
    Type type;
    int  note;           // 0..127
    float value;         // velocity 0..1, bend -1..+1, sustain 0/1
};
void SynthCore::process(const NoteEvent* events, int numEvents,
                        float* outL, float* outR, int numSamples);
```

The IPlug2 wrapper's *entire* MIDI job is translating `IMidiMsg` into that
struct. Everything testable is therefore testable with the SDK absent — which,
given §0.4, is the difference between a plan that can be executed on the
development box and one that cannot.

### 10.2 Event timing  **[PERF-1]**

Events are **quantised forward to the control-block grid**: an event at sample
offset `s` takes effect at the start of the next control block boundary at or
after `s`. A control block is never split and never started early, so the event
lands somewhere in `[s, s + 31]`. **Worst-case latency 31 samples = 0.65 ms at
48 kHz**; average 16 samples.

This is stated as a specification, not discovered as a defect. Sample-accurate
note starts would require splitting the block at every event, which fights the
control-rate architecture directly. 0.65 ms is below the threshold where note
onsets read as loose.

### 10.3 Allocation

* `kPolyphony` selects 4 / 6 / 8 / 12 / 16 active voices; the core always holds
  **`kMaxVoices = 16` plus 2 fade-out slots** (§10.4), all constructed in
  `init()`. No allocation ever happens in `process()` (R3).
* Note-on: first **Idle** voice; else the oldest **Released** voice; else the
  oldest **Playing** voice.
* Re-triggering a note that is already sounding reuses that voice.
* Note-off with sustain (CC 64) held moves the voice to a **Held** state; it
  releases when the pedal lifts.
* CC 123 (all notes off) releases every voice normally. CC 120 (all sound off)
  routes every voice through the fade-out slots (§10.4).

### 10.4 Stealing does not click

A stolen voice is **not** hard-reset. It is moved into one of two dedicated
**fade-out slots**, where it continues to render with a 2 ms linear fade to
zero, while the new note starts immediately on the freed voice. Two slots is
enough: a third steal within 2 ms of two others is not a musical situation, and
if it happens the oldest fade slot is simply overwritten (which is at most a
2 ms-old, already-fading signal).

### 10.5 Voice modes

* **Poly** — as above.
* **Unison** — all `kPolyphony` voices play the held note, spread symmetrically
  across ±`StereoDetune` cents. Cost is `kPolyphony` × a single note; this is
  the one mode that can approach the CPU budget, and G11 measures it.
* **Mono** — one voice, last-note priority, legato (no envelope retrigger while
  a key is still held), glide always applied.

### 10.6 Glide

Exponential, one-pole toward the target pitch in semitones, at control rate.
`kGlideTime` is the time to reach **within 1 %** of the target, so
`tau = t / 4.605` — same convention as the envelope decay (§6), deliberately,
so there is one definition of "time" in this plugin and not two.

### 10.7 Silent-voice skip  **[PERF-7]**

A voice is skipped **entirely** — not processed and not summed — when its ENV-A
is Idle *and* its stored peak over the previous control block was below
−100 dBFS. This is what makes an 8-voice instance holding one note cost roughly
one voice, and it is the reason the CPU budget in §12 is quoted per *active*
voice.

---

## 11. Parameter surface (53 params, append-only)

Indices are **final and frozen** (R4). Each gate lands exactly the next
contiguous run. `kNumParams == 53 == PLUG_N_PARAMS`.

| # | Param | Range | Default | Gate |
|---|---|---|---|---|
| 0 | `kMasterVolume` | −60 .. +12 dB | −6 | G0 |
| 1 | `kOutputClip` | bool | on | G0 |
| 2 | `kOsc1Wave` | Saw / Pulse / Tri | Saw | G2 |
| 3 | `kOsc1Octave` | 16' / 8' / 4' / 2' | 8' | G2 |
| 4 | `kOsc1Fine` | −50 .. +50 cents | 0 | G2 |
| 5 | `kOsc1PW` | 5 .. 95 % | 50 | G2 |
| 6 | `kOsc1Level` | 0 .. 100 % | 100 | G2 |
| 7 | `kOsc2Wave` | Saw / Pulse / Tri | Saw | G2 |
| 8 | `kOsc2Octave` | 16' / 8' / 4' / 2' | 8' | G2 |
| 9 | `kOsc2Semi` | −12 .. +12 | 0 | G2 |
| 10 | `kOsc2Fine` | −50 .. +50 cents | −7 | G2 |
| 11 | `kOsc2PW` | 5 .. 95 % | 50 | G2 |
| 12 | `kOsc2Level` | 0 .. 100 % | 80 | G2 |
| 13 | `kOsc2Sync` | bool | off | G2 |
| 14 | `kOsc2KeyTrack` | bool | on | G2 |
| 15 | `kSubOctave` | −1 / −2 | −1 | G2 |
| 16 | `kSubLevel` | 0 .. 100 % | 0 | G2 |
| 17 | `kNoiseColor` | White / Pink | White | G2 |
| 18 | `kNoiseLevel` | 0 .. 100 % | 0 | G2 |
| 19 | `kEnvFAttack` | 1 .. 10000 ms | 2 | G3 |
| 20 | `kEnvFDecay` | 1 .. 10000 ms | 400 | G3 |
| 21 | `kEnvFSustain` | 0 .. 100 % | 30 | G3 |
| 22 | `kEnvFRelease` | 1 .. 10000 ms | 300 | G3 |
| 23 | `kEnvAAttack` | 1 .. 10000 ms | 2 | G3 |
| 24 | `kEnvADecay` | 1 .. 10000 ms | 800 | G3 |
| 25 | `kEnvASustain` | 0 .. 100 % | 80 | G3 |
| 26 | `kEnvARelease` | 1 .. 10000 ms | 250 | G3 |
| 27 | `kLfoWave` | Tri/Saw/Ramp/Sqr/S&H | Tri | G3 |
| 28 | `kLfoRate` | 0.05 .. 30 Hz | 5 | G3 |
| 29 | `kLfoDelay` | 0 .. 3000 ms | 0 | G3 |
| 30 | `kLfoPitchAmount` | 0 .. 100 % | 0 | G3 |
| 31 | `kLfoPwmAmount` | 0 .. 100 % | 0 | G3 |
| 32 | `kLpfSlope` | 24 / 12 dB | 24 | G4 |
| 33 | `kLpfCutoff` | 20 .. 18000 Hz | 2000 | G4 |
| 34 | `kLpfResonance` | 0 .. 100 % | 20 | G4 |
| 35 | `kLpfEnvAmount` | −100 .. +100 % | 40 | G5 |
| 36 | `kLpfKeyFollow` | 0 .. 100 % | 50 | G5 |
| 37 | `kLpfLfoAmount` | 0 .. 100 % | 0 | G5 |
| 38 | `kDrive` | 0 .. 100 % | 15 | G6 |
| 39 | `kHpfSlope` | 12 / 24 dB | 12 | G6 |
| 40 | `kHpfCutoff` | 20 .. 2000 Hz | 20 | G6 |
| 41 | `kHpfKeyFollow` | 0 .. 100 % | 0 | G6 |
| 42 | `kPmEnvFToOsc2` | −100 .. +100 % | 0 | G6 |
| 43 | `kPmEnvFToPw` | −100 .. +100 % | 0 | G6 |
| 44 | `kPolyphony` | 4/6/8/12/16 | 8 | G7 |
| 45 | `kVoiceMode` | Poly / Unison / Mono | Poly | G7 |
| 46 | `kGlideTime` | 0 .. 2000 ms | 0 | G7 |
| 47 | `kBendRange` | 0 .. 24 semi | 2 | G7 |
| 48 | `kVelToVca` | 0 .. 100 % | 40 | G7 |
| 49 | `kVelToFilter` | 0 .. 100 % | 20 | G7 |
| 50 | `kStereoMode` | bool | off | G8 |
| 51 | `kStereoDetune` | 0 .. 25 cents | 6 | G8 |
| 52 | `kStereoSpread` | 0 .. 100 % | 70 | G8 |

### Channel configuration

`PLUG_CHANNEL_IO "0-2"`. This is an **instrument**: `PLUG_TYPE 1`,
`PLUG_DOES_MIDI_IN 1`, `PLUG_DOES_MIDI_OUT 0`. Unlike Zermatt there is no mono
core and no mono-sum wrapper — `SynthCore` is natively stereo out.

### Output stage

```
outL/R = MasterVolume_linear * accumulator
if (kOutputClip) {
    outL/R = shapeCubic(outL/R, 2.0)
    outL/R = OutputDcBlock(outL/R)     // 1-pole HP @ 5 Hz — see below
}
```

`shapeCubic(x, L) = x + x^3/(2L^2) - x^5/(2L^4)` for `|x| < L`, `±L` beyond —
lifted from Zermatt's `amp_dsp.h`. With `L = 2.0` the ceiling is +6 dBFS, and at
x = 0.25 (−12 dBFS):

```
0.25 + 0.015625/8 - 0.0009765625/32
  = 0.25 + 0.001953125 - 0.000030517578125
  = 0.251922607421875           exactly, i.e. +0.06654 dB
```

**Both terms count.** Quoting only the cubic term gives 0.251953125 / +0.0676 dB
— a different number, and one that a verbatim lift of Zermatt's `shapeCubic`
will never produce. G1.19 and G6.12 assert `0.251922607421875` bit-exactly; it
is arithmetic, not a measurement, so an implementation that disagrees with it is
wrong.

**The output clip needs its own DC blocker, found at G6, same mechanism as
§5.6, one stage further downstream.** `shapeCubic` is an odd function, exactly
like the filters' feedback saturators, and by G6 the drive stage (§4) sits
upstream of it in the same chain — so a non-half-wave-symmetric signal (any
pulse at duty ≠ 50 %, or a plain saw) that reaches the clip's nonlinear region
re-introduces DC that neither the mixer blocker (§4.1) nor the post-filter
blocker (§5.6) can remove, because both sit *before* the clip. Measured with
only those two: worst corner 1.94e-3 (pulse, PW = 25 %, Drive = 0 %, HPF
bypassed, output clip on) against every other cell of the same grid at
≈1e-10 — the clip is unambiguously the source. A third one-pole 5 Hz
high-pass on the clipped signal, mirroring the two upstream blockers exactly,
brings the whole grid back to ≈1e-10. It runs **only when `kOutputClip` is
on**: with the clip off there is no clip-introduced DC to remove (the signal
is already clean by construction, §4.1/§5.6), and gating it this way is what
keeps `kOutputClip = off` a bit-exact passthrough of the master-scaled sum
(G6.12). **Per channel as of G8** — `outL`/`outR` are identical only up to
mono mode / stereo mode at `Spread = 0` with zero detune; stereo mode
otherwise gives them independent content, and a single shared 5 Hz blocker
fed two different interleaved signals would not be either channel's correct
filter (it is a stateful IIR recursion). Two independent instances, reset
identically, cost G8.1's mono bit-identity nothing: fed the identical input
sequence from the identical starting state, they produce bit-identical output
sequences.

### Reset semantics

`reset()` clears every voice's oscillator phase, filter state, envelope state
and noise generator seed, empties the allocator, snaps every `SmoothedValue` to
its target, and resets the LFO phase — **without touching any parameter**.

---

## 12. Performance budget and the escape hatches

**The budget (G11.5):** 8 voices, all sounding, mono mode, 48 kHz, Release
build, best of 7 — **≥ 10× realtime, i.e. ≤ 10 % of one core.** Stereo mode
≤ 2.0× the mono cost. An all-idle instance ≤ 5 % of the 8-active cost.

The 10× is a **floor, deliberately set below the expectation**.

**MEASURED (G11, Xeon E5-1650 v3 @ 3.5 GHz, 48 kHz, Release, best of 7):**

| config | ns/sample | ×realtime |
|---|---|---|
| idle (0 voices) | 19.0 | 1099× |
| 1 voice | 72.2 | 288× |
| **8 voices mono** | **467.6** | **44.6×** |
| 8 voices stereo | 851.4 | 24.5× |
| 16 voices unison | 919.1 | 22.7× |

Per-voice marginal cost **56.1 ns/sample**. (Before the G11 ladder
restructure: 516.5 ns / 40.3× / 62 ns per voice.)

**Where it goes**, measured per component at the real call rate:

| component | ns/sample | note |
|---|---|---|
| LadderFilter (24 dB) | 19.3 | was 24.4 before the restructure |
| SvfFilter (12 dB) | 7.7 | only one slope runs at a time |
| HpfCascade 2-pole | 4.2 | 5.6 at 4 poles |
| SubOsc | 3.4 | |
| OnePoleHP × 2 | 4.3 | mixer + post-LPF blockers |
| Osc saw / pulse | ~2.0 each | |
| NoiseSource | 1.8 | |
| drive | 0.8 | |

**The low-pass is the cost centre** — roughly a third of a voice even after
being made 1.30× faster. Anything further has to go through it.

**The budget is met with a 4× margin**, and it was already met *before* any
optimization (40.0×), so §12's escape hatches were correctly never used.

**The 50–100× estimate this section used to carry was optimistic by about 2×,
and is corrected here rather than left standing.** The measured marginal cost is
**~62 ns per voice per sample** — confirmed linear across the range (62.9 ns
from idle to 1 voice, 62.2 from 1 to 8, 62.6 from 8 to 16, which is also what
proves the benchmark is processing real voices rather than skipping them). At
3.5 GHz that is ~218 cycles per voice per sample, against an op-count guess of
~100 flops. The gap is everything an op count omits: three DC blockers, four HPF
poles, four ladder poles plus a division, the unpredictable branches inside
PolyBLEP, the slope-structure branch, and the per-sample lerps. Op counts
under-predict real DSP cost by roughly this factor as a rule; the floor was set
low precisely because the estimate could not be trusted, and that judgement was
correct. NassauZermatt's G9.5 is the cautionary tale: a target invented before any
code existed (50× realtime) was missed by 18 %, and the honest thing turned out
to be recording the shortfall rather than moving the target. Setting a floor
that is defensible from an op count, and recording the actual, avoids repeating
that.

### 12.1 Optimizations tried and REJECTED, with their measurements

Recorded per G11.10, because a tried-and-reverted change with a number is
worth more to the next person than silence.

| tried | measured | verdict |
|---|---|---|
| `fastTan` / `fastExp2` (hatch 1) | control-rate work is **≤ 5.7 %** of total cost — halving its *rate* (32→64) bought only that, and transcendentals are a fraction of that fraction | rejected; §2.1's argument confirmed empirically |
| `kControlBlock` 32 → 64 (hatch 2) | **5.7 %** faster (640 → 622 ns in an 8-voice driver), at the price of doubling event quantisation to 1.3 ms | rejected — poor trade |
| `kControlBlock` 32 → 128 | 4.8 % beyond that | rejected — same reason, worse |
| rcp + Newton reciprocal replacing the saturator's division | **31.91 vs 20.97 ns/sample — 52 % SLOWER.** Three Newton steps cost far more than one `divsd`, and the division is only 11 % of the ladder | rejected |
| `float` instead of `double` in the per-sample path | **1.03×** in scalar on x86-64 | rejected here — it would only pay 4-wide in SIMD, or on hardware with half-rate doubles (many ARM cores). Still open for a weak-machine build |

**If the floor is ever missed, in this order:**

1. `fastTan` / `fastExp2` (§2.1) — but only after profiling shows they are more
   than 1 % of the total, which the arithmetic in §2.1 says they will not be.
2. Widen `kControlBlock` from 32 to 64 — halves all modulation cost, doubles the
   event-timing quantisation to 1.3 ms. Requires re-running G3.9 and G7.7.
3. **SIMD over voices**: render 4 voices at a time from a structure-of-arrays
   voice pool. This is the only change here with a real payoff and it is also
   the most dangerous — it is the same class of change that caused Zermatt's G6
   revert. It must not be attempted without the golden battery (G11.1) already
   frozen and watching.

**No `-ffast-math` and no `-march=native`, ever** (R7). The ZDF solve and every
IIR recursion in §5 are unsafe under fast-math.

---

## 13. Non-goals for v1

Each of these is excluded for a stated reason, not overlooked:

* **Audio-rate cross-modulation / full Prophet Poly-Mod** — needs an
  oversampled oscillator section (§8).
* **Oversampling of any kind** — the whole design is arranged so nothing needs
  it (§4).
* **Per-voice LFOs** — **[PERF-4]**.
* **Arpeggiator, sequencer, chord memory** — not DSP; a separate plugin's job.
* **Effects (chorus, delay, reverb)** — the stereo mode (§9) is the only
  width-generating element, deliberately. A Juno-style chorus is a plausible v2
  and would sit after the master volume, outside every voice.
* **MPE** — `PLUG_DOES_MPE 0`. Per-note expression is a voice-architecture
  change, not a feature flag.
* **Microtuning / alternative scale tables** — the pitch path in §3.2 is
  12-TET by construction.
