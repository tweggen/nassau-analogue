// NassauAnalogue state-framing unit tests (docs/GATES.md G9.1/G9.2).
//
// Exercises the pure, dependency-free framing/migration helper in
// Source/Plugin/nassau_state.h WITHOUT pulling in iPlug2. We model the
// on-disk chunk with a plain byte buffer and a tiny reader/writer that
// mirror iPlug2's IByteChunk::Put/Get (little-endian raw bytes), then drive
// the same tolerant read logic NassauAnaloguePlugin::UnserializeState uses,
// so the cases the host validators care about are covered standalone:
//   * same-count round-trip (exact),
//   * fewer stored (older save -> extra current params keep defaults),
//   * more stored (newer save -> tail skipped, end position exact, no
//     overread) -- also proves G9.2 (UnserializeState returns the exact end
//     offset, which is what lets a VST3 SetState seek past it),
//   * bad magic -> legacy raw-param fallback.
//
// Design copied verbatim from nassau-zermatt/Tests/state_tests.cpp
// (docs/GATES.md G9's own "copy the shape" instruction), sized here for
// NassauAnalogue's real param count (53, DESIGN.md §11) instead of
// NassauZermatt's 25 -- the framing math is rate-independent of that number,
// but the "fewer stored" / "more stored" migration paths are additionally
// exercised at NassauAnalogue's own real size (G9.1).

#include "nassau_state.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void check(const char* name, bool cond) {
  ++g_checks;
  std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name << "\n";
  if (!cond) ++g_failures;
}

// ---- Minimal byte chunk mirroring iPlug2 IByteChunk semantics --------------
struct Chunk {
  std::vector<uint8_t> bytes;

  void PutBytes(const void* src, int n) {
    const uint8_t* p = static_cast<const uint8_t*>(src);
    bytes.insert(bytes.end(), p, p + n);
  }
  template <class T> void Put(const T* v) { PutBytes(v, sizeof(T)); }

  // Returns new position, or -1 if the read would run past the end (this is
  // the exact overread guard IByteChunk::GetBytes provides).
  int GetBytes(void* dst, int n, int pos) const {
    if (pos < 0 || pos + n > static_cast<int>(bytes.size())) return -1;
    std::memcpy(dst, bytes.data() + pos, n);
    return pos + n;
  }
  template <class T> int Get(T* dst, int pos) const { return GetBytes(dst, sizeof(T), pos); }

  int Size() const { return static_cast<int>(bytes.size()); }
};

// Writes a versioned NassauAnalogue chunk exactly like
// NassauAnaloguePlugin::SerializeState, for a plugin that has `count` params
// with the given values, then appends a trailing sentinel byte to stand in
// for iPlug2's post-state bypass byte (so we can assert the returned end
// position lands exactly on it -- G9.2).
constexpr uint8_t kSentinel = 0xAB;

Chunk WriteState(const std::vector<double>& vals) {
  Chunk c;
  c.PutBytes(nassau_state::kMagic, 4);
  const uint32_t version = nassau_state::kStateVersion;
  const uint32_t count = static_cast<uint32_t>(vals.size());
  c.Put(&version);
  c.Put(&count);
  for (double v : vals) c.Put(&v);
  c.PutBytes(&kSentinel, 1);
  return c;
}

// Mirror of NassauAnaloguePlugin::UnserializeState against the model chunk.
// `params` is the current build's param array, pre-filled with defaults.
// Returns the end position (should point at the sentinel byte) or -1.
int ReadState(const Chunk& c, std::vector<double>& params, bool& usedLegacy) {
  usedLegacy = false;
  unsigned char magic[4] = {0, 0, 0, 0};
  int pos = c.GetBytes(magic, 4, 0);
  if (pos < 0 || !nassau_state::MagicMatches(magic)) {
    // Legacy fallback: read up to params.size() raw doubles from position 0.
    // This is also the path a FACTORY PRESET chunk takes (MakePresetFromChunk/
    // MakePresetFromNamedParams write an unframed, raw positional-double
    // chunk -- see NassauAnaloguePlugin.cpp's preset bank comment) -- not just
    // a hypothetical pre-versioning save.
    usedLegacy = true;
    int p = 0;
    for (size_t i = 0; i < params.size(); ++i) {
      double v = 0.0;
      int np = c.Get(&v, p);
      if (np < 0) break;
      params[i] = v;
      p = np;
    }
    return p;
  }

  uint32_t version = 0, storedCount = 0;
  pos = c.Get(&version, pos);
  pos = c.Get(&storedCount, pos);
  if (pos < 0) return pos;

  const auto plan = nassau_state::PlanUnserialize(
      storedCount, static_cast<uint32_t>(params.size()));

  for (int i = 0; i < plan.paramsToRead && pos >= 0; ++i) {
    double v = 0.0;
    pos = c.Get(&v, pos);
    if (pos >= 0) params[static_cast<size_t>(i)] = v;
  }
  if (pos >= 0 && plan.skipBytes > 0) pos += plan.skipBytes;
  return pos;
}

