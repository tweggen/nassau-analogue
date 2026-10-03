cmake_minimum_required(VERSION 3.15)
project(NassauAnalogue VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Locate the shared Nassau plugin SDK and apply its project prologue: the
# OBJC/OBJCXX enablement, the macOS deployment target, NASSAU_WARNING_FLAGS /
# NASSAU_RELEASE_OPT_FLAGS, and the MSVC /MT runtime selection that keeps the
# DSP core from linking /MD against a /MT plugin (LNK2038).
#
# This replaces ~45 lines that used to live here and, verbatim, in
# nassau-zermatt and nassau-eq -- the duplication the comment at the top of
# this file used to admit to. The reasoning now lives once, in the SDK's
# cmake/NassauPluginProject.cmake. Two constraints that file documents and this
# call site has to honour: it must come BEFORE the first add_subdirectory()
# (CMAKE_MSVC_RUNTIME_LIBRARY seeds each target's property at creation time,
# and Source/DSP is added below), and it must be included from THIS top-level
# scope (enable_language(OBJC) has to happen here for iPlug2).
#
# Resolution order: an explicit -DNASSAU_SDK_DIR=..., then a sibling
# ../nassau-plugin-sdk checkout, then FetchContent. Finding no SDK is not an
# error -- NASSAU_PLUGIN_TARGETS_POSSIBLE goes false and the DSP core and tests
# build anyway (DESIGN.md §0.4, docs/GATES.md G0).
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/NassauSDK.cmake)


# Opt-in single-precision DSP path (DESIGN.md §12.2), OFF by default: the
# double-precision path stays the shipped default and must remain
# bit-identical to every gate through G11 (both golden batteries verify at
# exactly 0.000e+00). Turning this ON redefines `nassau_real`
# (Source/DSP/synth_dsp.h) from `double` to `float` for filter state, filter
# coefficients and the DC blockers ONLY -- oscillator phase accumulators and
# everything else stay `double` unconditionally (G2.8 needs 1e-9, float only
# carries ~1e-7). Targets weak/ARM machines (half-rate double throughput,
# 4-wide NEON); the benefit is REASONED, NOT MEASURED on this x86-64 dev box
# -- see docs/DESIGN.md §12.2 and README.md's build section for the measured
# x86 number. `add_compile_definitions` (directory-wide, inherited by every
# add_subdirectory() below, including Tests' header-only binaries that never
# link nassau_synth_core) is deliberate: every translation unit that sees
# synth_dsp.h must agree on `nassau_real`'s width, or ODR-violating disagreement
# between TUs becomes possible.
option(NASSAU_DSP_FLOAT
    "Use single-precision (float) for DSP filter state/coefficients and DC blockers -- opt-in, weak/ARM targets, default OFF (DESIGN.md §12.2)"
    OFF)
if(NASSAU_DSP_FLOAT)
    add_compile_definitions(NASSAU_DSP_FLOAT)
endif()

# SIMD-over-voices (DESIGN.md §12.3), ON by default on the double-precision
# path: pairs two active voices' (or one stereo voice's own two chains')
# LadderFilter/SvfFilter/HpfCascade/OnePoleHP per-sample processing via SSE2
# 2-wide `double` intrinsics (Source/DSP/synth_simd.h), bit-identical to the
# scalar path it replaces (both golden batteries verify at exactly
# 0.000e+00 with this ON). This flag is the R7-required portable escape
# hatch: with it ON, synth_simd.h's Vec2d compiles to two plain `double`s
# and plain scalar arithmetic instead of intrinsics -- no arch header, no
# -march flag, just ordinary C++ -- which is also what any non-x86 target
# without an intrinsic backend compiles to today (there is no NEON backend
# yet, see synth_simd.h's own header comment). `add_compile_definitions`
# (directory-wide) matches NASSAU_DSP_FLOAT's own convention just above, for
# the same ODR reason.
option(NASSAU_NO_SIMD
    "Disable the SSE2 SIMD-over-voices intrinsics and use the portable two-double scalar fallback instead -- default OFF (DESIGN.md §12.3)"
    OFF)
if(NASSAU_NO_SIMD)
    add_compile_definitions(NASSAU_NO_SIMD)
endif()


# DSP core (always builds — no framework dependencies, R2/R14).
add_subdirectory(Source/DSP)

# Plugin bundles: VST3 + AU + CLAP on macOS, VST3 + CLAP on Windows, none
# elsewhere (this dev box: Linux). NassauPlugin.cmake pulls in iPlug2's own
# CMake system (find_package(iPlug2)) at include time, which is the other
# reason this stays at top-level scope.
if(NASSAU_PLUGIN_TARGETS_POSSIBLE)
    include(${NASSAU_SDK_DIR}/cmake/NassauPlugin.cmake)
    add_subdirectory(Source/Plugin)
endif()

# Tests (hand-rolled harness, always builds).
enable_testing()
add_subdirectory(Tests)
