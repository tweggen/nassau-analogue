// G10: full IGraphics editor for NassauAnalogue. Binds all 53 params
// (EParams, NassauAnaloguePlugin.h) by enum index, grouped in signal-chain
// order exactly as docs/GATES.md G10.2 lists it:
//   VCO 1 · VCO 2 · Sub+Noise · Mixer+Drive · HPF · LPF · ENV-F · ENV-A ·
//   LFO · Poly-Mod · Voice · Stereo · Output
// plus the factory preset selector (IVBakedPresetManagerControl) and an
// IVKeyboardControl so the instrument can be auditioned by ear without
// external MIDI (G10.3).
//
// The ENTIRE body is wrapped in #if IPLUG_EDITOR so that a headless build
// (IPLUG_EDITOR == 0, e.g. NASSAU_FORCE_HEADLESS) compiles this file to
// nothing and needs no IGraphics include paths (G10.4).
//
// Look and idiom are lifted deliberately from nassau-zermatt's proven G8
// editor (Source/Plugin/NassauZermattUI.cpp in that sibling repo): same
// palette-variable names and values (this product's own amber-on-warm-brown
// palette, per the G10 brief), same IVStyle construction, same "empty
// options list -> IVTabSwitchControl reads InitEnum's own labels" idiom, and
// the SAME font-loading routine, copied verbatim including its comment,
// because it fixes two real bugs that shipped in Zermatt G8 (see below).
#if IPLUG_EDITOR

#include "NassauAnalogueUI.h"
#include "NassauAnaloguePlugin.h"
#include "IGraphics.h"
#include "IControls.h"

#include <array>
#include <cassert>

using namespace iplug;
using namespace iplug::igraphics;

namespace {

// Tracks, per enum-index param, whether some control in this Layout() call
// has bound it -- G10.1 requires ALL 53 to be bound, and the most reliable
// way to make that true is to make it impossible to walk away from this
// function with one missed: every bind goes through Binder, which flips a
// bit, and Layout() asserts the whole array is set (and, in a debug build,
// that nothing was bound twice -- a silent duplicate is exactly the kind of
// off-by-one a copy/paste layout like this one is prone to) before
// returning.
struct Binder {
    IGraphics& ui;
    std::array<bool, kNumParams>& bound;

    void Mark(int paramIdx)
    {
        assert(paramIdx >= 0 && paramIdx < kNumParams && "param index out of range");
        assert(!bound[static_cast<size_t>(paramIdx)] && "param bound to more than one control");
        bound[static_cast<size_t>(paramIdx)] = true;
    }

    void Knob(const IRECT& r, int paramIdx, const char* label, const IVStyle& style)
    {
        Mark(paramIdx);
        ui.AttachControl(new IVKnobControl(r, paramIdx, label, style));
    }

    // Full-width enum switch. Empty options list -> IVTabSwitchControl pulls
    // its display labels from GetParam(paramIdx)->InitEnum(...) in
    // NassauAnaloguePlugin.cpp, so the UI text can never drift from the
    // parameter definition (same idiom as NassauZermattUI.cpp).
    void Switch(const IRECT& r, int paramIdx, const char* label, const IVStyle& style)
    {
        Mark(paramIdx);
        ui.AttachControl(new IVTabSwitchControl(r, paramIdx, std::vector<const char*>{}, label, style));
    }

    void Toggle(const IRECT& r, int paramIdx, const char* label, const IVStyle& style)
    {
        Mark(paramIdx);
        ui.AttachControl(new IVToggleControl(r, paramIdx, label, style, "Off", "On"));
    }

    // Evenly-spaced knobs across `row`.
    void KnobRow(const IRECT& row, const IVStyle& style,
                 std::initializer_list<std::pair<int, const char*>> knobs)
    {
        const int n = static_cast<int>(knobs.size());
        int i = 0;
        for (const auto& kv : knobs) {
            Knob(row.GetGridCell(i, 1, n).GetPadded(-4.f), kv.first, kv.second, style);
            ++i;
        }
    }

    // Evenly-spaced toggles across `row` (VCO2's Sync/KeyTrack, and single-
    // toggle rows like Stereo/Output).
    void ToggleRow(const IRECT& row, const IVStyle& style,
                   std::initializer_list<std::pair<int, const char*>> toggles)
    {
        const int n = static_cast<int>(toggles.size());
        int i = 0;
        for (const auto& kv : toggles) {
            Toggle(row.GetGridCell(i, 1, n).GetPadded(-4.f), kv.first, kv.second, style);
            ++i;
        }
    }
};

} // namespace

