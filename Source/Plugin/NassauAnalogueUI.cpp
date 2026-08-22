// G0: placeholder IGraphics editor for NassauAnalogue — just the two params
// wired at this gate (kMasterVolume, kOutputClip) plus a title and the
// factory preset selector. The full 53-param, signal-chain-grouped editor
// (DESIGN.md §1/§11, docs/GATES.md G10.2) is a G10 deliverable; G0's job is
// only to prove the shape compiles and links (G0.3/G0.4), including headless.
//
// The ENTIRE body is wrapped in #if IPLUG_EDITOR so that a headless build
// (IPLUG_EDITOR == 0, e.g. NASSAU_FORCE_HEADLESS) compiles this file to
// nothing and needs no IGraphics include paths (G0.3).
#if IPLUG_EDITOR

#include "NassauAnalogueUI.h"
#include "NassauAnaloguePlugin.h"
#include "IGraphics.h"
#include "IControls.h"

#include <cassert>

using namespace iplug;
using namespace iplug::igraphics;

void NassauAnalogueUI::Layout(IGraphics& ui, NassauAnaloguePlugin& /*plugin*/)
{
    // Register the UI font under the id "default", by FAMILY NAME, on EVERY
    // editor open, from a per-platform candidate list, with no `static bool`
    // guard. Both halves of this matter and both are documented failure
    // modes from nassau-zermatt's own G8 (see
    // nassau-zermatt/Source/Plugin/NassauZermattUI.cpp's comment block,
    // which this is copied from verbatim, and docs/GATES.md G10.5, which
    // exists specifically because of it):
    //   1. A macOS-only family name (e.g. "Helvetica") does not exist on
    //      Windows -- LoadFont returns false and every IText below draws
    //      nothing.
    //   2. IGraphics is destroyed and recreated every time the editor is
    //      opened, taking its font storage with it -- a process-lifetime
    //      `static bool` guard would make every editor open AFTER the first
    //      silently skip loading and render no text at all.
    static const char* const kFontFamilies[] = {
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

    // Dark, minimal analogue-synth palette (G10 will develop this properly).
    const IColor kBg(255, 20, 22, 26);
    const IColor kPanel(255, 32, 34, 40);
    const IColor kText(255, 220, 224, 230);
    const IColor kAccent(255, 120, 180, 220);

    const IText header(16, kText, "default", EAlign::Near);

    const IVStyle knobStyle = DEFAULT_STYLE
        .WithColor(kFG, kAccent)
        .WithLabelText(IText(10, kText, "default"))
        .WithValueText(IText(9, kText, "default"))
        .WithDrawFrame(false)
        .WithShowValue(true);

    const IVStyle toggleStyle = DEFAULT_STYLE
        .WithColor(kFG, kAccent)
        .WithValueText(IText(10, kText, "default"))
        .WithLabelText(IText(9, kText, "default"));

    const IVStyle presetStyle = DEFAULT_STYLE
        .WithDrawShadows(false)
        .WithColor(kFG, kAccent)
        .WithColor(kBG, kPanel)
        .WithLabelText(IText(10, kText, "default", EAlign::Center, EVAlign::Middle))
        .WithValueText(IText(10, kText, "default"));

    ui.AttachPanelBackground(kBg);

    ui.AttachControl(new ITextControl(
        IRECT(8.f, 8.f, 200.f, 32.f), "NassauAnalogue", header));

    // Factory ("baked") preset manager: no presets exist yet at G0 (G9 lands
    // the 12-preset bank, NassauAnaloguePlugin.h's kNumPresets note), but the
    // control itself is harmless to attach early — it will simply show an
    // empty list until then.
    ui.AttachControl(new IVBakedPresetManagerControl(
        IRECT(210.f, 8.f, static_cast<float>(PLUG_WIDTH) - 8.f, 32.f), "", presetStyle));

    ui.AttachControl(new IPanelControl(
        IRECT(8.f, 40.f, static_cast<float>(PLUG_WIDTH) - 8.f,
              static_cast<float>(PLUG_HEIGHT) - 8.f), kPanel));

    ui.AttachControl(new IVKnobControl(
        IRECT(24.f, 56.f, 124.f, 156.f), kMasterVolume, "Volume", knobStyle));

    ui.AttachControl(new IVToggleControl(
        IRECT(150.f, 90.f, 230.f, 130.f), kOutputClip, "Clip", toggleStyle));
}

#endif // IPLUG_EDITOR
