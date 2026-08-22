#pragma once

#define PLUG_NAME        "NassauAnalogue"
#define PLUG_MFR         "Nassau"
#define PLUG_VERSION_HEX  0x00000100
#define PLUG_VERSION_STR  "0.1.0"

#define PLUG_URL_STR       "https://github.com/tweggen/nassau-analogue"
#define PLUG_EMAIL_STR     "support@nassau.audio"
#define PLUG_COPYRIGHT_STR "Copyright 2026 Nassau"

#define PLUG_CLASS_NAME   NassauAnaloguePlugin

#define BUNDLE_NAME       "NassauAnalogue"
#define BUNDLE_MFR        "Nassau"
#define BUNDLE_DOMAIN     "com"
// NassauAnaloguePlugin.cpp passes iPlug2's per-format BUNDLE_ID (derived from
// BUNDLE_DOMAIN/BUNDLE_MFR/BUNDLE_NAME) to iplug::Config — not a hand-set id.

// 4-char codes — unique per plugin (AU + some VST3 hosts key on these).
// [voicing] 'Nan1' per docs/GATES.md's own G0 deliverable snippet: MUST
// differ from NassauZermatt's 'Nzm1' (G0.10). PLUG_MFR_ID stays 'Nass' --
// DELIBERATELY the SAME as NassauZermatt's: a manufacturer 4-char code
// identifies the COMPANY (Nassau) across every one of its plugins, the same
// way a real AU/VST3 vendor id does not change per product; only the
// (type, subtype) pair -- PLUG_UNIQUE_ID here -- has to be unique per
// plugin. See this file's own note below on G0.10's wording for why this is
// a deliberate reading, not an oversight.
#define PLUG_UNIQUE_ID    'Nan1'
#define PLUG_MFR_ID       'Nass'

// Fresh 32-hex VST3 UID, unique to NassauAnalogue (generated for G0; different
// from both NassauZermatt's C51E51D5BAFEFB22B5B6E8CFF6BEFB30 and NassauEQ's
// 6DDB946C34D64805A0C496E553B57266 — G0.10).
// [voicing] SHA-256("NassauAnalogue-VST3-UID-2026"), first 32 hex chars,
// uppercased — deterministic and reproducible, not a guessed/random value.
// Derivation (python3): hashlib.sha256(b"NassauAnalogue-VST3-UID-2026")
//   .hexdigest()[:32].upper()
#define PLUG_UID_STR  "09A441BBA33C29E3E04DA501F8D06F21"

// G0.10 NOTE ON PLUG_MFR_ID (found while implementing this gate, R11): the AC
// as literally worded ("PLUG_UNIQUE_ID / PLUG_MFR_ID... AND SO DO") reads as
// requiring BOTH 4-char codes to differ from NassauZermatt's. But this file's
// OWN "Deliverables" config.h snippet in docs/GATES.md's G0 section pins
// PLUG_MFR_ID to 'Nass' — textually IDENTICAL to NassauZermatt's own
// PLUG_MFR_ID. Those two statements conflict. Resolved here in favour of the
// Deliverables snippet (PLUG_MFR_ID = 'Nass', matching Zermatt) because that
// is also the objectively correct AU/VST3 convention: the manufacturer code
// names the COMPANY, is expected to be constant across a vendor's whole
// catalogue (that is the entire point of "manufacturer"), and AU/VST3 tell
// plugins apart by the (type, subtype) pair, not by manufacturer alone —
// which PLUG_UNIQUE_ID ('Nan1' != 'Nzm1') already makes unique. Recorded here
// rather than silently resolved, per R11.

// This is an INSTRUMENT (DESIGN.md §1, §11): two VCOs + sub + noise, no audio
// input at all. Unlike NassauZermatt (mono effect core + a mono-sum 1-1/2-2
// wrapper), SynthCore is natively stereo-out — there is no mono core and no
// mono-sum wrapper here (DESIGN.md §11 "Channel configuration").
#define PLUG_CHANNEL_IO        "0-2"
// No oversampling anywhere in this design (DESIGN.md §2.1, §12) — latency
// stays 0, permanently, unlike NassauZermatt's oversampling-dependent one.
#define PLUG_LATENCY           0
// PLUG_TYPE: 0 = effect (aufx / CLAP audio-effect), 1 = instrument, 2 = MIDI fx.
#define PLUG_TYPE              1
#define PLUG_DOES_MIDI_IN      1
#define PLUG_DOES_MIDI_OUT     0
#define PLUG_DOES_MPE          0
// 0, yet SerializeState/UnserializeState ARE overridden in
// NassauAnaloguePlugin.cpp — copying NassauZermatt's proven stance verbatim
// (nassau_state.h's own file header explains why this is correct: iPlug2's
// "state chunks" flag and a plugin choosing to implement versioned state via
// SerializeState/UnserializeState are orthogonal).
#define PLUG_DOES_STATE_CHUNKS 0

