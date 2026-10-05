// G10: full IGraphics editor for NassauAnalogue. Binds EVERY param
// (EParams, NassauAnaloguePlugin.h) by enum index, grouped in signal-chain
// order exactly as docs/GATES.md G10.2 lists it:
//   VCO 1 · VCO 2 · Sub+Noise · Mixer+Drive · HPF · LPF · ENV-F · ENV-A ·
//   LFO · Poly-Mod · Voice · Stereo · Chorus+Output
// (G12 added kChorus to the last of those panels -- see its own note in the
// layout-metrics block for the two pixels of editor height that cost)
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
// has bound it -- G10.1 requires ALL of them to be bound, and the most reliable
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
    //
    // GetHPadded, NOT GetPadded: the gutter between neighbouring cells is
    // wanted, but the 4px that GetPadded also takes off the TOP and BOTTOM is
    // not. IVectorBase::MakeRects hands the label its measured text height
    // off the top and the value its measured text height off the bottom, and
    // whatever survives is the knob -- so every vertical pixel spent on
    // padding comes straight out of the knob's diameter. The row heights
    // below are budgeted exactly (label + knob + value); padding them again
    // here is what starved the widget in the first place.
    void KnobRow(const IRECT& row, const IVStyle& style,
                 std::initializer_list<std::pair<int, const char*>> knobs)
    {
        const int n = static_cast<int>(knobs.size());
        int i = 0;
        for (const auto& kv : knobs) {
            Knob(row.GetGridCell(i, 1, n).GetHPadded(-4.f), kv.first, kv.second, style);
            ++i;
        }
    }

    // Evenly-spaced toggles across `row` (VCO2's Sync/KeyTrack, and single-
    // toggle rows like Stereo/Output). Horizontal-only padding, for the same
    // reason as KnobRow: an IVToggleControl draws its "Off"/"On" INSIDE the
    // button (IVSwitchControl's valueInButton is hard-coded true for it), so
    // any height taken off the row is height taken off the box that text has
    // to fit in.
    void ToggleRow(const IRECT& row, const IVStyle& style,
                   std::initializer_list<std::pair<int, const char*>> toggles)
    {
        const int n = static_cast<int>(toggles.size());
        int i = 0;
        for (const auto& kv : toggles) {
            Toggle(row.GetGridCell(i, 1, n).GetHPadded(-4.f), kv.first, kv.second, style);
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
    // EVERY dimension in the editor derives from this block, and the block is
    // constexpr precisely so the two static_asserts at the bottom of it can
    // check the derived editor size against PLUG_WIDTH/PLUG_HEIGHT in
    // config.h. Those asserts are the whole point: the previous version of
    // this file carried the same arithmetic as a COMMENT, the comment drifted
    // from the code, and the result was an editor whose content did not fit
    // its own window. A comment cannot fail a build; a static_assert can.
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
    // ---- how the VERTICAL budget is derived (the thing that was wrong) ----
    //
    // IVectorBase::MakeRects (IControl.h) carves every IVControl's bounds up
    // like this, and the row heights below are budgeted to match it exactly:
    //
    //   * the LABEL takes MeasureText(labelText).H() off the TOP;
    //   * the VALUE takes MeasureText(valueText).H() off the BOTTOM -- but
    //     ONLY when the value text's EVAlign is Bottom. At EVAlign::Middle it
    //     takes nothing and is drawn straight ON TOP of the widget instead;
    //   * whatever is left is the widget, and for the button-ish controls
    //     (IVSwitchControl and therefore IVTabSwitchControl/IVToggleControl,
    //     which pass hasHandle=true) GetAdjustedHandleBounds then removes
    //     another frameThickness + shadowOffset = kHandleInset off the height.
    //
    // Two consequences drove this rewrite:
    //
    // 1. iPlug2's own DEFAULT_VALUE_TEXT is {DEFAULT_TEXT_SIZE, Bottom}, but
    //    the IText(size, color, fontID) constructor defaults valign to
    //    MIDDLE. Every IText here is built with that constructor, so every
    //    knob silently got a Middle-aligned value -- i.e. the value string
    //    painted across the middle of the knob face, over the pointer. It
    //    survived review on Windows because Segoe UI is narrow enough at 10pt
    //    that "-5.2 cents" stayed inside the knob and read as deliberate; in
    //    macOS Helvetica the same string is ~10% wider, spills past the knob
    //    on both sides, and reads as broken. Fixed by spelling the alignment
    //    out (kKnobValueText below) rather than relying on a default.
    //
    // 2. The switch/toggle rows were 20px, then 30px, and both were too short
    //    to contain their own text: at 30px an IVToggleControl kept 30 - 4
    //    (this file's old GetPadded(-4)) - 10 (label) - 4 (handle inset) = 12
    //    for a widget that has to hold an 11pt "Off"/"On" -- so the value was
    //    drawn clipped in half. The heights below are computed from the font
    //    sizes instead of guessed, and the row helpers pad horizontally only.
    //
    // MeasureText's returned HEIGHT is the em box, not the glyph extent, so
    // it comes back as exactly the point size -- MEASURED on macOS/Skia, for
    // every size used here (10, 11, 12, 17), which is what lets the vertical
    // budget below be constexpr at all. The Windows build goes through the
    // same Skia backend, so the same should hold there; if a future Windows
    // screenshot ever shows vertical clipping, that assumption is the first
    // thing to re-measure. Text WIDTH is emphatically NOT portable (Helvetica
    // runs ~10% wider than Segoe UI at the same size), which is exactly why
    // the fix for (1) is a layout change and not a "drop the font a point on
    // macOS" fudge.
    constexpr float kMargin     = 8.f;
    constexpr float kHeaderH    = 32.f;
    constexpr float kGap        = 4.f;    // vertical gap inside a panel
    constexpr float kColGap     = 8.f;    // horizontal gap between panels
    constexpr float kRowGap     = 8.f;    // vertical gap between bands
    constexpr float kKnobColW   = 64.f;   // nominal per-knob-column width; feeds the width assert
    constexpr float kKeyboardH  = 80.f;   // bottom keyboard strip

    // Text point sizes. Named, because the row heights below are computed
    // from them -- change a size and the layout follows, instead of the two
    // silently drifting apart.
    constexpr float kTitleTextSz = 12.f;  // panel caption
    constexpr float kKnobLabelSz = 11.f;
    constexpr float kKnobValueSz = 10.f;
    constexpr float kSwLabelSz   = 10.f;  // tab switch + toggle label
    constexpr float kSwValueSz   = 10.f;  // tab captions
    constexpr float kTogValueSz  = 11.f;  // toggle "Off"/"On"

    // Height MakeRects hands to GetAdjustedHandleBounds and never gives back:
    // frameThickness (1, halved off each edge) + shadowOffset (3, off the
    // bottom only) with iPlug2's DEFAULT_STYLE. Applies to the button-shaped
    // controls only -- IVKnobControl does not pass hasHandle.
    constexpr float kHandleInset = 4.f;

    // The clear height each widget must keep AFTER label/value/inset, chosen
    // so the text inside it is never clipped and the knob stays a knob.
    constexpr float kTabH   = 20.f;   // tab-caption box in a tab switch
    constexpr float kTogH   = 22.f;   // button box in a toggle
    constexpr float kKnobD  = 45.f;   // knob diameter

    constexpr float kPanelTitle = kTitleTextSz + 6.f;                        // 18
    constexpr float kSwitchH    = kSwLabelSz + kHandleInset + kTabH;         // 34
    constexpr float kToggleH    = kSwLabelSz + kHandleInset + kTogH;         // 36
    constexpr float kRowH       = kKnobLabelSz + kKnobD + kKnobValueSz;      // 66

    // Row heights: each set by its own tallest panel's internal stack --
    // row 1 by VCO 2 (title + 2 switches + knob row + toggle row), row 2 by
    // LPF (title + switch + 2 knob rows), row 3 by Chorus+Output.
    //
    // G12 CHANGED WHICH PANEL SETS ROW 3. It used to be Voice (title + 2
    // switches + knob row = 160), and Output had 36px of slack under its
    // toggle + knob stack. A chorus tab switch needs kSwitchH + kGap = 38,
    // i.e. exactly TWO PIXELS more than that slack -- so rather than shave
    // something to make it fit (which is how the content came to overflow
    // its window before this block existed at all), row 3 is now budgeted
    // from the taller Output stack and PLUG_HEIGHT follows it, 712 -> 714.
    // Voice's 160 still fits inside the 162 with room to spare.
    constexpr float row1H = kPanelTitle + 2.f * (kSwitchH + kGap) + kRowH + kGap + kToggleH;
    constexpr float row2H = kPanelTitle + (kSwitchH + kGap) + kRowH + kGap + kRowH;
    constexpr float row3H = kPanelTitle + (kSwitchH + kGap) + kToggleH + kGap + kRowH;
    static_assert(row3H >= kPanelTitle + 2.f * (kSwitchH + kGap) + kRowH,
                  "row 3 must still contain the Voice panel's own title + 2 switches + knob row");

    // ...and the editor size those add up to. If either assert fires, change
    // PLUG_WIDTH/PLUG_HEIGHT in config.h to the number the compiler names --
    // do not "fix" it by shaving a row, which is how the content came to
    // overflow its window in the first place.
    constexpr float kEditorW = kKnobColW * 14.f + kColGap * 4.f + 2.f * kMargin;
    constexpr float kEditorH = kHeaderH
                             + kRowGap + row1H
                             + kRowGap + row2H
                             + kRowGap + row3H
                             + kRowGap + kKeyboardH
                             + 2.f * kMargin;
    static_assert(kEditorW == static_cast<float>(PLUG_WIDTH),
                  "PLUG_WIDTH does not match the width this layout needs");
    static_assert(kEditorH == static_cast<float>(PLUG_HEIGHT),
                  "PLUG_HEIGHT does not match the height this layout needs");

    const IText header(17, kText, "default", EAlign::Near);
    const IText brand(12, kAccent, "default", EAlign::Far);
    const IText colHdr(kTitleTextSz, kAccent, "default", EAlign::Center);

    // EVAlign::Bottom, spelled out: see note (1) in the metrics block above.
    // This is what puts the value UNDER the knob instead of across its face.
    const IText kKnobValueText(kKnobValueSz, kText, "default", EAlign::Center, EVAlign::Bottom);

    const IVStyle knobStyle = DEFAULT_STYLE
        .WithColor(kFG, kAccent)
        .WithLabelText(IText(kKnobLabelSz, kText, "default"))
        .WithValueText(kKnobValueText)
        .WithDrawFrame(false)
        .WithShowValue(true);

    // The switch/toggle values are drawn INSIDE the widget -- IVSwitchControl
    // hard-codes valueInButton, and overrides the valign to Middle itself --
    // so their valign here is irrelevant and only the box height matters.
    const IVStyle switchStyle = DEFAULT_STYLE
        .WithColor(kFG, kAccent)
        .WithColor(kBG, kPanel)
        .WithLabelText(IText(kSwLabelSz, kText, "default"))
        .WithValueText(IText(kSwValueSz, kText, "default"))
        .WithDrawFrame(true);

    const IVStyle toggleStyle = DEFAULT_STYLE
        .WithColor(kFG, kAccent)
        .WithValueText(IText(kTogValueSz, kText, "default"))
        .WithLabelText(IText(kSwLabelSz, kText, "default"));

    ui.AttachPanelBackground(kBg);

    // --- Header row: title (left), preset selector (centre), brand (right) --
    ui.AttachControl(new ITextControl(
        IRECT(kMargin + 2.f, kMargin, 160.f, kMargin + kHeaderH), "NassauAnalogue", header));

    const IVStyle presetStyle = DEFAULT_STYLE
        .WithDrawShadows(false)
        .WithColor(kFG, kAccent)
        .WithColor(kBG, kPanel)
        .WithLabelText(IText(11, kText, "default", EAlign::Center, EVAlign::Middle))
        .WithValueText(IText(11, kText, "default"));
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
    const PanelSpec kRow3[] = { { "LFO", 4.f }, { "POLY-MOD", 2.f }, { "VOICE", 4.f }, { "STEREO", 2.f }, { "CHORUS+OUTPUT", 2.f } };

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
        bind.ToggleRow(IRECT(c.L, y, c.R, y + kToggleH), toggleStyle,
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
        bind.ToggleRow(IRECT(c.L, y, c.R, y + kToggleH), toggleStyle, { { kStereoMode, "Stereo" } });
        y += kToggleH + kGap;
        bind.KnobRow(IRECT(c.L, y, c.R, y + kRowH), knobStyle,
                     { { kStereoDetune, "Detune" }, { kStereoSpread, "Spread" } });
    }

    // --- Chorus + Output (cols[12]) ---------------------------------------
    // The chorus lives HERE, at the top of the output panel, because that is
    // where it lives in the signal chain (DESIGN.md §1/§13: on the summed
    // accumulator, ahead of master volume) -- reading the panel top to bottom
    // reads the output stage in order. A 4-position tab switch, not a knob
    // and not four toggles: the instrument it models has two buttons whose
    // four states ARE these four positions, and there is nothing continuous
    // to dial.
    {
        const IRECT& c = cols[12];
        float y = c.T + kPanelTitle;
        bind.Switch(IRECT(c.L + 3.f, y, c.R - 3.f, y + kSwitchH), kChorus, "Chorus", switchStyle);
        y += kSwitchH + kGap;
        bind.ToggleRow(IRECT(c.L, y, c.R, y + kToggleH), toggleStyle, { { kOutputClip, "Clip" } });
        y += kToggleH + kGap;
        bind.KnobRow(IRECT(c.L, y, c.R, y + kRowH), knobStyle, { { kMasterVolume, "Volume" } });
    }

    // Every param must be bound by now (G10.1) -- this is the
    // assertion the count is checked by: Binder::Mark flips one bit per
    // successful bind and refuses a second bind of the same index, so if
    // this holds, all kNumParams indices were each bound exactly once.
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
