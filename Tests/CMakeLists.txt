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

# NOTE: Tests/test_util.h (the shared rms/thdPercent/harmonicDb/... harness
# helpers, docs/GATES.md "Shared test harness") is deliberately NOT created
# here -- it is a G1 deliverable (started from
# nassau-zermatt/Tests/test_util.h, plus the instrument-specific additions
# docs/GATES.md lists). synth_tests.cpp above needs none of it: G0's
# SynthCore::process() writes exact silence, so its own checks are
# silence/finiteness/no-crash/thread-safety/allocation-count assertions with
# no signal-analysis helpers required.
