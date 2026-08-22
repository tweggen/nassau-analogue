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