void NassauAnalogueUI::Layout(IGraphics& ui, NassauAnaloguePlugin& /*plugin*/)
{
    // Register the UI font under the id "default", by FAMILY NAME, on EVERY
    // editor open.
    //
    // Two bugs lived here and both shipped in Zermatt G8; they are worth
    // spelling out again because the pattern below is a verbatim copy of the
    // now-proven fix (docs/GATES.md G10.5 exists specifically because of
    // this history):
    //
    // 1. "Helvetica" is a macOS font and does NOT exist on Windows. LoadFont
    //    returned false, so every IText below referenced a font id that was
    //    never registered, and the editor drew NOTHING — an empty window at
    //    the right size. The user builds and views THIS plugin on Windows,
    //    so the Windows branch below is the one that actually matters.
    //
    // 2. It was guarded by a `static bool` so it ran "exactly once". But
    //    IGraphics is destroyed and recreated every time the editor is
    //    opened, and its font storage goes with it — while a process-lifetime
    //    static persists. So the SECOND and every later editor open skipped
    //    the load and came up with no font at all. That is the classic
    //    "works every other time" symptom. The load is cheap and idempotent;
    //    just do it every time — there is NO static guard here.
    //
    // iPlug2's DEFAULT_FONT ("Roboto-Regular") is only a constant — IGraphics
    // does not auto-load it — so there is no free fallback to lean on. Try a
    // list, most-preferred first, and keep the first that actually loads.
    const char* const kFontFamilies[] = {
#ifdef OS_WIN
        "Segoe UI", "Arial", "Tahoma", "Verdana"
#else
        "Helvetica", "Arial"
#endif
    };
    bool fontLoaded = false;
    for (const char* family : kFontFamilies) {
        if (ui.LoadFont("default", family, ETextStyle::Normal)) {
            fontLoaded = true;
            break;
        }
    }
    assert(fontLoaded && "No UI font could be loaded — all text will be invisible");

    // Warm amber-on-dark palette, lifted verbatim from NassauZermatt per the
    // G10 brief -- this is a sibling product and should look like one.
    const IColor kBg(255, 27, 24, 22);       // near-black warm brown
    const IColor kPanel(255, 40, 36, 32);    // panel fill
    const IColor kText(255, 226, 218, 205);  // warm off-white
    const IColor kAccent(255, 212, 150, 64); // amber

    // ---- Layout metrics -------------------------------------------------
    // EVERY dimension in the editor derives from this block, so the whole
    // panel can be resized by changing these numbers plus PLUG_WIDTH/
    // PLUG_HEIGHT in config.h.
    //
    // Panels are organised into three content rows, in exactly the group
    // order docs/GATES.md G10.2 lists, read left-to-right then top-to-
    // bottom:
    //   row 1 (sound sources):  VCO 1 · VCO 2 · Sub+Noise · Mixer+Drive
    //   row 2 (filters):        HPF · LPF · ENV-F · ENV-A
    //   row 3 (mod/performance): LFO · Poly-Mod · Voice · Stereo · Output
    // plus a keyboard strip spanning the full width below row 3.
    //
    // Each panel's WEIGHT below is the number of knob/switch "columns" its
    // widest internal row needs (e.g. Mixer+Drive's widest row is Osc1/Osc2/
    // Sub level = 3 knobs, so weight 3); row widths are then normalised to
    // fill the same contentW, exactly as NassauZermattUI.cpp does. The widest
    // row overall is row 3, at 14 weight units (LFO 4 + Poly-Mod 2 + Voice 4
    // + Stereo 2 + Output 2) across 5 panels (4 inter-panel gaps). Sizing
    // PLUG_WIDTH off THAT row keeps knobs roughly a comfortable width
    // everywhere; rows with fewer weight units simply get a little more
    // breathing room per knob, which is a fine trade, not a defect.
    //
    //   kKnobColW * 14 + kColGap * 4  =  64*14 + 8*4      =  896 + 32 = 928
    //   PLUG_WIDTH = contentW + 2*kMargin                 =  928 + 16 = 944
    //
    // Height is the sum of the three content rows' own heights (each row's
    // height is set by its OWN tallest panel -- e.g. row 1 is set by VCO 2:
    // title + 2 switch rows + 1 knob row + 1 toggle row), plus the header
    // and keyboard strip:
    //   row1 (VCO2 tallest): 16 + (20+4)*2 + 56 + 4 + 20  = 144
    //   row2 (LPF tallest):  16 + 20+4 + 56+4 + 56         = 156
    //   row3 (Voice tallest):16 + (20+4)*2 + 56            = 120
    //   header 32, keyboard 90, 4x kRowGap(8) between the five bands
    //   32 + 8+144 + 8+156 + 8+120 + 8+90 = 574
    //   PLUG_HEIGHT = 574 + 2*kMargin(8) = 590
    //
    // (53 params plus a keyboard needs considerably more room than Zermatt's
    // 552x390 six-panel/25-param layout -- this is roughly 2.4x the area.)
    const float kMargin     = 8.f;
    const float kHeaderH    = 32.f;
    const float kPanelTitle = 16.f;   // panel caption strip
    const float kRowH       = 56.f;   // one knob row (knob + label + value)
    const float kSwitchH    = 20.f;   // one tab-switch / toggle row
    const float kGap        = 4.f;    // vertical gap inside a panel
    const float kColGap     = 8.f;    // horizontal gap between panels
    const float kRowGap     = 8.f;    // vertical gap between bands
    // kKnobColW (64px, the nominal per-knob-column width) is not read back
    // here -- it only fed the PLUG_WIDTH arithmetic above at design time,
    // the same way it is not an input to Zermatt's own placeRow() weighting
    // either. Actual per-column widths come from normalising each row's
    // panel weights to contentW below, exactly as NassauZermattUI.cpp does.
    const float kKeyboardH  = 90.f;   // bottom keyboard strip

    const IText header(16, kText, "default", EAlign::Near);
    const IText brand(11, kAccent, "default", EAlign::Far);
    const IText colHdr(11, kAccent, "default", EAlign::Center);

    const IVStyle knobStyle = DEFAULT_STYLE
        .WithColor(kFG, kAccent)
        .WithLabelText(IText(10, kText, "default"))
        .WithValueText(IText(9, kText, "default"))
        .WithDrawFrame(false)
        .WithShowValue(true);

    const IVStyle switchStyle = DEFAULT_STYLE
        .WithColor(kFG, kAccent)
        .WithColor(kBG, kPanel)
        .WithLabelText(IText(9, kText, "default"))
        .WithValueText(IText(9, kText, "default"))
        .WithDrawFrame(true);

    const IVStyle toggleStyle = DEFAULT_STYLE
        .WithColor(kFG, kAccent)
        .WithValueText(IText(10, kText, "default"))
        .WithLabelText(IText(9, kText, "default"));

    ui.AttachPanelBackground(kBg);

    // --- Header row: title (left), preset selector (centre), brand (right) --
    ui.AttachControl(new ITextControl(
        IRECT(kMargin + 2.f, kMargin, 160.f, kMargin + kHeaderH), "NassauAnalogue", header));

    const IVStyle presetStyle = DEFAULT_STYLE
        .WithDrawShadows(false)
        .WithColor(kFG, kAccent)
        .WithColor(kBG, kPanel)
        .WithLabelText(IText(10, kText, "default", EAlign::Center, EVAlign::Middle))
        .WithValueText(IText(10, kText, "default"));
    // Factory ("baked") preset manager: [<] preset-name menu [>], auto-
    // populated from the plugin's MakePreset* bank; arrowing/selecting calls
    // RestorePreset. G10.2's "plus the preset selector" requirement.
    ui.AttachControl(new IVBakedPresetManagerControl(
        IRECT(166.f, kMargin + 2.f, static_cast<float>(PLUG_WIDTH) - 66.f,
              kMargin + kHeaderH - 2.f), "", presetStyle));

    ui.AttachControl(new ITextControl(
        IRECT(static_cast<float>(PLUG_WIDTH) - 62.f, kMargin,
              static_cast<float>(PLUG_WIDTH) - kMargin, kMargin + kHeaderH), "Nassau", brand));

    // --- Panel geometry ------------------------------------------------------
    const float contentW = static_cast<float>(PLUG_WIDTH) - 2.f * kMargin;

    struct PanelSpec { const char* name; float weight; };
    // Row weights are each panel's widest internal knob/switch row (see the
    // metrics comment above).
    const PanelSpec kRow1[] = { { "VCO 1", 2.f }, { "VCO 2", 3.f }, { "SUB+NOISE", 2.f }, { "MIXER+DRIVE", 3.f } };
    const PanelSpec kRow2[] = { { "HPF", 2.f }, { "LPF", 3.f }, { "ENV-F", 4.f }, { "ENV-A", 4.f } };
    const PanelSpec kRow3[] = { { "LFO", 4.f }, { "POLY-MOD", 2.f }, { "VOICE", 4.f }, { "STEREO", 2.f }, { "OUTPUT", 2.f } };

    // Row heights: each set by its own tallest panel's internal stack (see
    // the metrics comment above for the arithmetic).
    const float row1H = kPanelTitle + 2.f * (kSwitchH + kGap) + kRowH + kGap + kSwitchH; // VCO 2
    const float row2H = kPanelTitle + kSwitchH + kGap + kRowH + kGap + kRowH;             // LPF
    const float row3H = kPanelTitle + 2.f * (kSwitchH + kGap) + kRowH;                    // Voice

    IRECT cols[13];
    {
        auto placeRow = [&](const PanelSpec* row, int n, float y, float h, int outBase) {
            float wsum = 0.f;
            for (int i = 0; i < n; ++i) wsum += row[i].weight;
            const float usable = contentW - static_cast<float>(n - 1) * kColGap;
            float x = kMargin;
            for (int i = 0; i < n; ++i) {
                const float w = usable * row[i].weight / wsum;
                cols[outBase + i] = IRECT(x, y, x + w, y + h);
                x += w + kColGap;
            }
        };
        const float y1 = kMargin + kHeaderH + kRowGap;
        const float y2 = y1 + row1H + kRowGap;
        const float y3 = y2 + row2H + kRowGap;
        placeRow(kRow1, 4, y1, row1H, 0);
        placeRow(kRow2, 4, y2, row2H, 4);
        placeRow(kRow3, 5, y3, row3H, 8);
    }

    const PanelSpec* const kAllSpecs[13] = {
        &kRow1[0], &kRow1[1], &kRow1[2], &kRow1[3],
        &kRow2[0], &kRow2[1], &kRow2[2], &kRow2[3],
        &kRow3[0], &kRow3[1], &kRow3[2], &kRow3[3], &kRow3[4],
    };
    for (int i = 0; i < 13; ++i) {
        ui.AttachControl(new IPanelControl(cols[i].GetPadded(-2.f), kPanel));
        ui.AttachControl(new ITextControl(
            IRECT(cols[i].L, cols[i].T + 2.f, cols[i].R, cols[i].T + kPanelTitle),
            kAllSpecs[i]->name, colHdr));
    }

    std::array<bool, kNumParams> bound{};
    Binder bind{ ui, bound };

    // --- VCO 1 (cols[0]) -------------------------------------------------
    {
        const IRECT& c = cols[0];
        float y = c.T + kPanelTitle;
        bind.Switch(IRECT(c.L + 3.f, y, c.R - 3.f, y + kSwitchH), kOsc1Wave, "Osc1 Wave", switchStyle);
        y += kSwitchH + kGap;
        bind.Switch(IRECT(c.L + 3.f, y, c.R - 3.f, y + kSwitchH), kOsc1Octave, "Osc1 Range", switchStyle);
        y += kSwitchH + kGap;
        bind.KnobRow(IRECT(c.L, y, c.R, y + kRowH), knobStyle,
                     { { kOsc1Fine, "Fine" }, { kOsc1PW, "PW" } });
    }

    // --- VCO 2 (cols[1]) -------------------------------------------------
    {
        const IRECT& c = cols[1];
        float y = c.T + kPanelTitle;
        bind.Switch(IRECT(c.L + 3.f, y, c.R - 3.f, y + kSwitchH), kOsc2Wave, "Osc2 Wave", switchStyle);
        y += kSwitchH + kGap;
        bind.Switch(IRECT(c.L + 3.f, y, c.R - 3.f, y + kSwitchH), kOsc2Octave, "Osc2 Range", switchStyle);
        y += kSwitchH + kGap;
        bind.KnobRow(IRECT(c.L, y, c.R, y + kRowH), knobStyle,
                     { { kOsc2Semi, "Semi" }, { kOsc2Fine, "Fine" }, { kOsc2PW, "PW" } });
        y += kRowH + kGap;
        bind.ToggleRow(IRECT(c.L, y, c.R, y + kSwitchH), toggleStyle,
                       { { kOsc2Sync, "Sync" }, { kOsc2KeyTrack, "KeyTrk" } });
    }

    // --- Sub+Noise (cols[2]) ----------------------------------------------
    {
        const IRECT& c = cols[2];
        float y = c.T + kPanelTitle;
        bind.Switch(IRECT(c.L + 3.f, y, c.R - 3.f, y + kSwitchH), kSubOctave, "Sub Range", switchStyle);
        y += kSwitchH + kGap;
        bind.Switch(IRECT(c.L + 3.f, y, c.R - 3.f, y + kSwitchH), kNoiseColor, "Noise Color", switchStyle);
    }

    // --- Mixer+Drive (cols[3]) ---------------------------------------------
    {
        const IRECT& c = cols[3];
        float y = c.T + kPanelTitle;
        bind.KnobRow(IRECT(c.L, y, c.R, y + kRowH), knobStyle,
                     { { kOsc1Level, "Osc1" }, { kOsc2Level, "Osc2" }, { kSubLevel, "Sub" } });
        y += kRowH + kGap;
        bind.KnobRow(IRECT(c.L, y, c.R, y + kRowH), knobStyle,
                     { { kNoiseLevel, "Noise" }, { kDrive, "Drive" } });
    }

    // --- HPF (cols[4]) -------------------------------------------------------
    {
        const IRECT& c = cols[4];
        float y = c.T + kPanelTitle;
        bind.Switch(IRECT(c.L + 3.f, y, c.R - 3.f, y + kSwitchH), kHpfSlope, "HPF Slope", switchStyle);
        y += kSwitchH + kGap;
        bind.KnobRow(IRECT(c.L, y, c.R, y + kRowH), knobStyle,
                     { { kHpfCutoff, "Cutoff" }, { kHpfKeyFollow, "KeyTrk" } });
    }

    // --- LPF (cols[5]) -------------------------------------------------------
    {
        const IRECT& c = cols[5];
        float y = c.T + kPanelTitle;
        bind.Switch(IRECT(c.L + 3.f, y, c.R - 3.f, y + kSwitchH), kLpfSlope, "LPF Slope", switchStyle);
        y += kSwitchH + kGap;
        bind.KnobRow(IRECT(c.L, y, c.R, y + kRowH), knobStyle,
                     { { kLpfCutoff, "Cutoff" }, { kLpfResonance, "Reso" }, { kLpfEnvAmount, "Env Amt" } });
        y += kRowH + kGap;
        bind.KnobRow(IRECT(c.L, y, c.R, y + kRowH), knobStyle,
                     { { kLpfKeyFollow, "KeyTrk" }, { kLpfLfoAmount, "LFO Amt" } });
    }

    // --- ENV-F (cols[6]) ------------------------------------------------------
    {
        const IRECT& c = cols[6];
        bind.KnobRow(IRECT(c.L, c.T + kPanelTitle, c.R, c.T + kPanelTitle + kRowH), knobStyle,
                     { { kEnvFAttack, "Attack" }, { kEnvFDecay, "Decay" },
                       { kEnvFSustain, "Sustain" }, { kEnvFRelease, "Release" } });
    }

    // --- ENV-A (cols[7]) ------------------------------------------------------
    {
        const IRECT& c = cols[7];
        bind.KnobRow(IRECT(c.L, c.T + kPanelTitle, c.R, c.T + kPanelTitle + kRowH), knobStyle,
                     { { kEnvAAttack, "Attack" }, { kEnvADecay, "Decay" },
                       { kEnvASustain, "Sustain" }, { kEnvARelease, "Release" } });
    }

    // --- LFO (cols[8]) ---------------------------------------------------------
    {
        const IRECT& c = cols[8];
        float y = c.T + kPanelTitle;
        bind.Switch(IRECT(c.L + 3.f, y, c.R - 3.f, y + kSwitchH), kLfoWave, "LFO Wave", switchStyle);
        y += kSwitchH + kGap;
        bind.KnobRow(IRECT(c.L, y, c.R, y + kRowH), knobStyle,
                     { { kLfoRate, "Rate" }, { kLfoDelay, "Delay" },
                       { kLfoPitchAmount, "Pitch" }, { kLfoPwmAmount, "PWM" } });
    }

    // --- Poly-Mod (cols[9]) -----------------------------------------------
    {
        const IRECT& c = cols[9];
        bind.KnobRow(IRECT(c.L, c.T + kPanelTitle, c.R, c.T + kPanelTitle + kRowH), knobStyle,
                     { { kPmEnvFToOsc2, "EnvF>Osc2" }, { kPmEnvFToPw, "EnvF>PW" } });
    }

    // --- Voice (cols[10]) -------------------------------------------------
    {
        const IRECT& c = cols[10];
        float y = c.T + kPanelTitle;
        bind.Switch(IRECT(c.L + 3.f, y, c.R - 3.f, y + kSwitchH), kPolyphony, "Polyphony", switchStyle);
        y += kSwitchH + kGap;
        bind.Switch(IRECT(c.L + 3.f, y, c.R - 3.f, y + kSwitchH), kVoiceMode, "Voice Mode", switchStyle);
        y += kSwitchH + kGap;
        bind.KnobRow(IRECT(c.L, y, c.R, y + kRowH), knobStyle,
                     { { kGlideTime, "Glide" }, { kBendRange, "Bend" },
                       { kVelToVca, "Vel>VCA" }, { kVelToFilter, "Vel>Filt" } });
    }

    // --- Stereo (cols[11]) -----------------------------------------------
    {
        const IRECT& c = cols[11];
        float y = c.T + kPanelTitle;
        bind.ToggleRow(IRECT(c.L, y, c.R, y + kSwitchH), toggleStyle, { { kStereoMode, "Stereo" } });
        y += kSwitchH + kGap;
        bind.KnobRow(IRECT(c.L, y, c.R, y + kRowH), knobStyle,
                     { { kStereoDetune, "Detune" }, { kStereoSpread, "Spread" } });
    }

    // --- Output (cols[12]) -----------------------------------------------
    {
        const IRECT& c = cols[12];
        float y = c.T + kPanelTitle;
        bind.ToggleRow(IRECT(c.L, y, c.R, y + kSwitchH), toggleStyle, { { kOutputClip, "Clip" } });
        y += kSwitchH + kGap;
        bind.KnobRow(IRECT(c.L, y, c.R, y + kRowH), knobStyle, { { kMasterVolume, "Volume" } });
    }

    // Every one of the 53 params must be bound by now (G10.1) -- this is the
    // assertion the count is checked by: Binder::Mark flips one bit per
    // successful bind and refuses a second bind of the same index, so if
    // this holds, all 53 indices were each bound exactly once.
    for (int i = 0; i < kNumParams; ++i) {
        assert(bound[static_cast<size_t>(i)] && "a param was left unbound by the G10 layout");
    }

    // --- Keyboard strip, full width, below the three panel rows -----------
    // G10.3: an instrument editor without a keyboard is untestable by ear
    // without external MIDI.
    //
    // SIGNATURE VERIFIED against the pinned iPlug2 commit
    // 7dfe7a96db9b5a9cd5e322569db4a53cf243030a (the SHA in
    // nassau-plugin-sdk/.gitmodules), IGraphics/Controls/IVKeyboardControl.h:
    //
    //   IVKeyboardControl(const IRECT& bounds, int minNote = 48,
    //                     int maxNote = 72, bool roundedKeys = false,
    //                     const IColor& WK_COLOR, BK_COLOR, PK_COLOR,
    //                     FR_COLOR, HK_COLOR)
    //
    // and IControls.h includes IVKeyboardControl.h at line 25, so no extra
    // include is needed. NassauZermatt is an effect and has no keyboard, so
    // this is the one control here with no in-house call site to copy.
    //
    // Colours are passed explicitly rather than left at their defaults: the
    // stock keyboard is a generic white/black that sits badly against this
    // panel's warm amber-on-near-black palette. Pressed keys use the same
    // accent as every knob's indicator, so a held note reads as "lit" in the
    // same visual language as the rest of the editor.
    const float kbY = cols[8].T + row3H + kRowGap; // just below row 3
    const IRECT kbRect(kMargin, kbY, static_cast<float>(PLUG_WIDTH) - kMargin, kbY + kKeyboardH);
    constexpr int kKeyboardMinNote = 36; // C2
    constexpr int kKeyboardMaxNote = 96; // C7 -- 5 octaves
    const IColor kKeyWhite(255, 206, 198, 186);  // warm off-white, a touch below kText
    const IColor kKeyBlack(255, 22, 20, 18);     // slightly darker than kBg
    const IColor kKeyPressed = kAccent;          // same amber as every knob indicator
    const IColor kKeyFrame = kPanel;
    const IColor kKeyHover(255, 236, 190, 130);  // lifted accent
    ui.AttachControl(new IVKeyboardControl(kbRect, kKeyboardMinNote, kKeyboardMaxNote,
                                            /*roundedKeys=*/false, kKeyWhite, kKeyBlack,
                                            kKeyPressed, kKeyFrame, kKeyHover));
}

#endif // IPLUG_EDITOR
