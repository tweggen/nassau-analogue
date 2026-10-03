# NassauSDK.cmake -- the Nassau plugin SDK locator (CANONICAL COPY)
# =============================================================================
# THIS FILE IS COPIED VERBATIM INTO EACH PLUGIN REPO as cmake/NassauSDK.cmake.
# The copy in nassau-plugin-sdk/cmake/bootstrap/ is the source of truth; if you
# need to change the resolution order, change it here and re-copy, do not edit a
# consumer's copy in place.
#
# Why a copy at all, when the whole point of NassauPluginProject.cmake was to
# stop copying things: locating the SDK is irreducible. A plugin repo cannot
# include() a file from a directory it has not found yet, so the finding has to
# live in the consumer. What it must NOT also contain is the toolchain prologue
# -- that is what this file delegates, on its last line.
#
# This file is pure location logic and is expected never to change. The prologue
# it delegates to has changed repeatedly, which is exactly why the two are split.
#
# -----------------------------------------------------------------------------
# RESOLUTION ORDER (one order, three fallbacks, no per-repo variation):
#
#   1. An explicit -DNASSAU_SDK_DIR=... (or a NASSAU_SDK_DIR already in the
#      cache). This is what the nassau-suite superbuild passes, so the suite
#      controls the SDK for a release build.
#   2. A sibling checkout at ../nassau-plugin-sdk, for ordinary multi-repo
#      local development.
#   3. FetchContent from the SDK's git remote at NASSAU_SDK_GIT_REF, so a bare
#      clone of one plugin repo can still build its plugin targets.
#
# Expect to land on 2 while developing and 1 in CI and release builds. 3 is a
# last resort and is slow: the SDK carries the vst3sdk submodule (~190 MB), and
# a private SDK also needs git credentials in the environment.
#
# Finding no SDK at all is NOT an error. The DSP core and the full test suite
# must still build without one -- the DSP-only CI job configures with
# -DNASSAU_SDK_DIR=/nonexistent on purpose -- so an unresolved SDK just clears
# NASSAU_PLUGIN_TARGETS_POSSIBLE and the plugin targets are skipped.
# =============================================================================

# The minimum SDK this repo needs. Raise it when adopting a new SDK feature, so
# a stale sibling checkout fails loudly here instead of mysteriously later.
if(NOT DEFINED NASSAU_SDK_REQUIRED_VERSION)
    set(NASSAU_SDK_REQUIRED_VERSION "0.1.0")
endif()

set(NASSAU_SDK_GIT_REPOSITORY "https://github.com/tweggen/nassau-plugin-sdk.git"
    CACHE STRING "Nassau plugin SDK git remote, for the FetchContent fallback")
# A branch today because the SDK has published no semver tag yet; this becomes
# a tag ("sdk-v0.1.0") the moment it does, and pinning it is the point.
set(NASSAU_SDK_GIT_REF "main"
    CACHE STRING "Nassau plugin SDK git ref for the FetchContent fallback")

function(_nassau_sdk_is_usable dir out)
    if(dir AND EXISTS "${dir}/cmake/NassauPluginProject.cmake")
        set(${out} TRUE PARENT_SCOPE)
        return()
    endif()
    set(${out} FALSE PARENT_SCOPE)
    # Distinguish "no SDK here" from "an SDK that predates this module", which
    # would otherwise degrade silently to a DSP-only build and look like a
    # missing checkout. The version check below cannot catch this: the file that
    # declares NASSAU_SDK_VERSION is the very file that is absent.
    if(dir AND EXISTS "${dir}/cmake/NassauPlugin.cmake")
        message(WARNING
            "Nassau SDK at ${dir} is too old: it has no "
            "cmake/NassauPluginProject.cmake. Update that checkout (it needs "
            "the shared-project-module change) or point NASSAU_SDK_DIR "
            "elsewhere. Building the DSP core and tests only.")
    endif()
endfunction()

# --- 1. explicit ---------------------------------------------------------------
set(_nassau_sdk_found FALSE)
if(NASSAU_SDK_DIR)
    _nassau_sdk_is_usable("${NASSAU_SDK_DIR}" _nassau_sdk_found)
    if(_nassau_sdk_found)
        message(STATUS "Nassau SDK: explicit NASSAU_SDK_DIR=${NASSAU_SDK_DIR}")
    else()
        # Deliberately not a FATAL_ERROR: the DSP-only CI job asks for
        # /nonexistent precisely to get the no-SDK path.
        message(STATUS "Nassau SDK: NASSAU_SDK_DIR=${NASSAU_SDK_DIR} holds no SDK")
    endif()
endif()

