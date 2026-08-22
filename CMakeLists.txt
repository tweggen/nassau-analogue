cmake_minimum_required(VERSION 3.15)
project(NassauAnalogue VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# OBJC/OBJCXX exist only on Apple toolchains and are needed solely by iPlug2's
# macOS plugin bundles — the DSP core and tests are plain C++. Enabling them in
# project() unconditionally breaks configure on Windows/Linux, so enable them
# here instead, from this top-level scope (iPlug2 requires that). Mirrors
# nassau-zermatt/CMakeLists.txt verbatim (DESIGN.md §0.4, docs/GATES.md G0).
if(APPLE)
    enable_language(OBJC)
    enable_language(OBJCXX)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")
endif()

# Portable warning/optimization flags: the GCC/Clang spellings used throughout
# this tree are not understood by MSVC. Subdirectories consume these variables
# instead of hard-coding compiler-specific flags.
if(MSVC)
    set(NASSAU_WARNING_FLAGS /W4)
    # MSVC Release already implies /O2 /Ob2 (CMAKE_CXX_FLAGS_RELEASE) -- this
    # is the portable equivalent of GCC/Clang's -O3 (R7: no -march=native, no
    # -ffast-math; /Oi below is neither, it just lets the compiler inline CRT
    # calls like fabs/sqrt instead of emitting a real call).
    set(NASSAU_RELEASE_OPT_FLAGS /Oi)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

    # Statically link the MSVC runtime (/MT), matching what iPlug2.cmake selects
    # for the plugin targets. This MUST be set before the first add_subdirectory:
    # the variable seeds each target's MSVC_RUNTIME_LIBRARY property at creation
    # time, and Source/DSP is added before the SDK include that pulls in
    # iPlug2.cmake -- so leaving it unset gives the DSP core /MD against the
    # plugin's /MT and the link fails with LNK2038.
    #
    # /MT is also simply correct for a distributed plugin: the .vst3/.clap then
    # carries no dependency on a VC++ redistributable being present on the host.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    set(NASSAU_WARNING_FLAGS -Wall -Wextra -Wpedantic)
    set(NASSAU_RELEASE_OPT_FLAGS -O3)
endif()

# Where to find the shared Nassau plugin SDK. Default: a sibling checkout
# `../nassau-plugin-sdk`. Override with -DNASSAU_SDK_DIR=... .
set(NASSAU_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../nassau-plugin-sdk"
    CACHE PATH "Nassau plugin SDK root")

# DSP core (always builds — no framework dependencies, R2/R14).
add_subdirectory(Source/DSP)

# Plugin bundles (VST3 + AU + CLAP) — only when the SDK module and the vendored
# iPlug2 tree are present, so the DSP core + tests still build on a machine
# without the SDK (G0.2). The SDK's NassauPlugin.cmake pulls in iPlug2's own
# CMake system (find_package(iPlug2)) and must be included from this top-level
# scope so iPlug2 can enable the OBJC/OBJCXX languages.
#
# The SDK supports macOS (VST3 + AU + CLAP) and Windows (VST3 + CLAP); on any
# other platform (this dev box: Linux) we skip the plugin targets rather than
# fail configure, so the DSP core and the full test suite still build and run
# (DESIGN.md §0.4).
if(NOT APPLE AND NOT WIN32)
    message(STATUS "Plugin targets are macOS/Windows-only — building DSP core + tests only")
elseif(EXISTS "${NASSAU_SDK_DIR}/cmake/NassauPlugin.cmake"
       AND EXISTS "${NASSAU_SDK_DIR}/external/iplug2/iPlug2.cmake")
    include(${NASSAU_SDK_DIR}/cmake/NassauPlugin.cmake)
    add_subdirectory(Source/Plugin)
else()
    message(STATUS "Nassau SDK not found at ${NASSAU_SDK_DIR} — skipping plugin targets")
endif()

# Tests (hand-rolled harness, always builds).
enable_testing()
add_subdirectory(Tests)
