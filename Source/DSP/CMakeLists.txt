add_library(nassau_synth_core
    synth_core.cpp
    synth_core.h
)

target_include_directories(nassau_synth_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(nassau_synth_core PRIVATE ${NASSAU_WARNING_FLAGS})

# G0: build the DSP core at -O3 in Release (MSVC Release already implies /O2,
# plus /Oi -- see NASSAU_RELEASE_OPT_FLAGS's own comment in the root
# CMakeLists.txt). Portable only -- NO fast-math (fast-math is unsafe for the
# recursive IIR / feedback-loop math landing in later gates, R7).
target_compile_options(nassau_synth_core PRIVATE
    $<$<CONFIG:Release>:${NASSAU_RELEASE_OPT_FLAGS}>)