std::vector<double> Defaults(int n) {
  std::vector<double> v(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) v[static_cast<size_t>(i)] = -1.0 - i;  // distinctive default sentinel values
  return v;
}

}  // namespace

int main() {
  std::cout << "NassauAnalogue state-framing tests\n";

  // NassauAnalogue's real param count as of G9 (DESIGN.md §11, PLUG_N_PARAMS).
  constexpr int kRealParamCount = 53;

  // ---- Pure plan math (at the real 53-param count) --------------------------
  {
    auto p = nassau_state::PlanUnserialize(kRealParamCount, kRealParamCount);
    check("plan: same count reads all, skips none",
          p.paramsToRead == kRealParamCount && p.paramsToSkip == 0 && p.skipBytes == 0);

    auto pf = nassau_state::PlanUnserialize(30, kRealParamCount);
    check("plan: fewer stored reads stored, skips none",
          pf.paramsToRead == 30 && pf.paramsToSkip == 0 && pf.skipBytes == 0);

    auto pm = nassau_state::PlanUnserialize(60, kRealParamCount);
    check("plan: more stored reads current, skips tail",
          pm.paramsToRead == kRealParamCount && pm.paramsToSkip == 7 &&
          pm.skipBytes == 7 * static_cast<int>(sizeof(double)));
  }

  // ---- Case 1: same-count exact round-trip -----------------------------------
  {
    const int n = kRealParamCount;
    std::vector<double> saved(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) saved[static_cast<size_t>(i)] = i * 1.5 + 0.25;
    Chunk c = WriteState(saved);

    std::vector<double> params = Defaults(n);
    bool legacy = false;
    int end = ReadState(c, params, legacy);

    bool match = !legacy && params == saved;
    check("same-count: values round-trip exactly", match);
    check("same-count: end position lands on sentinel byte (G9.2)", end == c.Size() - 1);
    check("same-count: sentinel intact (no overread)", c.bytes[static_cast<size_t>(end)] == kSentinel);
  }

  // ---- Case 2: fewer stored (older save) -> extra params keep defaults ------
  {
    const int stored = 30, current = kRealParamCount;
    std::vector<double> saved(static_cast<size_t>(stored));
    for (int i = 0; i < stored; ++i) saved[static_cast<size_t>(i)] = 100.0 + i;
    Chunk c = WriteState(saved);

    std::vector<double> defaults = Defaults(current);
    std::vector<double> params = defaults;
    bool legacy = false;
    int end = ReadState(c, params, legacy);

    bool storedOk = true;
    for (int i = 0; i < stored; ++i) storedOk &= (params[static_cast<size_t>(i)] == saved[static_cast<size_t>(i)]);
    bool defaultsKept = true;
    for (int i = stored; i < current; ++i)
      defaultsKept &= (params[static_cast<size_t>(i)] == defaults[static_cast<size_t>(i)]);

    check("fewer-stored: stored params restored", storedOk);
    check("fewer-stored: appended params keep defaults", defaultsKept);
    check("fewer-stored: end position lands on sentinel byte", end == c.Size() - 1);
  }

  // ---- Case 3: more stored (newer save) -> tail skipped, no overread --------
  {
    const int stored = 60, current = kRealParamCount;
    std::vector<double> saved(static_cast<size_t>(stored));
    for (int i = 0; i < stored; ++i) saved[static_cast<size_t>(i)] = 200.0 + i;
    Chunk c = WriteState(saved);

    std::vector<double> params = Defaults(current);
    bool legacy = false;
    int end = ReadState(c, params, legacy);

    bool readOk = true;
    for (int i = 0; i < current; ++i) readOk &= (params[static_cast<size_t>(i)] == saved[static_cast<size_t>(i)]);

    check("more-stored: first current params restored", readOk);
    check("more-stored: end position skips tail to sentinel byte (G9.2)", end == c.Size() - 1);
    check("more-stored: sentinel intact (no overread past chunk)",
          end >= 0 && end < c.Size() && c.bytes[static_cast<size_t>(end)] == kSentinel);
  }

  // ---- Case 4: bad magic -> legacy raw-param fallback (also the factory-
  // preset chunk path -- see ReadState's comment above) -----------------------
  {
    const int n = kRealParamCount;
    // Build a raw, UNFRAMED param chunk: just n doubles (no magic/version/count),
    // the exact shape MakePresetFromNamedParams/MakePresetFromChunk produce.
    Chunk c;
    std::vector<double> saved(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) { saved[static_cast<size_t>(i)] = 3.0 + i; c.Put(&saved[static_cast<size_t>(i)]); }

    std::vector<double> params = Defaults(n);
    bool legacy = false;
    int end = ReadState(c, params, legacy);

    check("bad-magic: legacy fallback path taken", legacy);
    check("bad-magic: raw params still restored", params == saved);
    check("bad-magic: end position at end of raw chunk", end == c.Size());
  }

  std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
  return g_failures == 0 ? 0 : 1;
}
