// G10: full IGraphics editor for NassauAnalogue. Binds EVERY param
// (EParams, NassauAnaloguePlugin.h) by enum index, grouped in signal-chain
// order exactly as docs/GATES.md G10.2 lists it:
//   VCO 1 · VCO 2 · Sub+Noise · Mixer+Drive · HPF · LPF · ENV-F · ENV-A ·
//   LFO · Poly-Mod · Voice · Stereo · Chorus+Output
// (G12 added kChorus to the last of those panels -- see the row-3 note below
// for the two pixels of editor height that cost)
// plus the factory preset selector (IVBakedPresetManagerControl) and an
// IVKeyboardControl so the instrument can be auditioned by ear without
// external MIDI (G10.3).
//
// Built from nassau-plugin-sdk's shared editor style (include/nassau/
// ui_style.h, specified in docs/UI_STYLE.md): the metrics, text sizes,
// control styles, font loading (G10.5), header, panels, row helpers and the
// binding check are the SDK's, and UI_STYLE.md says why each of them is what
// it is. Most of those numbers were first worked out in this file (G10/G12);
// what stays here is what is analogue's own: its warm amber-on-dark palette,
// its panels and their contents, its keyboard strip, and its window size.
//
// The ENTIRE body is wrapped in #if IPLUG_EDITOR so that a headless build
// (IPLUG_EDITOR == 0, e.g. NASSAU_FORCE_HEADLESS) compiles this file to
// nothing and needs no IGraphics include paths (G10.4).
#if IPLUG_EDITOR

#include "NassauAnalogueUI.h"
#include "NassauAnaloguePlugin.h"
#include "nassau/ui_style.h"

using namespace iplug;
using namespace iplug::igraphics;
namespace nui = nassau::ui;

