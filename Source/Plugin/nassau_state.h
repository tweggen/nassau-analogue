#pragma once
// NassauAnalogue state framing / migration helper.
//
// This header is deliberately DEPENDENCY-FREE (no iPlug2 includes) so the pure
// framing/migration math can be unit-tested standalone (G9's StateTests, once
// it lands — see docs/GATES.md). It describes the layout of the versioned
// state chunk NassauAnaloguePlugin writes, and the tolerant rules for reading
// it back. Design and magic copied from nassau-zermatt's own nassau_state.h
// ('NsZm' -> 'NsAn', per docs/GATES.md's G9 deliverable list: "magic
// 'NsAn'"); the framing logic itself is unchanged (it is proven — G7 in
// NassauZermatt, this repo's own G9).
//
// Chunk layout (little-endian, as written by IByteChunk::Put):
//   [0..3]   magic  : 4 bytes, {'N','s','A','n'}
//   [4..7]   uint32 : stateVersion (starts at 1)
//   [8..11]  uint32 : storedParamCount (== NParams() at save time)
//   [12..]   double : storedParamCount parameter values, in enum order
//
// APPEND-ONLY PARAM DISCIPLINE
// ----------------------------
// Params are serialized positionally, so their on-disk meaning is their enum
// index. To stay forward/backward compatible NEW params must be APPENDED at
// the end of the enum (just before kNumParams) and NEVER reordered or
// inserted in the middle (R4). Given that discipline:
//   * An OLDER save (storedCount < currentCount) is read for the params it
//     has; the newer, appended params keep their defaults.
//   * A NEWER save (storedCount > currentCount) loaded by an OLDER build
//     reads the params it understands and SKIPS the tail, never reading past
//     the end.
//
// G0 note: NassauAnaloguePlugin::SerializeState/UnserializeState ARE
// overridden already at G0 (PLUG_DOES_STATE_CHUNKS stays 0 regardless — see
// config.h's own note: iPlug2's "state chunks" flag and a plugin choosing to
// implement versioned state via these two overrides are orthogonal, exactly
// NassauZermatt's stance, copied verbatim). Only 2 of the FINAL 53 params are
// actually registered with a live IParam at G0 (see NassauAnaloguePlugin.h),
// but the framing below only ever operates on `NParams()` (== PLUG_N_PARAMS
// == 53, fixed from G0 — see config.h's own note on why that differs from
// NassauZermatt's incremental growth) and `GetParam(i)->Set(v)`, neither of
// which cares whether index i has been InitXxx()'d yet. PlanUnserialize is
// pure and dependency-free, so nothing here needs to change as later gates
// register more params. G9 is where Tests/state_tests.cpp actually exercises
// this (docs/GATES.md).

#include <cstdint>

namespace nassau_state {

// 4-byte chunk magic: 'N','s','A','n'. Kept as bytes (not a packed int) so
// the on-disk order is unambiguous regardless of host endianness.
inline constexpr unsigned char kMagic[4] = {'N', 's', 'A', 'n'};

// State format version. Bump when the framing (not just an appended param)
// changes. Appending a param does NOT require a version bump thanks to the
// tolerant read below.
inline constexpr uint32_t kStateVersion = 1;

// Size on disk of a single serialized parameter value (iPlug2 stores each
// param as a double via IByteChunk::Put).
inline constexpr int kParamSize = static_cast<int>(sizeof(double));

// Returns true if the 4 bytes at `p` are the NassauAnalogue magic.
inline bool MagicMatches(const unsigned char* p)
{
    return p[0] == kMagic[0] && p[1] == kMagic[1] &&
           p[2] == kMagic[2] && p[3] == kMagic[3];
}

// Pure migration plan for reading a param block.
struct FramePlan {
    int paramsToRead;   // params actually read from the chunk into params[0..)
    int paramsToSkip;   // stored-but-unknown tail params to skip over (advance pos)
    int skipBytes;      // bytes to advance past the skipped tail
    // Any current params with index >= paramsToRead keep their defaults.
};

// Given how many params the chunk stored and how many this build has, decide
// how many to read and how many trailing (unknown, newer) params to skip.
inline FramePlan PlanUnserialize(uint32_t storedCount, uint32_t currentCount)
{
    const uint32_t toRead = storedCount < currentCount ? storedCount : currentCount;
    const uint32_t toSkip = storedCount > currentCount ? (storedCount - currentCount) : 0u;
    FramePlan plan{};
    plan.paramsToRead = static_cast<int>(toRead);
    plan.paramsToSkip = static_cast<int>(toSkip);
    plan.skipBytes    = static_cast<int>(toSkip) * kParamSize;
    return plan;
}

} // namespace nassau_state
