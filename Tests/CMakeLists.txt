# std::thread needs pthreads on Linux/macOS (no-op on MSVC, which has native
# thread support built in) -- used by synth_tests.cpp's G0.9 setter-thread-
# safety check.
find_package(Threads REQUIRED)

# ---- G0: scaffold + pipeline proof (Source/DSP/synth_core.{h,cpp}) --------
add_executable(synth_tests synth_tests.cpp)
target_link_libraries(synth_tests nassau_synth_core Threads::Threads)
target_include_directories(synth_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../Source/DSP)
target_compile_options(synth_tests PRIVATE ${NASSAU_WARNING_FLAGS})

add_test(NAME SynthTests COMMAND synth_tests)

# ---- G1: DSP primitives (Source/DSP/synth_dsp.h) ---------------------------
# Header-only (no nassau_synth_core link needed) -- dsp_tests.cpp exercises
# synth_dsp.h's structs/free functions directly, using the shared
# Tests/test_util.h harness helpers (docs/GATES.md "Shared test harness").
add_executable(dsp_tests dsp_tests.cpp)
target_include_directories(dsp_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../Source/DSP)
target_compile_options(dsp_tests PRIVATE ${NASSAU_WARNING_FLAGS})

add_test(NAME DspTests COMMAND dsp_tests)

# ---- G2: oscillators, sub, noise, mixer (Source/DSP/synth_osc.h) ----------
# Header-only (no nassau_synth_core link needed) -- osc_tests.cpp exercises
# synth_osc.h's structs directly, same shape as dsp_tests.cpp above (and
# nassau-zermatt's Tests/cabinet_tests.cpp driving Cabinet directly).
add_executable(osc_tests osc_tests.cpp)
target_include_directories(osc_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../Source/DSP)
target_compile_options(osc_tests PRIVATE ${NASSAU_WARNING_FLAGS})

add_test(NAME OscTests COMMAND osc_tests)

# ---- G3: envelopes, LFO, control-rate architecture (Source/DSP/synth_core.*)
# Links nassau_synth_core (unlike dsp_tests/osc_tests): envlfo_tests.cpp
# drives the real SynthCore voice path (NoteEvent in, rendered audio out),
# not synth_dsp.h/synth_osc.h primitives directly.
add_executable(envlfo_tests envlfo_tests.cpp)
target_link_libraries(envlfo_tests nassau_synth_core)
target_include_directories(envlfo_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../Source/DSP)
target_compile_options(envlfo_tests PRIVATE ${NASSAU_WARNING_FLAGS})
# G3.2's grep-based inspection check reads synth_core.cpp's own source text
# (between its "AUDIO-RATE LOOP BEGIN/END" markers) at an ABSOLUTE path, so
# it resolves regardless of ctest's working directory.
target_compile_definitions(envlfo_tests PRIVATE
    NASSAU_CORE_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../Source/DSP/synth_core.cpp")

add_test(NAME EnvLfoTests COMMAND envlfo_tests)

# ---- G4: the low-pass filter (Source/DSP/synth_filter.h) -------------------
# Links nassau_synth_core: G4.1-G4.8/G4.10 drive LadderFilter/SvfFilter
# directly (header-only, same shape as dsp_tests.cpp/osc_tests.cpp), but
# G4.9 reads SynthCore::getDebugLpfCutoff(), so the library is needed too.
add_executable(filter_tests filter_tests.cpp)
target_link_libraries(filter_tests nassau_synth_core)
target_include_directories(filter_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../Source/DSP)
target_compile_options(filter_tests PRIVATE ${NASSAU_WARNING_FLAGS})
# G4.11's grep-based inspection check reads synth_filter.h's own source text
# (between its "PER-SAMPLE PROCESS BEGIN/END" markers) at an ABSOLUTE path,
# matching envlfo_tests.cpp's NASSAU_CORE_CPP_PATH precedent (G3.2).
target_compile_definitions(filter_tests PRIVATE
    NASSAU_FILTER_H_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../Source/DSP/synth_filter.h")

add_test(NAME FilterTests COMMAND filter_tests)

# ---- G5.7: golden-reference parity harness (Tests/synth_golden.cpp) -------
# synth_golden generate|verify [path]. Captures the sound-defining core as
# of G5 (mixer -> DC block -> LPF [+cutoff modulation +slope crossfade] ->
# VCA) into fixtures/golden_g5.bin, checked in. Regenerate with
# `synth_golden generate "<abs path>"` -- a deliberate act (docs/GATES.md
# R8/R13), never a side effect of a normal build/test run (`generate` is
# never invoked by ctest, only `verify` is).
set(SYNTH_GOLDEN_FIXTURE_G5 "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/golden_g5.bin")
add_executable(synth_golden synth_golden.cpp)
target_link_libraries(synth_golden nassau_synth_core)
target_include_directories(synth_golden PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../Source/DSP)
target_compile_options(synth_golden PRIVATE ${NASSAU_WARNING_FLAGS})
target_compile_definitions(synth_golden PRIVATE
    GOLDEN_FIXTURE_PATH_G5="${SYNTH_GOLDEN_FIXTURE_G5}")

add_test(NAME GoldenParityG5 COMMAND synth_golden verify "${SYNTH_GOLDEN_FIXTURE_G5}")

# ---- G6: HPF, drive, Poly-Mod, output stage, complete voice ---------------
# Links nassau_synth_core: the HPF's static-response ACs (G6.1/G6.2/G6.3/
# G6.5/G6.6) drive Source/DSP/synth_filter.h's HpfCascade directly (same
# shape as filter_tests.cpp's G4/G5 precedent); the rest drive the real
# SynthCore per-voice path.
add_executable(voice_tests voice_tests.cpp)
target_link_libraries(voice_tests nassau_synth_core)
target_include_directories(voice_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../Source/DSP)
target_compile_options(voice_tests PRIVATE ${NASSAU_WARNING_FLAGS})

add_test(NAME VoiceTests COMMAND voice_tests)

# ---- G7: voice allocation, MIDI, polyphony, glide, velocity ---------------
# Links nassau_synth_core: the standalone-allocator ACs drive Source/DSP/
# synth_alloc.h's nassau_alloc:: functions directly (header-only, no
# SynthCore needed for THAT group -- same shape as dsp_tests.cpp/
# osc_tests.cpp), but every numbered G7 AC drives the real SynthCore
# per-voice/allocation path (NoteEvent in, rendered audio out).
add_executable(alloc_tests alloc_tests.cpp)
target_link_libraries(alloc_tests nassau_synth_core)
target_include_directories(alloc_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../Source/DSP)
target_compile_options(alloc_tests PRIVATE ${NASSAU_WARNING_FLAGS})

add_test(NAME AllocTests COMMAND alloc_tests)
