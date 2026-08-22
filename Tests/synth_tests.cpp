// NassauAnalogue DSP Unit Tests — Gate G0 (scaffold + pipeline proof).
// Hand-rolled harness (plain int main + soft checks, no external framework) —
// the nassau-eq/nassau-zermatt house style (see
// nassau-zermatt/Tests/amp_tests.cpp's first ~70 lines, and docs/GATES.md
// "Shared test harness"). Determinism (R8): no rand()/time(); randomness
// comes from an inline xorshift32 PRNG with a fixed seed.
//
// At G0, SynthCore::process() wrote EXACT SILENCE regardless of params/events
// (no voice/oscillator/filter/envelope existed yet), so most groups below
// still assert silence/finiteness/no-crash rather than a signal shape; the
// meaningful new-at-G0 behaviour under test is the control-rate grid's
// persistence (DESIGN.md §2) and the 53-param setter surface (DESIGN.md §11)
// compiling, running and being thread-safe (R3, G0.9). G3 (docs/GATES.md)
// wires in a real, enveloped voice, so the ONE check that specifically
// asserted silence for a NoteOn/NoteOff-bearing event stream (G0.8's block-
// size loop) was narrowed to what its own AC text actually requires --
// finiteness -- with a separate, still-exact silence check kept for the
// genuinely-silent numEvents==0 case. See that group's own comment.

#include "synth_core.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>
#include <thread>
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

// ---- Deterministic PRNG (xorshift32), R8: no rand(), no time() ------------
struct TestXorshift32 {
    uint32_t s;
    explicit TestXorshift32(uint32_t seed) : s(seed ? seed : 0x1234567u) {}
    uint32_t next() {
        uint32_t x = s;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        s = x;
        return x;
    }
    // Uniform in [-1, 1).
    float nextBipolar() {
        return (static_cast<float>(next() >> 8) / 8388608.0f) - 1.0f;
    }
};

} // namespace

// ---- G0.11: global allocation counter --------------------------------------
// Overriding the global operator new/delete lets us measure, by DELTA across
// a specific region of code, whether SynthCore::process() allocates. Every
// common (sized/unsized, scalar/array) form is overridden so no call site can
// bypass the counter via a different overload resolution (docs/GATES.md
// G0.11: "global operator new/delete counter").
namespace {
std::atomic<long long> gAllocCount{0};
std::atomic<long long> gDeallocCount{0};
} // namespace

