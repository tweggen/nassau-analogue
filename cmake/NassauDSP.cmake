# NassauDSP.cmake -- the nassau-dsp locator (CANONICAL COPY)
# =============================================================================
# THIS FILE IS COPIED VERBATIM INTO EACH CONSUMER as cmake/NassauDSP.cmake
# (nassau-analogue, nassau-digital). The copy in nassau-dsp/cmake/bootstrap/ is
# the source of truth: change the order here and re-copy, never edit a
# consumer's copy in place. A consumer cannot include() a file from a directory
# it has not found yet, so the finding has to live in the consumer; that is the
# whole reason for a copy, as with nassau-plugin-sdk's NassauSDK.cmake.
#
# -----------------------------------------------------------------------------
# ONE PIN PER REPO (the author's decision of 2026-10-26, NassauDigital DESIGN
# §11). nassau-dsp is pinned exactly once for a release: as a top-level
# submodule of nassau-suite, which passes -DNASSAU_DSP_DIR to every component
# and whose ci/release.sh therefore tags it. No consumer carries it as a nested
# submodule: a second pin is a second answer to which DSP shipped.
#
# RESOLUTION ORDER (one order, no per-repo variation):
#
#   1. An explicit -DNASSAU_DSP_DIR=... (or one already in the cache). This is
#      what the nassau-suite superbuild passes.
#   2. A sibling checkout at ../nassau-dsp, for multi-repo development (and
#      what an IDE opening the workspace directory sees).
#   3. FetchContent from NASSAU_DSP_GIT_REPOSITORY at NASSAU_DSP_GIT_REF, so a
#      bare clone of one consumer still builds. The repo is public and tiny.
#
# Unlike the SDK, finding nothing IS an error: the DSP core of every consumer
# compiles against these headers, so there is no smaller build to fall back to.
# Turn the fetch off with -DNASSAU_DSP_ALLOW_FETCH=OFF to keep a build offline.
#
# The consumer sets NASSAU_DSP_GIT_REF before including this file (a commit
# sha or a tag, never a branch: a branch would make a bare clone build
# different code on different days). Result: the target nassau::dsp, and
# NASSAU_DSP_DIR in the cache.
# =============================================================================

set(NASSAU_DSP_GIT_REPOSITORY "https://github.com/tweggen/nassau-dsp.git"
    CACHE STRING "nassau-dsp git remote, for the FetchContent fallback")
if(NOT NASSAU_DSP_GIT_REF)
    message(FATAL_ERROR
        "NassauDSP.cmake: set NASSAU_DSP_GIT_REF (a commit sha or tag) before "
        "including it.")
endif()

function(_nassau_dsp_is_usable dir out)
    if(dir AND EXISTS "${dir}/CMakeLists.txt" AND EXISTS "${dir}/Source/DSP/synth_dsp.h")
        set(${out} TRUE PARENT_SCOPE)
    else()
        set(${out} FALSE PARENT_SCOPE)
    endif()
endfunction()

set(_nassau_dsp_found FALSE)
set(_nassau_dsp_how "")

# --- 1. explicit ---------------------------------------------------------------
if(NASSAU_DSP_DIR)
    _nassau_dsp_is_usable("${NASSAU_DSP_DIR}" _nassau_dsp_found)
    if(NOT _nassau_dsp_found)
        # An explicit directory that holds nothing is a mistake, not a request
        # for a fallback: the suite asked for THIS checkout.
        message(FATAL_ERROR "nassau-dsp: NASSAU_DSP_DIR=${NASSAU_DSP_DIR} holds no nassau-dsp checkout")
    endif()
    set(_nassau_dsp_how "explicit NASSAU_DSP_DIR")
endif()

# --- 2. sibling checkout -------------------------------------------------------
if(NOT _nassau_dsp_found)
    get_filename_component(_nassau_dsp_sibling "${CMAKE_CURRENT_SOURCE_DIR}/../nassau-dsp" ABSOLUTE)
    _nassau_dsp_is_usable("${_nassau_dsp_sibling}" _nassau_dsp_found)
    if(_nassau_dsp_found)
        set(NASSAU_DSP_DIR "${_nassau_dsp_sibling}" CACHE PATH "nassau-dsp root")
        set(_nassau_dsp_how "sibling checkout")
    endif()
endif()

# --- 3. FetchContent -----------------------------------------------------------
option(NASSAU_DSP_ALLOW_FETCH
    "Allow fetching nassau-dsp from git when no checkout is found" ON)

if(NOT _nassau_dsp_found AND NASSAU_DSP_ALLOW_FETCH)
    include(FetchContent)
    FetchContent_Declare(nassau_dsp_src
        GIT_REPOSITORY "${NASSAU_DSP_GIT_REPOSITORY}"
        GIT_TAG        "${NASSAU_DSP_GIT_REF}")
    # MakeAvailable adds nassau-dsp's own CMakeLists.txt, which defines
    # nassau::dsp and builds its tests only when it is the top-level project.
    FetchContent_MakeAvailable(nassau_dsp_src)
    _nassau_dsp_is_usable("${nassau_dsp_src_SOURCE_DIR}" _nassau_dsp_found)
    if(_nassau_dsp_found)
        set(NASSAU_DSP_DIR "${nassau_dsp_src_SOURCE_DIR}" CACHE PATH "nassau-dsp root" FORCE)
        set(_nassau_dsp_how "fetched ${NASSAU_DSP_GIT_REF}")
    endif()
endif()

if(NOT _nassau_dsp_found)
    message(FATAL_ERROR
        "nassau-dsp not found. Clone it beside this repo:\n"
        "    git clone ${NASSAU_DSP_GIT_REPOSITORY} ../nassau-dsp\n"
        "or pass -DNASSAU_DSP_DIR=<checkout>, or allow the fetch "
        "(-DNASSAU_DSP_ALLOW_FETCH=ON).")
endif()

if(NOT TARGET nassau::dsp)
    add_subdirectory("${NASSAU_DSP_DIR}" "${CMAKE_BINARY_DIR}/_deps/nassau-dsp-build")
endif()

# Say which commit was built, every configure: with three routes the question
# "which DSP is in this build?" must be answered in the log, not guessed.
find_package(Git QUIET)
set(_nassau_dsp_rev "unknown")
if(GIT_FOUND AND EXISTS "${NASSAU_DSP_DIR}/.git")
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${NASSAU_DSP_DIR}" rev-parse --short=12 HEAD
                    OUTPUT_VARIABLE _nassau_dsp_rev OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
message(STATUS "nassau-dsp: ${_nassau_dsp_how}, ${NASSAU_DSP_DIR} at ${_nassau_dsp_rev}")