void NassauAnalogueUI::Layout(IGraphics& ui, NassauAnaloguePlugin& /*plugin*/)
{
    // Every open, by family name, from a per-platform list (G10.5,
    // UI_STYLE.md §4).
    nui::LoadFont(ui);

    // Warm amber-on-dark palette, the same values as before the shared style:
    // this product's own colours. knobFace is the one new colour, a dark warm
    // brown a step above the panel, so the knob faces and the unselected
    // switch segments are dark and the amber is the value arc / the selected
    // segment (UI_STYLE.md §3).
    const nui::Palette pal{
        IColor(255, 27, 24, 22),     // bg: near-black warm brown
        IColor(255, 40, 36, 32),     // panel
        IColor(255, 226, 218, 205),  // text: warm off-white
        IColor(255, 150, 140, 126),  // textDim
        IColor(255, 212, 150, 64),   // accent: amber
        IColor(255, 70, 58, 46),     // knobFace: dark warm brown
    };

    // ---- Size: derived, and checked against config.h ----------------------
    // The metrics are the SDK's (UI_STYLE.md §2). The row heights and the
    // editor size they add up to are this layout's, constexpr so the
    // static_asserts below can hold PLUG_WIDTH/PLUG_HEIGHT to them: a size
    // carried as a comment once drifted from the code, and the content
    // overflowed its own window. A comment cannot fail a build.
    //
    // Three content rows, in G10.2's order, left to right then top to bottom:
    //   row 1 (sound sources):   VCO 1 · VCO 2 · Sub+Noise · Mixer+Drive
    //   row 2 (filters):         HPF · LPF · ENV-F · ENV-A
    //   row 3 (mod/performance): LFO · Poly-Mod · Voice · Stereo · Chorus+Output
    // plus a keyboard strip spanning the full width below row 3.
    //
    // Each panel's weight is the number of knob/switch columns its widest
    // internal row needs (Mixer+Drive's widest is Osc1/Osc2/Sub = 3). The
    // widest row overall is row 3, 14 units (LFO 4 + Poly-Mod 2 + Voice 4 +
    // Stereo 2 + Output 2) across 5 panels, so PLUG_WIDTH is sized off it;
    // rows with fewer units get a little more room per knob.
    constexpr float kKeyboardH = 80.f;  // bottom keyboard strip

    // Row heights: each set by its own tallest panel's stack -- row 1 by
    // VCO 2 (title + 2 switches + knob row + toggle row), row 2 by LPF (title
    // + switch + 2 knob rows), row 3 by Chorus+Output.
    //
    // G12 CHANGED WHICH PANEL SETS ROW 3. It used to be Voice (title + 2
    // switches + knob row = 160), and Output had 36px of slack under its
    // toggle + knob stack. A chorus tab switch needs kSwitchH + kGap = 38,
    // i.e. exactly TWO PIXELS more than that slack -- so rather than shave
    // something to make it fit (which is how the content came to overflow its
    // window before this block existed at all), row 3 is budgeted from the
    // taller Output stack and PLUG_HEIGHT follows it, 712 -> 714. Voice's 160
    // still fits inside the 162 with room to spare.
    constexpr float row1H = nui::kPanelTitleH + 2.f * (nui::kSwitchH + nui::kGap) + nui::kKnobRowH
                          + nui::kGap + nui::kToggleH;
    constexpr float row2H = nui::kPanelTitleH + (nui::kSwitchH + nui::kGap) + nui::kKnobRowH
                          + nui::kGap + nui::kKnobRowH;
    constexpr float row3H = nui::kPanelTitleH + (nui::kSwitchH + nui::kGap) + nui::kToggleH
                          + nui::kGap + nui::kKnobRowH;
    static_assert(row3H >= nui::kPanelTitleH + 2.f * (nui::kSwitchH + nui::kGap) + nui::kKnobRowH,
                  "row 3 must still contain the Voice panel's own title + 2 switches + knob row");

    // If either assert fires, change PLUG_WIDTH/PLUG_HEIGHT in config.h to
    // the number the layout needs -- do not "fix" it by shaving a row.
    constexpr float kEditorW = nui::kKnobColW * 14.f + nui::kColGap * 4.f + 2.f * nui::kMargin;
    constexpr float kEditorH = nui::kHeaderH
                             + nui::kRowGap + row1H
                             + nui::kRowGap + row2H
                             + nui::kRowGap + row3H
                             + nui::kRowGap + kKeyboardH
                             + 2.f * nui::kMargin;
    static_assert(kEditorW == static_cast<float>(PLUG_WIDTH),
                  "PLUG_WIDTH does not match the width this layout needs");
    static_assert(kEditorH == static_cast<float>(PLUG_HEIGHT),
                  "PLUG_HEIGHT does not match the height this layout needs");

    // Every switch and toggle goes through nui::Switch / nui::ToggleRow, i.e.
    // nui::TabSwitch / nui::Toggle, never the bare iPlug2 classes: those draw
    // the selected segment's and the "on" text in the ink that reads on the
    // amber (here the dark knobFace; the off-white text read at ~1.9:1).
    const IVStyle knob = nui::KnobStyle(pal);
    const IVStyle sw = nui::SwitchStyle(pal);
    const IVStyle tog = nui::ToggleStyle(pal);

    ui.AttachPanelBackground(pal.bg);

    // Header: name, the factory ("baked") preset manager -- [<] menu [>],
    // populated from the MakePreset* bank, selecting calls RestorePreset
    // (G10.2's preset selector) -- and the brand.
    nui::Header(ui, pal, "NassauAnalogue", kEditorW);

    // --- Panel geometry ------------------------------------------------------
    const float x0 = nui::kMargin, x1 = kEditorW - nui::kMargin;
    const float y1 = nui::kMargin + nui::kHeaderH + nui::kRowGap;
    const float y2 = y1 + row1H + nui::kRowGap;
    const float y3 = y2 + row2H + nui::kRowGap;
    const auto row1 = nui::PanelRow(IRECT(x0, y1, x1, y1 + row1H), { 2.f, 3.f, 2.f, 3.f });
    const auto row2 = nui::PanelRow(IRECT(x0, y2, x1, y2 + row2H), { 2.f, 3.f, 4.f, 4.f });
    const auto row3 = nui::PanelRow(IRECT(x0, y3, x1, y3 + row3H), { 4.f, 2.f, 4.f, 2.f, 2.f });

    // Rows inside a panel's content area, top to bottom.
    auto knobRow = [](const IRECT& a, float y) { return IRECT(a.L, y, a.R, y + nui::kKnobRowH); };
    auto switchRow = [](const IRECT& a, float y) { return IRECT(a.L, y, a.R, y + nui::kSwitchH); };
    auto toggleRow = [](const IRECT& a, float y) { return IRECT(a.L, y, a.R, y + nui::kToggleH); };
    constexpr float kSwStep = nui::kSwitchH + nui::kGap;
    constexpr float kKnobStep = nui::kKnobRowH + nui::kGap;
    constexpr float kTogStep = nui::kToggleH + nui::kGap;

    // --- VCO 1 -----------------------------------------------------------------
    {
        const IRECT a = nui::Panel(ui, pal, row1[0], "VCO 1");
        float y = a.T;
        nui::Switch(ui, switchRow(a, y), kOsc1Wave, "Osc1 Wave", sw);
        y += kSwStep;
        nui::Switch(ui, switchRow(a, y), kOsc1Octave, "Osc1 Range", sw);
        y += kSwStep;
        nui::KnobRow(ui, knobRow(a, y), knob, { { kOsc1Fine, "Fine" }, { kOsc1PW, "PW" } });
    }

    // --- VCO 2 -----------------------------------------------------------------
    {
        const IRECT a = nui::Panel(ui, pal, row1[1], "VCO 2");
        float y = a.T;
        nui::Switch(ui, switchRow(a, y), kOsc2Wave, "Osc2 Wave", sw);
        y += kSwStep;
        nui::Switch(ui, switchRow(a, y), kOsc2Octave, "Osc2 Range", sw);
        y += kSwStep;
        nui::KnobRow(ui, knobRow(a, y), knob,
                     { { kOsc2Semi, "Semi" }, { kOsc2Fine, "Fine" }, { kOsc2PW, "PW" } });
        y += kKnobStep;
        nui::ToggleRow(ui, toggleRow(a, y), tog, { { kOsc2Sync, "Sync" }, { kOsc2KeyTrack, "KeyTrk" } });
    }

    // --- Sub+Noise -------------------------------------------------------------
    {
        const IRECT a = nui::Panel(ui, pal, row1[2], "SUB+NOISE");
        float y = a.T;
        nui::Switch(ui, switchRow(a, y), kSubOctave, "Sub Range", sw);
        y += kSwStep;
        nui::Switch(ui, switchRow(a, y), kNoiseColor, "Noise Color", sw);
    }

    // --- Mixer+Drive -----------------------------------------------------------
    {
        const IRECT a = nui::Panel(ui, pal, row1[3], "MIXER+DRIVE");
        float y = a.T;
        nui::KnobRow(ui, knobRow(a, y), knob,
                     { { kOsc1Level, "Osc1" }, { kOsc2Level, "Osc2" }, { kSubLevel, "Sub" } });
        y += kKnobStep;
        nui::KnobRow(ui, knobRow(a, y), knob, { { kNoiseLevel, "Noise" }, { kDrive, "Drive" } });
    }

    // --- HPF -------------------------------------------------------------------
    {
        const IRECT a = nui::Panel(ui, pal, row2[0], "HPF");
        float y = a.T;
        nui::Switch(ui, switchRow(a, y), kHpfSlope, "HPF Slope", sw);
        y += kSwStep;
        nui::KnobRow(ui, knobRow(a, y), knob, { { kHpfCutoff, "Cutoff" }, { kHpfKeyFollow, "KeyTrk" } });
    }

    // --- LPF -------------------------------------------------------------------
    {
        const IRECT a = nui::Panel(ui, pal, row2[1], "LPF");
        float y = a.T;
        nui::Switch(ui, switchRow(a, y), kLpfSlope, "LPF Slope", sw);
        y += kSwStep;
        nui::KnobRow(ui, knobRow(a, y), knob,
                     { { kLpfCutoff, "Cutoff" }, { kLpfResonance, "Reso" }, { kLpfEnvAmount, "Env Amt" } });
        y += kKnobStep;
        nui::KnobRow(ui, knobRow(a, y), knob, { { kLpfKeyFollow, "KeyTrk" }, { kLpfLfoAmount, "LFO Amt" } });
    }

    // --- ENV-F -----------------------------------------------------------------
    {
        const IRECT a = nui::Panel(ui, pal, row2[2], "ENV-F");
        nui::KnobRow(ui, knobRow(a, a.T), knob,
                     { { kEnvFAttack, "Attack" }, { kEnvFDecay, "Decay" },
                       { kEnvFSustain, "Sustain" }, { kEnvFRelease, "Release" } });
    }

    // --- ENV-A -----------------------------------------------------------------
    {
        const IRECT a = nui::Panel(ui, pal, row2[3], "ENV-A");
        nui::KnobRow(ui, knobRow(a, a.T), knob,
                     { { kEnvAAttack, "Attack" }, { kEnvADecay, "Decay" },
                       { kEnvASustain, "Sustain" }, { kEnvARelease, "Release" } });
    }

    // --- LFO -------------------------------------------------------------------
    {
        const IRECT a = nui::Panel(ui, pal, row3[0], "LFO");
        float y = a.T;
        nui::Switch(ui, switchRow(a, y), kLfoWave, "LFO Wave", sw);
        y += kSwStep;
        nui::KnobRow(ui, knobRow(a, y), knob,
                     { { kLfoRate, "Rate" }, { kLfoDelay, "Delay" },
                       { kLfoPitchAmount, "Pitch" }, { kLfoPwmAmount, "PWM" } });
    }

    // --- Poly-Mod --------------------------------------------------------------
    {
        const IRECT a = nui::Panel(ui, pal, row3[1], "POLY-MOD");
        nui::KnobRow(ui, knobRow(a, a.T), knob, { { kPmEnvFToOsc2, "EnvF>Osc2" }, { kPmEnvFToPw, "EnvF>PW" } });
    }

    // --- Voice -----------------------------------------------------------------
    {
        const IRECT a = nui::Panel(ui, pal, row3[2], "VOICE");
        float y = a.T;
        nui::Switch(ui, switchRow(a, y), kPolyphony, "Polyphony", sw);
        y += kSwStep;
        nui::Switch(ui, switchRow(a, y), kVoiceMode, "Voice Mode", sw);
        y += kSwStep;
        nui::KnobRow(ui, knobRow(a, y), knob,
                     { { kGlideTime, "Glide" }, { kBendRange, "Bend" },
                       { kVelToVca, "Vel>VCA" }, { kVelToFilter, "Vel>Filt" } });
    }

    // --- Stereo ----------------------------------------------------------------
    {
        const IRECT a = nui::Panel(ui, pal, row3[3], "STEREO");
        float y = a.T;
        nui::ToggleRow(ui, toggleRow(a, y), tog, { { kStereoMode, "Stereo" } });
        y += kTogStep;
        nui::KnobRow(ui, knobRow(a, y), knob, { { kStereoDetune, "Detune" }, { kStereoSpread, "Spread" } });
    }

    // --- Chorus + Output -------------------------------------------------------
    // The chorus lives HERE, at the top of the output panel, because that is
    // where it lives in the signal chain (DESIGN.md §1/§13: on the summed
    // accumulator, ahead of master volume) -- reading the panel top to bottom
    // reads the output stage in order. A 4-position tab switch, not a knob
    // and not four toggles: the instrument it models has two buttons whose
    // four states ARE these four positions, and there is nothing continuous
    // to dial.
    {
        const IRECT a = nui::Panel(ui, pal, row3[4], "CHORUS+OUTPUT");
        float y = a.T;
        nui::Switch(ui, switchRow(a, y), kChorus, "Chorus", sw);
        y += kSwStep;
        nui::ToggleRow(ui, toggleRow(a, y), tog, { { kOutputClip, "Clip" } });
        y += kTogStep;
        nui::KnobRow(ui, knobRow(a, y), knob, { { kMasterVolume, "Volume" } });
    }

    // --- Keyboard strip, full width, below the three panel rows -----------
    // G10.3: an instrument editor without a keyboard is untestable by ear
    // without external MIDI. Analogue's own control, outside the shared style.
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
    // and IControls.h includes IVKeyboardControl.h, so no extra include is
    // needed.
    //
    // Colours are passed explicitly rather than left at their defaults: the
    // stock keyboard is a generic white/black that sits badly against this
    // palette. Pressed keys use the accent, the colour of every knob's value
    // arc, so a held note reads as "lit" in the same visual language.
    //
    // KNOWN, Debug only: in a _DEBUG build IVKeyboardControl::Draw paints a
    // "key: .., vel: .." overlay with IText(20, COLOR_RED), i.e. in iPlug2's
    // DEFAULT_FONT "Roboto-Regular", which nothing loads. NanoVG's
    // PrepareAndMeasureText then asserts ("No font found"), the assert's modal
    // dialog blocks the first paint, and the Debug editor stays white and does
    // not close. Release builds are unaffected; this predates the shared style.
    const float kbY = y3 + row3H + nui::kRowGap;  // just below row 3
    const IRECT kbRect(x0, kbY, x1, kbY + kKeyboardH);
    constexpr int kKeyboardMinNote = 36;  // C2
    constexpr int kKeyboardMaxNote = 96;  // C7 -- 5 octaves
    const IColor kKeyWhite(255, 206, 198, 186);  // warm off-white, a touch below text
    const IColor kKeyBlack(255, 22, 20, 18);     // slightly darker than bg
    const IColor kKeyPressed = pal.accent;
    const IColor kKeyFrame = pal.panel;
    const IColor kKeyHover(255, 236, 190, 130);  // lifted accent
    ui.AttachControl(new IVKeyboardControl(kbRect, kKeyboardMinNote, kKeyboardMaxNote,
                                           /*roundedKeys=*/false, kKeyWhite, kKeyBlack,
                                           kKeyPressed, kKeyFrame, kKeyHover));

    // G10.1: every param bound by exactly one control (UI_STYLE.md §6). It
    // runs after the keyboard so that it sees every control the editor has.
#ifndef NDEBUG
    nui::CheckBindings(ui, kNumParams, "NassauAnalogue");
#endif
}

#endif // IPLUG_EDITOR