void* operator new(std::size_t sz) {
    ++gAllocCount;
    void* p = std::malloc(sz ? sz : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t sz) {
    ++gAllocCount;
    void* p = std::malloc(sz ? sz : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept {
    ++gDeallocCount;
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
    ++gDeallocCount;
    std::free(p);
}
void operator delete[](void* p) noexcept {
    ++gDeallocCount;
    std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
    ++gDeallocCount;
    std::free(p);
}

int main() {
    std::cout << "=== NassauAnalogue DSP Tests (G0) ===\n\n";

    // ------------------------------------------------------------------ API ---
    std::cout << "Group: API / lifecycle (G0.6)\n";
    {
        SynthCore core;
        core.init(44100.0f);
        check("construct + init sets sample rate", core.getSampleRate() == 44100.0f);

        // G0.6: every one of the 5 named rates is accepted and reported back.
        const float rates[] = {44100.0f, 48000.0f, 88200.0f, 96000.0f, 192000.0f};
        bool allOk = true;
        for (float fs : rates) {
            core.init(fs);
            if (core.getSampleRate() != fs) allOk = false;
        }
        check("init() accepts 44100/48000/88200/96000/192000, getSampleRate() reports each back",
              allOk);

        core.init(0.0f); // bogus -> ignored, stays at last valid (192000)
        check("init() ignores a bogus sample rate", core.getSampleRate() == 192000.0f);
        core.init(-1.0f); // bogus -> ignored
        check("init() ignores a negative sample rate", core.getSampleRate() == 192000.0f);
        core.init(999999.0f); // bogus (too high) -> ignored
        check("init() ignores an absurdly high sample rate", core.getSampleRate() == 192000.0f);

        // Exercise the full FINAL setter surface (DESIGN.md §11, 53 params)
        // once each; the point at G0 is that this compiles and doesn't crash,
        // not that it audibly changes anything (process() has no DSP math
        // yet — see synth_core.h's class comment).
        core.setMasterVolumeDb(0.0f);
        core.setOutputClip(false);
        core.setOsc1Wave(SynthCore::Wave::Pulse);
        core.setOsc1Octave(SynthCore::Octave::Ft4);
        core.setOsc1FineCents(25.0f);
        core.setOsc1PwPercent(30.0f);
        core.setOsc1LevelPercent(60.0f);
        core.setOsc2Wave(SynthCore::Wave::Tri);
        core.setOsc2Octave(SynthCore::Octave::Ft2);
        core.setOsc2Semi(7);
        core.setOsc2FineCents(-25.0f);
        core.setOsc2PwPercent(70.0f);
        core.setOsc2LevelPercent(40.0f);
        core.setOsc2Sync(true);
        core.setOsc2KeyTrack(false);
        core.setSubOctave(SynthCore::SubOctave::Minus2);
        core.setSubLevelPercent(50.0f);
        core.setNoiseColor(SynthCore::NoiseColor::Pink);
        core.setNoiseLevelPercent(20.0f);
        core.setEnvFAttackMs(50.0f);
        core.setEnvFDecayMs(300.0f);
        core.setEnvFSustainPercent(60.0f);
        core.setEnvFReleaseMs(500.0f);
        core.setEnvAAttackMs(10.0f);
        core.setEnvADecayMs(600.0f);
        core.setEnvASustainPercent(70.0f);
        core.setEnvAReleaseMs(400.0f);
        core.setLfoWave(SynthCore::LfoWave::SampleHold);
        core.setLfoRateHz(10.0f);
        core.setLfoDelayMs(200.0f);
        core.setLfoPitchAmountPercent(50.0f);
        core.setLfoPwmAmountPercent(50.0f);
        core.setLpfSlope(SynthCore::LpfSlope::Db12);
        core.setLpfCutoffHz(4000.0f);
        core.setLpfResonancePercent(60.0f);
        core.setLpfEnvAmountPercent(-50.0f);
        core.setLpfKeyFollowPercent(80.0f);
        core.setLpfLfoAmountPercent(30.0f);
        core.setDrivePercent(40.0f);
        core.setHpfSlope(SynthCore::HpfSlope::Db24);
        core.setHpfCutoffHz(200.0f);
        core.setHpfKeyFollowPercent(50.0f);
        core.setPmEnvFToOsc2Percent(50.0f);
        core.setPmEnvFToPwPercent(-50.0f);
        core.setPolyphony(SynthCore::Polyphony::Sixteen);
        core.setVoiceMode(SynthCore::VoiceMode::Unison);
        core.setGlideTimeMs(300.0f);
        core.setBendRangeSemitones(12);
        core.setVelToVcaPercent(80.0f);
        core.setVelToFilterPercent(60.0f);
        core.setStereoMode(true);
        core.setStereoDetuneCents(12.0f);
        core.setStereoSpreadPercent(50.0f);
        check("every setter in the FINAL 53-param surface compiles and runs", true);
        static_assert(SynthCore::kNumParams == 53, "DESIGN.md §11 froze this at 53 (R4)");
    }

    // ---------------------------------------------------------------- Silence --
    std::cout << "\nGroup: exact silence (G0.5)\n";
    {
        const int N = 4096;
        std::vector<float> outL(static_cast<size_t>(N), 1.0f); // poisoned, not zeroed
        std::vector<float> outR(static_cast<size_t>(N), 1.0f);

        SynthCore fresh;
        fresh.init(48000.0f);
        fresh.process(nullptr, 0, outL.data(), outR.data(), N);
        bool freshSilent = true;
        for (int i = 0; i < N; ++i) {
            if (outL[static_cast<size_t>(i)] != 0.0f || outR[static_cast<size_t>(i)] != 0.0f)
                freshSilent = false;
        }
        check("process() with no events writes exact silence, fresh instance, 4096 samples",
              freshSilent);

        // Perturb params + control-grid phase, reset(), then check silence again.
        fresh.setMasterVolumeDb(12.0f);
        fresh.setDrivePercent(100.0f);
        std::vector<float> scratchL(17, 0.0f), scratchR(17, 0.0f);
        fresh.process(nullptr, 0, scratchL.data(), scratchR.data(), 17); // leaves mControlPhase == 17
        fresh.reset();
        std::fill(outL.begin(), outL.end(), 1.0f);
        std::fill(outR.begin(), outR.end(), 1.0f);
        fresh.process(nullptr, 0, outL.data(), outR.data(), N);
        bool afterResetSilent = true;
        for (int i = 0; i < N; ++i) {
            if (outL[static_cast<size_t>(i)] != 0.0f || outR[static_cast<size_t>(i)] != 0.0f)
                afterResetSilent = false;
        }
        check("process() with no events writes exact silence after reset(), 4096 samples",
              afterResetSilent);
    }

    // ---------------------------------------------------- Control-rate grid ---
    std::cout << "\nGroup: control-rate grid persistence (DESIGN.md §2, load-bearing for G3.3)\n";
    {
        SynthCore core;
        core.init(48000.0f);
        check("fresh instance: control phase starts at 0", core.getDebugControlPhase() == 0);
        check("default control block is 32 samples (DESIGN.md §2 [PERF-1])",
              core.getDebugControlBlock() == 32);

        std::vector<float> l(4096, 0.0f), r(4096, 0.0f);

        // 10 samples: less than one control block -- phase should simply advance.
        core.process(nullptr, 0, l.data(), r.data(), 10);
        check("phase advances by the exact sample count within one control block",
              core.getDebugControlPhase() == 10);

        // Another 25 samples: 10+25 = 35 = 32 + 3, i.e. one wrap, remainder 3.
        // This is the exact mechanism G3.3 depends on: the grid must NOT
        // restart at the second process() call's own start.
        core.process(nullptr, 0, l.data(), r.data(), 25);
        check("phase persists ACROSS process() calls and wraps correctly at the control-block "
              "boundary (10+25=35 samples -> phase 3, not 25 or 0)",
              core.getDebugControlPhase() == 3);

        // Landing exactly on a boundary must wrap to 0, not mControlBlock.
        core.process(nullptr, 0, l.data(), r.data(), 29); // 3 + 29 = 32
        check("phase wraps to exactly 0 when a call lands exactly on the control-block boundary",
              core.getDebugControlPhase() == 0);

        core.reset();
        check("reset() clears the control-grid phase back to 0", core.getDebugControlPhase() == 0);

        // Multi-wrap in one call: 100 samples with a 32-sample grid -> 100 mod 32 = 4.
        core.process(nullptr, 0, l.data(), r.data(), 100);
        check("a single call spanning multiple control blocks lands at the correct remainder "
              "(100 mod 32 = 4)",
              core.getDebugControlPhase() == 4);

        // setDebugControlBlock (G3.9's test hook): changes the block size and
        // resets phase for consistency.
        core.setDebugControlBlock(1);
        check("setDebugControlBlock(1) is reported back by getDebugControlBlock()",
              core.getDebugControlBlock() == 1);
        check("setDebugControlBlock() resets the grid phase to 0", core.getDebugControlPhase() == 0);
        core.setDebugControlBlock(0); // must clamp to >= 1, not divide by zero / infinite-loop
        check("setDebugControlBlock() clamps a bogus (<=0) size up to 1",
              core.getDebugControlBlock() == 1);
    }

    // ------------------------------------------------------ Reset semantics ---
    std::cout << "\nGroup: reset semantics (G0.7)\n";
    {
        // DESIGN.md §11 "Reset semantics": process a note (well, an event
        // sequence -- there is no audio input to this instrument), reset(),
        // then re-run the IDENTICAL event sequence: output must be
        // bit-identical to a FRESH instance's (also R13's first test). At G0
        // this is necessarily satisfied by construction (process() always
        // writes exact silence, DESIGN.md §11), but the test is written to be
        // the real, meaningful check it becomes from G7 onward, not a
        // silence-only placeholder: it perturbs both parameters AND the
        // control-grid phase before reset(), and compares full buffers.
        const int N = 2048;
        std::vector<NoteEvent> events = {
            {0, NoteEvent::NoteOn, 60, 1.0f},
            {512, NoteEvent::NoteOff, 60, 0.0f},
            {1000, NoteEvent::PitchBend, 0, 0.5f},
            {1500, NoteEvent::AllNotesOff, 0, 0.0f},
        };

        // G6 (R11 finding): reset() clears STATE and never touches PARAMS
        // (DESIGN.md §11 "Reset semantics") -- this AC exists to prove the
        // former, not to re-exercise the latter. Once kMasterVolume/kDrive
        // actually reached the audio path (this gate), `fresh` needs the
        // SAME param values `used` below sets, or a divergence caused
        // purely by DIFFERENT params (fresh at the -6dB/15% defaults, used
        // at 6dB/80%) gets misread as a reset() defect. Latent since G0;
        // invisible until params 0/38 had an audible effect.
        SynthCore fresh;
        fresh.init(48000.0f);
        fresh.setMasterVolumeDb(6.0f);
        fresh.setDrivePercent(80.0f);
        fresh.setPolyphony(SynthCore::Polyphony::Sixteen);
        std::vector<float> freshL(static_cast<size_t>(N)), freshR(static_cast<size_t>(N));
        fresh.process(events.data(), static_cast<int>(events.size()), freshL.data(), freshR.data(),
                      N);

        SynthCore used;
        used.init(48000.0f);
        used.setMasterVolumeDb(6.0f);
        used.setDrivePercent(80.0f);
        used.setPolyphony(SynthCore::Polyphony::Sixteen);
        // Perturb the control-grid phase with an odd-sized block before reset().
        std::vector<float> scratchL(777, 0.0f), scratchR(777, 0.0f);
        used.process(events.data(), static_cast<int>(events.size()), scratchL.data(),
                     scratchR.data(), 777);
        used.reset();
        std::vector<float> usedL(static_cast<size_t>(N)), usedR(static_cast<size_t>(N));
        used.process(events.data(), static_cast<int>(events.size()), usedL.data(), usedR.data(), N);

        bool matches = true;
        for (int i = 0; i < N; ++i) {
            if (freshL[static_cast<size_t>(i)] != usedL[static_cast<size_t>(i)] ||
                freshR[static_cast<size_t>(i)] != usedR[static_cast<size_t>(i)])
                matches = false;
        }
        check("reset() after param writes + a perturbed control-grid phase: replaying the "
              "identical event sequence matches a fresh instance's output bit-exactly",
              matches);
        check("reset() clears the control-grid phase specifically",
              used.getDebugControlPhase() == fresh.getDebugControlPhase());
    }

    // -------------------------------------------- Denormal protection (G11.12) --
    // R11/G11 FINDING: ScopedNoDenormals existed from G1 and was called from
    // NOWHERE for nine gates. Every one-pole here decays exponentially and the
    // output DC blocker keeps running while the instrument is silent, so its
    // tail reaches denormal range and every later sample pays the penalty.
    // Measured before the fix: idle cost 18.7 -> 96.9 ns/sample after 60 s of
    // silence, with no notes played at all.
    //
    // QUANTITY MEASURED: wall-clock cost of processing silence on a FRESH core
    // vs the SAME core after its one-pole tails have had time to go denormal.
    // A ratio, not an absolute -- absolute timings are load-sensitive (which is
    // why the benchmark is not a ctest), but a 5x self-relative blow-up is not
    // something machine load produces. Bound is deliberately loose (2.5x) so
    // this fails only on a real denormal stall, not on scheduling noise.
    std::cout << "\nGroup: denormal protection in the audio path (G11.12)\n";
    {
        const int blk = 512;
        std::vector<float> l(blk), r(blk);
        auto costOf = [&](SynthCore& c) {
            double best = 1e30;
            volatile double sink = 0;
            for (int rep = 0; rep < 5; ++rep) {
                const auto t0 = std::chrono::steady_clock::now();
                double a = 0;
                for (int i = 0; i < 400; ++i) { c.process(nullptr, 0, l.data(), r.data(), blk); a += l[0]; }
                const auto t1 = std::chrono::steady_clock::now();
                sink += a;
                const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / (400.0 * blk);
                if (ns < best) best = ns;
            }
            (void) sink;
            return best;
        };

        SynthCore fresh;
        fresh.init(48000.0f);
        const double costFresh = costOf(fresh);

        SynthCore aged;
        aged.init(48000.0f);
        NoteEvent on{0, NoteEvent::NoteOn, 60, 1.0f};
        NoteEvent off{0, NoteEvent::NoteOff, 60, 0.0f};
        aged.process(&on, 1, l.data(), r.data(), blk);
        for (int i = 0; i < 200; ++i) aged.process(nullptr, 0, l.data(), r.data(), blk);
        aged.process(&off, 1, l.data(), r.data(), blk);
        // 60 s of silence: long enough for a 5 Hz one-pole's tail to reach
        // denormal range from a musical starting amplitude.
        const int silentBlocks = static_cast<int>(60.0 * 48000.0 / blk);
        for (int i = 0; i < silentBlocks; ++i) aged.process(nullptr, 0, l.data(), r.data(), blk);
        const double costAged = costOf(aged);

        const double ratio = costAged / costFresh;
        std::cout << "    fresh " << costFresh << " ns/sample, after 60 s silence "
                  << costAged << " ns/sample, ratio " << ratio << "\n";
        check("G11.12: idle cost after 60 s of silence stays within 2.5x of a fresh core "
              "(without ScopedNoDenormals in process() this measured 5.2x)",
              ratio < 2.5);
    }

    // ------------------------------------------------------- Block-size safety --
    std::cout << "\nGroup: block-size / event-boundary safety (G0.8)\n";
    {
        // docs/GATES.md G0.8's own evidence column is "no crash, all finite"
        // -- NOT silence. At G0 silence was ALSO true (no voice existed), so
        // the original version of this check folded an extra, stronger-than-
        // required assertion in; G3 (docs/GATES.md: "SynthCore::process()
        // produces a real, enveloped tone") makes that extra assertion false
        // by design for any block size long enough for the NoteOn/NoteOff
        // pair below to actually sound, so it is split out here: the
        // numEvents==0 case (still silent, unconditionally, since no notes
        // ever sound) is its own check, and the NoteOn/NoteOff-bearing case
        // is checked against the AC's actual text, finiteness only.
        const int blockSizes[] = {1, 2, 7, 31, 32, 33, 63, 64, 511, 512, 513, 4095, 4096, 8192};
        bool allFinite = true;
        bool emptyEventsStillSilent = true;
        for (int bs : blockSizes) {
            SynthCore core;
            core.init(48000.0f);
            std::vector<float> l(static_cast<size_t>(bs)), r(static_cast<size_t>(bs));

            // Events at offset 0 and at numSamples-1, plus numEvents == 0 on a
            // separate call -- DESIGN.md §10.1's NoteEvent::sampleOffset range
            // is documented as 0..numSamples-1.
            std::vector<NoteEvent> ev = {
                {0, NoteEvent::NoteOn, 60, 1.0f},
                {bs - 1, NoteEvent::NoteOff, 60, 0.0f},
            };
            core.process(ev.data(), static_cast<int>(ev.size()), l.data(), r.data(), bs);
            for (int i = 0; i < bs; ++i) {
                if (!std::isfinite(l[static_cast<size_t>(i)]) || !std::isfinite(r[static_cast<size_t>(i)]))
                    allFinite = false;
            }

            core.process(nullptr, 0, l.data(), r.data(), bs);
            for (int i = 0; i < bs; ++i) {
                if (!std::isfinite(l[static_cast<size_t>(i)]) || !std::isfinite(r[static_cast<size_t>(i)]))
                    allFinite = false;
            }

            // A SEPARATE, fresh instance driven with numEvents==0 ONLY: no
            // note ever sounds, so this must stay exactly silent regardless
            // of block size (G0.5's property, re-checked here per block size).
            SynthCore silentCore;
            silentCore.init(48000.0f);
            std::vector<float> sl(static_cast<size_t>(bs), 1.0f), sr(static_cast<size_t>(bs), 1.0f);
            silentCore.process(nullptr, 0, sl.data(), sr.data(), bs);
            for (int i = 0; i < bs; ++i) {
                if (sl[static_cast<size_t>(i)] != 0.0f || sr[static_cast<size_t>(i)] != 0.0f)
                    emptyEventsStillSilent = false;
            }
        }
        check("process() is safe (no crash, all finite) for every host block size 1..8192 "
              "including the extremes, with numEvents == 0 and with events at offset 0 and "
              "numSamples-1",
              allFinite);
        check("process() with numEvents == 0 stays exactly silent at every block size (G0.5's "
              "property, unaffected by G3 wiring in a real voice)",
              emptyEventsStillSilent);

        // A sequence of odd, non-power-of-two block sizes back-to-back on one
        // instance (exercises the persistent control-grid remainder across
        // many boundaries without crashing).
        SynthCore core;
        core.init(44100.0f);
        const int oddSizes[] = {1, 3, 5, 17, 29, 31, 32, 33, 100, 4001, 8192, 1};
        bool oddOk = true;
        for (int bs : oddSizes) {
            std::vector<float> l(static_cast<size_t>(bs)), r(static_cast<size_t>(bs));
            core.process(nullptr, 0, l.data(), r.data(), bs);
            for (int i = 0; i < bs; ++i) {
                if (!std::isfinite(l[static_cast<size_t>(i)]) || !std::isfinite(r[static_cast<size_t>(i)]))
                    oddOk = false;
            }
        }
        check("a back-to-back sequence of varying, non-aligned block sizes on one instance "
              "does not crash and stays finite",
              oddOk);
    }

    // ------------------------------------------------------- Thread safety ----
    std::cout << "\nGroup: setter thread-safety (G0.9)\n";
    {
        // One thread hammers every setter with varying values while the main
        // thread runs process() in realistic-sized blocks, for ~1 s of audio
        // at 48 kHz. Every setter stores into a std::atomic (R3), so this
        // must run to completion with no crash / UB regardless of
        // interleaving. This is a liveness/no-crash proof, not a magnitude
        // assertion. Mirrors nassau-zermatt/Tests/amp_tests.cpp's G0.9 group.
        //
        // G8 FINDING (R11): the setter thread's own randomisation includes
        // kVoiceMode/kPolyphony/kStereoMode (params 44/45/50, landed by
        // G7/G8), so the worst case this loop can land on -- Unison mode,
        // 16-voice polyphony, kStereoMode on, the SAME NoteOn re-sent every
        // 512-sample block (a fresh full-polyphony Unison retrigger every
        // call) -- now does REAL, doubled per-chain work (DESIGN.md §9),
        // where before G8 kStereoMode was a stored-but-unread atomic. Measured
        // standalone (no contending thread): mono-equivalent worst case 1913
        // iterations/s (521 ns/sample), stereo worst case 1014 iterations/s
        // (983 ns/sample) -- a 1.89x ratio, matching DESIGN.md §9's own
        // "near 1.85x" expectation almost exactly, and nowhere near a stall.
        // Adding the ACTUAL contending setter thread (hammering 53 atomics in
        // a tight loop, no sleep) drops this further, to ~880 iterations/s
        // measured on this box -- BELOW this AC's original `> 1000` bound,
        // which was calibrated before G8 existed and never reasoned about a
        // stereo cost model. This is not a stall or a regression in the sense
        // R1 cares about (no correctness AC moved); it is G8 legitimately
        // making an already-adversarial corner (worst-case polyphony x
        // worst-case voice mode x per-block full retrigger x atomic-hammering
        // contention) cost what DESIGN.md §9 always said it would. The bound
        // is lowered to `> 100` -- roughly 9x below the measured contended
        // worst case on this box, so it still fails hard on an actual
        // deadlock/livelock (which would read single-digit or zero
        // iterations), while no longer being hostage to a performance ratio
        // G11, not G0.9, owns the budget for.
        SynthCore core;
        core.init(48000.0f);

        std::atomic<bool> stop{false};
        std::atomic<uint64_t> setterIterations{0};

        std::thread setterThread([&]() {
            TestXorshift32 rng(0x5EED5EEDu);
            while (!stop.load(std::memory_order_relaxed)) {
                const float v = rng.nextBipolar();
                core.setMasterVolumeDb(v * 36.0f - 24.0f);
                core.setOutputClip((rng.next() & 1u) != 0);
                core.setOsc1Wave(static_cast<SynthCore::Wave>(rng.next() % 3));
                core.setOsc1Octave(static_cast<SynthCore::Octave>(rng.next() % 4));
                core.setOsc1FineCents(v * 50.0f);
                core.setOsc1PwPercent((v + 1.0f) * 47.5f + 5.0f);
                core.setOsc1LevelPercent((v + 1.0f) * 50.0f);
                core.setOsc2Wave(static_cast<SynthCore::Wave>(rng.next() % 3));
                core.setOsc2Octave(static_cast<SynthCore::Octave>(rng.next() % 4));
                core.setOsc2Semi(static_cast<int>(rng.next() % 25) - 12);
                core.setOsc2FineCents(v * 50.0f);
                core.setOsc2PwPercent((v + 1.0f) * 47.5f + 5.0f);
                core.setOsc2LevelPercent((v + 1.0f) * 50.0f);
                core.setOsc2Sync((rng.next() & 1u) != 0);
                core.setOsc2KeyTrack((rng.next() & 1u) != 0);
                core.setSubOctave(static_cast<SynthCore::SubOctave>(rng.next() % 2));
                core.setSubLevelPercent((v + 1.0f) * 50.0f);
                core.setNoiseColor(static_cast<SynthCore::NoiseColor>(rng.next() % 2));
                core.setNoiseLevelPercent((v + 1.0f) * 50.0f);
                core.setEnvFAttackMs((v + 1.0f) * 5000.0f + 1.0f);
                core.setEnvFDecayMs((v + 1.0f) * 5000.0f + 1.0f);
                core.setEnvFSustainPercent((v + 1.0f) * 50.0f);
                core.setEnvFReleaseMs((v + 1.0f) * 5000.0f + 1.0f);
                core.setEnvAAttackMs((v + 1.0f) * 5000.0f + 1.0f);
                core.setEnvADecayMs((v + 1.0f) * 5000.0f + 1.0f);
                core.setEnvASustainPercent((v + 1.0f) * 50.0f);
                core.setEnvAReleaseMs((v + 1.0f) * 5000.0f + 1.0f);
                core.setLfoWave(static_cast<SynthCore::LfoWave>(rng.next() % 5));
                core.setLfoRateHz((v + 1.0f) * 14.975f + 0.05f);
                core.setLfoDelayMs((v + 1.0f) * 1500.0f);
                core.setLfoPitchAmountPercent((v + 1.0f) * 50.0f);
                core.setLfoPwmAmountPercent((v + 1.0f) * 50.0f);
                core.setLpfSlope(static_cast<SynthCore::LpfSlope>(rng.next() % 2));
                core.setLpfCutoffHz((v + 1.0f) * 8990.0f + 20.0f);
                core.setLpfResonancePercent((v + 1.0f) * 50.0f);
                core.setLpfEnvAmountPercent(v * 100.0f);
                core.setLpfKeyFollowPercent((v + 1.0f) * 50.0f);
                core.setLpfLfoAmountPercent((v + 1.0f) * 50.0f);
                core.setDrivePercent((v + 1.0f) * 50.0f);
                core.setHpfSlope(static_cast<SynthCore::HpfSlope>(rng.next() % 2));
                core.setHpfCutoffHz((v + 1.0f) * 990.0f + 20.0f);
                core.setHpfKeyFollowPercent((v + 1.0f) * 50.0f);
                core.setPmEnvFToOsc2Percent(v * 100.0f);
                core.setPmEnvFToPwPercent(v * 100.0f);
                core.setPolyphony(static_cast<SynthCore::Polyphony>(rng.next() % 5));
                core.setVoiceMode(static_cast<SynthCore::VoiceMode>(rng.next() % 3));
                core.setGlideTimeMs((v + 1.0f) * 1000.0f);
                core.setBendRangeSemitones(static_cast<int>(rng.next() % 25));
                core.setVelToVcaPercent((v + 1.0f) * 50.0f);
                core.setVelToFilterPercent((v + 1.0f) * 50.0f);
                core.setStereoMode((rng.next() & 1u) != 0);
                core.setStereoDetuneCents((v + 1.0f) * 12.5f);
                core.setStereoSpreadPercent((v + 1.0f) * 50.0f);
                setterIterations.fetch_add(1, std::memory_order_relaxed);
            }
        });

        // Driven by WALL-CLOCK time, not a sample count -- process() at G0 is
        // far faster than realtime, so a sample-count target would finish in
        // microseconds and never actually overlap the setter thread.
        const int block = 512;
        std::vector<float> l(static_cast<size_t>(block)), r(static_cast<size_t>(block));
        std::vector<NoteEvent> ev = {{0, NoteEvent::NoteOn, 60, 1.0f}};

        const auto t0 = std::chrono::steady_clock::now();
        const auto duration = std::chrono::seconds(1);
        uint64_t processIterations = 0;
        while (std::chrono::steady_clock::now() - t0 < duration) {
            core.process(ev.data(), static_cast<int>(ev.size()), l.data(), r.data(), block);
            ++processIterations;
        }

        stop.store(true, std::memory_order_relaxed);
        setterThread.join();

        check("1 s of process() blocks concurrent with continuous setter calls across all 53 "
              "params completes with no crash",
              true);
        check("process() thread made many iterations over the 1 s window", processIterations > 100);
        check("setter thread actually ran concurrently (made substantial progress)",
              setterIterations.load(std::memory_order_relaxed) > 1000);
    }

    // ------------------------------------------------------- Zero allocation --
    std::cout << "\nGroup: zero heap allocation in process() (G0.11)\n";
    {
        // Land this in G0 rather than G10 (docs/GATES.md's own rationale: an
        // instrument has a dozen places to accidentally allocate -- event
        // vectors, voice lists -- and retrofitting the check finds them all
        // at once, late). 10 s of audio at 48 kHz, busy (non-empty) event
        // stream, everything pre-allocated OUTSIDE the timed region so the
        // delta measures process() alone.
        SynthCore core;
        core.init(48000.0f);

        const int block = 512;
        std::vector<float> l(static_cast<size_t>(block)), r(static_cast<size_t>(block));

        // A "busy" event stream: several events per block, cycling through
        // every NoteEvent::Type, pre-allocated once.
        std::vector<NoteEvent> events;
        for (int i = 0; i < 8; ++i) {
            NoteEvent::Type t;
            switch (i % 6) {
                case 0: t = NoteEvent::NoteOn; break;
                case 1: t = NoteEvent::NoteOff; break;
                case 2: t = NoteEvent::PitchBend; break;
                case 3: t = NoteEvent::Sustain; break;
                case 4: t = NoteEvent::AllNotesOff; break;
                default: t = NoteEvent::AllSoundOff; break;
            }
            events.push_back(NoteEvent{i * (block / 8), t, 40 + i, 0.5f});
        }

        const long long targetSamples = 10 * 48000; // 10 s @ 48 kHz
        const long long numBlocks = targetSamples / block;

        const long long allocBefore = gAllocCount.load(std::memory_order_relaxed);
        const long long deallocBefore = gDeallocCount.load(std::memory_order_relaxed);

        for (long long b = 0; b < numBlocks; ++b) {
            core.process(events.data(), static_cast<int>(events.size()), l.data(), r.data(), block);
        }

        const long long allocAfter = gAllocCount.load(std::memory_order_relaxed);
        const long long deallocAfter = gDeallocCount.load(std::memory_order_relaxed);

        const long long allocDelta = allocAfter - allocBefore;
        const long long deallocDelta = deallocAfter - deallocBefore;

        std::cout << "  (measured: " << numBlocks << " blocks of " << block << " samples = "
                  << (numBlocks * block) << " samples processed; new-count delta = " << allocDelta
                  << ", delete-count delta = " << deallocDelta << ")\n";

        check("zero calls to operator new/new[] during 10 s of process() with a busy event stream",
              allocDelta == 0);
        check("zero calls to operator delete/delete[] during 10 s of process() with a busy event "
              "stream",
              deallocDelta == 0);
    }

    // ------------------------------------------------------------- Summary ----
    std::cout << "\n=== Summary: " << (g_checks - g_failures) << "/" << g_checks
              << " checks passed";
    if (g_failures > 0) {
        std::cout << ", " << g_failures << " FAILED ===\n";
        return 1;
    }
    std::cout << " ===\n";
    return 0;
}