// The FULL 53-param surface (DESIGN.md §11), append-only (R4), landed
// incrementally: kMasterVolume/kOutputClip (G0), Osc1/Osc2/Sub/Noise (G2),
// ENV-F/ENV-A/LFO (G3), LPF slope+cutoff+resonance (G4), LPF modulation (G5),
// HPF+Drive+Poly-Mod (G6), voice/glide/bend/velocity (G7), stereo (G8).
// Frozen here: R4 append-only, and this is the last index in the design.
#define PLUG_N_PARAMS   53
// Factory preset bank. MUST equal kNumPresets in NassauAnaloguePlugin.h (a
// static_assert there enforces it) and the number of entries in
// nassau_presets::Presets() once that lands (G9). G0 has no preset bank yet
// (kNumPresets stays 0 until G9) — see NassauAnaloguePlugin.h's own note.
#define PLUG_N_PRESETS  12

// G0: PLUG_HAS_UI advertises an editor; the actual editor code is gated by
// IPLUG_EDITOR (1 with IGraphics, forced 0 headless), so a forced-headless
// build still links cleanly — it simply reports no editor.
#define PLUG_HAS_UI     1
// G10: sized for the full 13-panel editor (VCO 1 / VCO 2 / Sub+Noise /
// Mixer+Drive / HPF / LPF / ENV-F / ENV-A / LFO / Poly-Mod / Voice / Stereo /
// Output, docs/GATES.md G10.2), covering all 53 params plus the preset
// selector and an IVKeyboardControl strip along the bottom. See
// NassauAnalogueUI.cpp's layout-metrics comment for the exact arithmetic
// this size was derived from (was a 360x180 G0 placeholder covering only
// kMasterVolume/kOutputClip).
#define PLUG_WIDTH      944
#define PLUG_HEIGHT     672
#define PLUG_FPS        60

#define PLUG_SHARED_RESOURCES 0
#define PLUG_HOST_RESIZE 0

// ---- VST3 -------------------------------------------------------------------
#define VST3_SUBCATEGORY "Instrument|Synth"

// ---- AUv2 (AudioComponents) -------------------------------------------------
// aumu (music device), NOT aufx (NassauZermatt is an effect) — DESIGN.md §11,
// docs/GATES.md G0's own note: "the AU Info.plist type must be aumu, not
// aufx". AU subtype == PLUG_UNIQUE_ID ('Nan1'), manufacturer == PLUG_MFR_ID
// ('Nass'). The entry/factory/view names below must match the strings in
// resources/NassauAnalogue-AU-Info.plist (factoryFunction / NSPrincipalClass).
#define AUV2_ENTRY          NassauAnalogue_Entry
#define AUV2_ENTRY_STR      "NassauAnalogue_Entry"
#define AUV2_FACTORY        NassauAnalogue_Factory
#define AUV2_VIEW_CLASS     NassauAnalogue_View
#define AUV2_VIEW_CLASS_STR "NassauAnalogue_View"

// ---- CLAP -------------------------------------------------------------------
// iPlug2 derives the CLAP plugin id from BUNDLE_DOMAIN.BUNDLE_MFR.BUNDLE_NAME
// (-> "com.Nassau.NassauAnalogue"), the name/vendor/url/version from the
// PLUG_* macros above, and the feature list from CLAP_FEATURES below.
#define CLAP_MANUAL_URL  "https://github.com/tweggen/nassau-analogue"
#define CLAP_SUPPORT_URL "https://github.com/tweggen/nassau-analogue/issues"
#define CLAP_DESCRIPTION "Polyphonic 1980-vintage analogue synthesiser"
#define CLAP_FEATURES    "instrument", "synthesizer", "stereo"