# --- 2. sibling checkout -------------------------------------------------------
if(NOT _nassau_sdk_found AND NOT NASSAU_SDK_DIR)
    set(_nassau_sibling "${CMAKE_CURRENT_SOURCE_DIR}/../nassau-plugin-sdk")
    _nassau_sdk_is_usable("${_nassau_sibling}" _nassau_sdk_found)
    if(_nassau_sdk_found)
        get_filename_component(_nassau_sibling "${_nassau_sibling}" ABSOLUTE)
        set(NASSAU_SDK_DIR "${_nassau_sibling}" CACHE PATH "Nassau plugin SDK root")
        message(STATUS "Nassau SDK: sibling checkout ${NASSAU_SDK_DIR}")
    endif()
endif()

# --- 3. FetchContent -----------------------------------------------------------
# Opt-out rather than opt-in: a bare clone should just build. Turn it off with
# -DNASSAU_SDK_ALLOW_FETCH=OFF to keep a build strictly offline.
option(NASSAU_SDK_ALLOW_FETCH
    "Allow fetching the Nassau plugin SDK from git when no checkout is found" ON)

if(NOT _nassau_sdk_found AND NOT NASSAU_SDK_DIR AND NASSAU_SDK_ALLOW_FETCH)
    message(STATUS
        "Nassau SDK: no checkout found -- fetching ${NASSAU_SDK_GIT_REF} from "
        "${NASSAU_SDK_GIT_REPOSITORY} (slow: the SDK carries the VST3 SDK "
        "submodule; set -DNASSAU_SDK_ALLOW_FETCH=OFF to skip)")
    include(FetchContent)
    FetchContent_Declare(nassau_plugin_sdk
        GIT_REPOSITORY "${NASSAU_SDK_GIT_REPOSITORY}"
        GIT_TAG        "${NASSAU_SDK_GIT_REF}"
        GIT_SHALLOW    TRUE
        # The SDK's own submodules (iplug2, vst3sdk) ARE needed -- they are what
        # the plugin targets compile against -- so submodule recursion stays on.
    )
    # MakeAvailable rather than the deprecated bare FetchContent_Populate()
    # (CMake 3.30+ warns, and this tree is built with CMake 4.x). It behaves as
    # populate-only here for a structural reason, not by luck: the SDK has no
    # top-level CMakeLists.txt, so MakeAvailable has nothing to add_subdirectory
    # and stops after populating. That is what we want -- the SDK's modules are
    # include()d, and NassauPlugin.cmake must enter from top-level scope
    # (NassauPluginProject.cmake header, constraint 2).
    FetchContent_MakeAvailable(nassau_plugin_sdk)
    _nassau_sdk_is_usable("${nassau_plugin_sdk_SOURCE_DIR}" _nassau_sdk_found)
    if(_nassau_sdk_found)
        set(NASSAU_SDK_DIR "${nassau_plugin_sdk_SOURCE_DIR}"
            CACHE PATH "Nassau plugin SDK root" FORCE)
        message(STATUS "Nassau SDK: fetched to ${NASSAU_SDK_DIR}")
    endif()
endif()

# --- apply the shared prologue, or stand down ---------------------------------
if(_nassau_sdk_found)
    include(${NASSAU_SDK_DIR}/cmake/NassauPluginProject.cmake)

    if(NASSAU_SDK_VERSION VERSION_LESS NASSAU_SDK_REQUIRED_VERSION)
        message(FATAL_ERROR
            "Nassau SDK at ${NASSAU_SDK_DIR} is version ${NASSAU_SDK_VERSION}, "
            "but this repo needs >= ${NASSAU_SDK_REQUIRED_VERSION}. "
            "Update the SDK checkout.")
    endif()
else()
    # No SDK: no plugin bundles. The DSP core and tests must still build, so
    # they still need the two flag variables -- but note what is deliberately
    # NOT repeated here: the /MT runtime selection and _CRT_SECURE_NO_WARNINGS.
    #
    # /MT exists solely to match what iPlug2.cmake selects for the plugin
    # targets, so that the DSP core does not link /MD against a /MT plugin
    # (LNK2038). With no SDK there are no plugin targets and nothing to match,
    # so the platform default is correct and the reasoning does not need to be
    # duplicated into this file. Keeping that reasoning in exactly one place
    # (NassauPluginProject.cmake) is the whole point of the split.
    set(NASSAU_PLUGIN_TARGETS_POSSIBLE FALSE)
    if(MSVC)
        set(NASSAU_WARNING_FLAGS /W4)
        set(NASSAU_RELEASE_OPT_FLAGS /Oi)
    else()
        set(NASSAU_WARNING_FLAGS -Wall -Wextra -Wpedantic)
        set(NASSAU_RELEASE_OPT_FLAGS -O3)
    endif()
    message(STATUS "Nassau SDK: not found -- DSP core and tests only")
endif()
